#include <WiFi.h>
#include <esp_now.h>
#include <stdarg.h>

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "shared/config.h"
#include "../shared/domain/MacUtils.h"
#include "domain/TransmitterManager.h"
#include "domain/SlotManager.h"
#include "domain/SlotManager.cpp"  // Force compilation of SlotManager
#include "infrastructure/EspNowTransport.h"
#include "infrastructure/Persistence.h"
#include "infrastructure/LEDService.h"
#include "../shared/infrastructure/DebugService.h"
#include "application/PairingService.h"
#include "application/KeyboardService.h"

// Domain layer instances
TransmitterManager transmitterManager;
ReceiverEspNowTransport transport;
LEDService ledService;

// Application layer instances
ReceiverPairingService pairingService;
KeyboardService keyboardService;

// System state
unsigned long bootTime = 0;
int cachedSlotsUsed = 0;  // Cache slot calculation to avoid recalculating every loop
unsigned long lastSlotCalculationTime = 0;
#define SLOT_CALCULATION_CACHE_MS 100  // Recalculate slots max once per 100ms

// Heartbeat state
unsigned long lastHeartbeatTime = 0;
#define HEARTBEAT_INTERVAL_MS 60000  // 1 minute

// Invalidate slot cache when transmitters change
static void invalidateSlotCache() {
  lastSlotCalculationTime = 0;  // Force immediate recalculation
}

// Forward declaration
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);

// Wrapper functions for DebugService (receiver uses ReceiverEspNowTransport, not EspNowTransport)
// Wrapper functions for DebugService (receiver uses ReceiverEspNowTransport, not EspNowTransport)
// These are needed because DebugService expects EspNowTransport, but receiver uses ReceiverEspNowTransport
static bool receiverEspNowTransport_send_wrapper(const uint8_t* mac, const uint8_t* data, int len) {
  return receiverEspNowTransport_send(&transport, mac, data, len);
}

static bool receiverEspNowTransport_addPeer_wrapper(const uint8_t* mac, uint8_t channel) {
  return receiverEspNowTransport_addPeer(&transport, mac, channel);
}

static bool receiverEspNowTransport_isInitialized_wrapper() {
  return transport.initialized;
}

// Wrapper function for debug callback
void pairingServiceDebugCallback(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  DebugService::print("%s", buffer);
}

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 2) return;  // Need at least deviceType + msgType
  
  uint8_t deviceType = data[0];  // First byte is deviceType
  uint8_t msgType = data[1];     // Second byte is msgType
  
  // Handle debug monitor discovery request FIRST (before filtering by device type)
  // This allows debug monitors to respond to MSG_ONLINE broadcasts
  if (msgType == MSG_DEBUG_MONITOR_REQ && len >= sizeof(debug_monitor_req_message)) {
    debug_monitor_req_message* req = (debug_monitor_req_message*)data;
    // Only process if it's from a debug monitor (not from another receiver/transmitter)
    if (req->deviceType == DEVICE_TYPE_DEBUG_MONITOR) {
      // handleDebugMonitorDiscovery will only print if this is a new discovery
      DebugService::handleDebugMonitorDiscovery(senderMAC);
    }
    return;
  }
  
  // Only process messages from transmitters (ignore messages from receivers and debug monitors)
  if (deviceType != DEVICE_TYPE_TRANSMITTER) {
    return;  // Ignore messages from non-transmitters
  }
  
  // Handle online message from transmitters (only when transmitter comes online, not as response to MSG_PAIRING_CONFIRMED)
  if (len >= sizeof(online_message)) {
    online_message* onlineMsg = (online_message*)data;
    // Only process if it's from a transmitter (check deviceType)
    if (onlineMsg->deviceType == DEVICE_TYPE_TRANSMITTER && onlineMsg->msgType == MSG_ONLINE) {
      int index = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (index >= 0) {
        DebugService::print("Received MSG_ONLINE from known transmitter %d", index);
      } else {
        DebugService::print( "Received MSG_ONLINE from unknown transmitter");
      }
      receiverPairingService_handleTransmitterOnline(&pairingService, senderMAC, channel);
      return;
    }
  }
  
  // Handle pairing confirmed message from transmitter (requesting reconnection after deep sleep)
  if (len >= sizeof(pairing_confirmed_message)) {
    pairing_confirmed_message* confirm = (pairing_confirmed_message*)data;
    // Only process if it's from a transmitter (check deviceType)
    if (confirm->deviceType == DEVICE_TYPE_TRANSMITTER && confirm->msgType == MSG_PAIRING_CONFIRMED) {
      int transmitterIndex = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (transmitterIndex >= 0) {
        // Known transmitter requesting reconnection - check if we can accept it
        // ALWAYS check slot availability, even for currently paired transmitters
        // This prevents over-allocation if slots have been reassigned (e.g., receiver restarted and accepted new transmitters)
        int slotsNeeded = getSlotsNeeded(transmitterManager.transmitters[transmitterIndex].pedalMode);
        bool isCurrentlyPaired = transmitterManager.transmitters[transmitterIndex].seenOnBoot;
        
        SlotAvailabilityResult result = slotManager_checkReconnection(&transmitterManager, transmitterIndex, slotsNeeded);
        bool shouldRespond = false;
        
        if (result.canFit) {
          shouldRespond = true;
          if (isCurrentlyPaired) {
            DebugService::print( "Known transmitter %d (currently paired) requesting reconnection - slots available, sending MSG_PAIRING_CONFIRMED_ACK", transmitterIndex);
          } else {
            DebugService::print( "Known transmitter %d (not currently paired) requesting reconnection - slots available, sending MSG_PAIRING_CONFIRMED_ACK", transmitterIndex);
          }
        } else {
          // Not enough slots - refuse pairing (transmitter will delete receiver from NVS on timeout)
          DebugService::print( "Known transmitter %d requesting reconnection - slots full (%d + %d > %d), refusing (transmitter will delete pairing)", 
                           transmitterIndex, result.currentSlotsUsed, slotsNeeded, MAX_PEDAL_SLOTS);
        }
        
        if (shouldRespond) {
          // Send MSG_PAIRING_CONFIRMED_ACK to acknowledge the reconnection request and confirm pairing
          receiverEspNowTransport_addPeer(&transport, senderMAC, channel);
          pairing_confirmed_ack_message ackMsg;
          ackMsg.deviceType = DEVICE_TYPE_RECEIVER;
          ackMsg.msgType = MSG_PAIRING_CONFIRMED_ACK;
          WiFi.macAddress(ackMsg.receiverMAC);
          
          bool sent = receiverEspNowTransport_send(&transport, senderMAC, (uint8_t*)&ackMsg, sizeof(ackMsg));
          if (sent) {
            // Mark as seen after successful send
            transmitterManager.transmitters[transmitterIndex].seenOnBoot = true;
            transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
            invalidateSlotCache();
            DebugService::print( "Sent MSG_PAIRING_CONFIRMED_ACK to known transmitter %d (reconnection accepted)", transmitterIndex);
          }
        } else {
          // Just update last seen time even if we can't accept
          transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
        }
      } else {
        DebugService::print( "Received MSG_PAIRING_CONFIRMED from unknown transmitter");
      }
      return;  // Don't process further - this is handled
    }
  }
  
  // Handle pairing confirmed acknowledgment from transmitter (acknowledgment that it received our MSG_PAIRING_CONFIRMED)
  if (len >= sizeof(pairing_confirmed_ack_message)) {
    pairing_confirmed_ack_message* ack = (pairing_confirmed_ack_message*)data;
    // Only process if it's from a transmitter (check deviceType)
    if (ack->deviceType == DEVICE_TYPE_TRANSMITTER && ack->msgType == MSG_PAIRING_CONFIRMED_ACK) {
      int transmitterIndex = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (transmitterIndex >= 0) {
        // Known transmitter acknowledging our MSG_PAIRING_CONFIRMED - mark as seen
        if (!transmitterManager.transmitters[transmitterIndex].seenOnBoot) {
          transmitterManager.transmitters[transmitterIndex].seenOnBoot = true;
          transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
          DebugService::print( "Known transmitter %d acknowledged MSG_PAIRING_CONFIRMED - marking as paired", transmitterIndex);
          invalidateSlotCache();  // Invalidate cache when transmitter becomes responsive
        } else {
          // Already marked as seen - just update last seen time
          transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
        }
      } else {
        DebugService::print( "Received MSG_PAIRING_CONFIRMED_ACK from unknown transmitter");
      }
      return;  // Don't process further - this is handled
    }
  }
  
  // Handle transmitter paired broadcast
  if (len >= sizeof(transmitter_paired_message)) {
    transmitter_paired_message* pairedMsg = (transmitter_paired_message*)data;
    if (pairedMsg->msgType == MSG_TRANSMITTER_PAIRED) {
      DebugService::print( "Received MSG_TRANSMITTER_PAIRED");
      receiverPairingService_handleTransmitterPaired(&pairingService, pairedMsg);
      return;
    }
  }
  
  // Handle standard messages
  if (len < sizeof(struct_message)) return;
  
  struct_message* msg = (struct_message*)data;
  
  switch (msg->msgType) {
    case MSG_DELETE_RECORD: {
      int index = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (index >= 0) {
        DebugService::print( "Received delete record request from transmitter %d - removing", index);
        transmitterManager_remove(&transmitterManager, index);
        persistence_save(&transmitterManager);
        invalidateSlotCache();  // Invalidate cache when transmitter removed
      }
      break;
    }
    
    case MSG_DISCOVERY_REQ: {
      DebugService::print( "Discovery request from %02X:%02X:%02X:%02X:%02X:%02X (mode=%d)",
                         senderMAC[0], senderMAC[1], senderMAC[2], senderMAC[3], senderMAC[4], senderMAC[5], msg->pedalMode);
      receiverPairingService_handleDiscoveryRequest(&pairingService, senderMAC, msg->pedalMode, channel, millis());
      persistence_save(&transmitterManager);
      invalidateSlotCache();  // Invalidate cache when transmitter added/modified
      break;
    }
    
    case MSG_PEDAL_EVENT: {
      int transmitterIndex = transmitterManager_findIndex(&transmitterManager, senderMAC);
      
      // If transmitter is unknown and we're in grace period, request discovery
      if (transmitterIndex < 0) {
        unsigned long currentTime = millis();
        unsigned long timeSinceBoot = currentTime - bootTime;
        bool inGracePeriod = (timeSinceBoot < TRANSMITTER_TIMEOUT);
        
        if (inGracePeriod && !pairingService.gracePeriodSkipped) {
          // Unknown transmitter sending pedal events during grace period - request discovery
          receiverEspNowTransport_addPeer(&transport, senderMAC, channel);
          struct_message alive = {DEVICE_TYPE_RECEIVER, MSG_ALIVE, 0, false, 0};
          receiverEspNowTransport_send(&transport, senderMAC, (uint8_t*)&alive, sizeof(alive));
          
          DebugService::print( "Unknown transmitter sent pedal event during grace period - requesting discovery");
        }
      } else {
        // Known transmitter - mark as seen (it's responding after receiving MSG_PAIRING_CONFIRMED)
        if (!transmitterManager.transmitters[transmitterIndex].seenOnBoot) {
          transmitterManager.transmitters[transmitterIndex].seenOnBoot = true;
          transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
          DebugService::print( "Known transmitter %d responded with pedal event - marking as paired", transmitterIndex);
        }
        
        // Handle pedal event normally
        char keyToPress;
        if (transmitterManager.transmitters[transmitterIndex].pedalMode == 0) {
          keyToPress = (msg->key == '1') ? 'l' : 'r';
        } else {
          keyToPress = transmitterManager_getAssignedKey(&transmitterManager, transmitterIndex);
        }
        // Use standardized pedal event format: T%d: '%c' ▼/▲
        DebugService::print( "T%d: '%c' %s", 
                          transmitterIndex, keyToPress, msg->pressed ? "▼" : "▲");
      }
      
      keyboardService_handlePedalEvent(&keyboardService, senderMAC, msg);
      break;
    }
    
    case MSG_ALIVE: {
      receiverPairingService_handleAlive(&pairingService, senderMAC);
      break;
    }
  }
}

void setup() {
  bootTime = millis();
  
  // Check if boot button is pressed (GPIO 0, typically pulled HIGH, LOW when pressed)
  // Wait a bit to allow button press to register
  delay(100);
  pinMode(0, INPUT_PULLUP);
  delay(50);
  
  // Initialize domain layer
  transmitterManager_init(&transmitterManager);
  
  // Initialize infrastructure layer first (needed for debug monitor)
  receiverEspNowTransport_init(&transport);
  
  // Initialize DebugService with function pointers for receiver transport
  DebugService::init(
    receiverEspNowTransport_send_wrapper,
    receiverEspNowTransport_addPeer_wrapper,
    receiverEspNowTransport_isInitialized_wrapper
  );
  DebugService::setDeviceType(DEVICE_TYPE_RECEIVER);  // Set device type to receiver
  DebugService::setEnabled(true);
  
  // Load debug monitor pairing from persistence and notify DebugService
  uint8_t savedMonitorMAC[6] = {0};
  bool monitorPaired = false;
  persistence_loadDebugMonitor(savedMonitorMAC, &monitorPaired);
  if (monitorPaired && !macIsZero(savedMonitorMAC)) {
    DebugService::handleDebugMonitorDiscovery(savedMonitorMAC);
  }
  
  // Check if boot button is initially pressed
  bool buttonInitiallyPressed = (digitalRead(0) == LOW);
  
  // If button is initially pressed, wait 2 seconds and check if still held
  bool bootButtonHeldFor2s = false;
  if (buttonInitiallyPressed) {
    unsigned long buttonCheckStart = millis();
    const unsigned long HOLD_TIME_MS = 2000;  // 2 seconds
    
    // Blink LED rapidly while waiting (if available)
    #ifdef LED_PIN
    if (LED_PIN != 255) {
      pinMode(LED_PIN, OUTPUT);
    }
    #endif
    
    // Check button state continuously for 2 seconds
    while ((millis() - buttonCheckStart) < HOLD_TIME_MS) {
      // Blink LED rapidly to indicate waiting for hold
      #ifdef LED_PIN
      if (LED_PIN != 255) {
        digitalWrite(LED_PIN, HIGH);
        delay(50);
        digitalWrite(LED_PIN, LOW);
        delay(50);
      }
      #endif
      
      // Check if button is still pressed
      if (digitalRead(0) != LOW) {
        // Button released before 2 seconds - abort
        bootButtonHeldFor2s = false;
        break;
      }
    }
    
    // If we got here and button is still pressed, it was held for 2 seconds
    if ((millis() - buttonCheckStart) >= HOLD_TIME_MS && digitalRead(0) == LOW) {
      bootButtonHeldFor2s = true;
    }
  }
  
  // If boot button was held for 2 seconds, clear all stored MAC addresses
  if (bootButtonHeldFor2s) {
    // Clear all stored transmitter MAC addresses (but preserve debug monitor pairing)
    persistence_clear();
    transmitterManager.count = 0;
    transmitterManager.slotsUsed = 0;
    
    // Note: We preserve debug monitor pairing so debug messages continue to work
    // If you want to clear debug monitor too, uncomment the lines below:
    
    // Reset grace period state to re-enter grace period
    pairingService.gracePeriodCheckDone = false;
    pairingService.gracePeriodSkipped = false;
    pairingService.initialPingSent = false;
    pairingService.initialPingTime = 0;
    pairingService.slotReassignmentDone = false;
    pairingService.bootTime = millis();  // Reset boot time so grace period starts fresh
    
    // Reset LED service boot time to match
    ledService.bootTime = pairingService.bootTime;
    
          DebugService::print( "Boot button held for 2s - cleared all stored MAC addresses");
          DebugService::print( "Receiver will start fresh with no known transmitters");
          DebugService::print( "Grace period restarted - actively seeking new transmitters");
    
    // Blink LED 5 times to indicate clear operation completed
    #ifdef LED_PIN
    if (LED_PIN != 255) {
      for (int i = 0; i < 5; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(200);
        digitalWrite(LED_PIN, LOW);
        delay(200);
      }
    }
    #endif
    
    delay(500);  // Give time to see the message
  } else {
    // Load persisted state only if boot button was NOT held for 2 seconds
    persistence_load(&transmitterManager);
  }
  
  ledService_init(&ledService, bootTime);
  
  // Initialize application layer
  receiverPairingService_init(&pairingService, &transmitterManager, &transport, bootTime);
  receiverPairingService_setDebugCallback(&pairingService, pairingServiceDebugCallback);
  keyboardService_init(&keyboardService, &transmitterManager);
  
  // Register message callback (must be before adding peers)
  receiverEspNowTransport_registerReceiveCallback(&transport, onMessageReceived);
  
  // Add broadcast peer
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  receiverEspNowTransport_addPeer(&transport, broadcastMAC, 0);
  
  // Add saved transmitters as peers
  for (int i = 0; i < transmitterManager.count; i++) {
    receiverEspNowTransport_addPeer(&transport, transmitterManager.transmitters[i].mac, 0);
  }
  
  // DebugService will automatically send buffered messages when debug monitor is discovered
  // Send debug messages now that ESP-NOW is fully initialized
  // Note: print() will automatically call sendBufferedMessages() if transport is ready and monitor is known
  DebugService::print("ESP-NOW initialized");
  DebugService::print("Loaded %d transmitter(s) from EEPROM", transmitterManager.count);
  // Show slots used based on responsive transmitters only (not stored slotsUsed)
  int responsiveSlots = transmitterManager_calculateSlotsUsed(&transmitterManager);
  DebugService::print("Pedal slots used: %d/%d (responsive transmitters only)", responsiveSlots, MAX_PEDAL_SLOTS);
  
  // Force sending buffered messages now that ESP-NOW is fully set up
  // This ensures messages are sent even if debug monitor was discovered before ESP-NOW was ready
  // We'll call print() with an empty message to trigger sendBufferedMessages() check
  DebugService::print("ESP-NOW setup complete");
  
  // Broadcast MSG_ONLINE to announce receiver is online (debug monitor will respond)
  online_message onlineMsg;
  onlineMsg.deviceType = DEVICE_TYPE_RECEIVER;
  onlineMsg.msgType = MSG_ONLINE;
  WiFi.macAddress(onlineMsg.deviceMAC);
  receiverEspNowTransport_broadcast(&transport, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
  
  // Ping known transmitters immediately on boot (before pairing/grace period)
  // This restores previous pairings if transmitters are still online
  receiverPairingService_pingKnownTransmittersOnBoot(&pairingService);
  
          DebugService::print( "=== Receiver Ready ===");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Check for runtime boot button press (hold for 2 seconds to clear)
  static unsigned long buttonPressStart = 0;
  static bool buttonWasPressed = false;
  static bool clearingInProgress = false;
  
  bool buttonCurrentlyPressed = (digitalRead(0) == LOW);
  
  if (buttonCurrentlyPressed && !buttonWasPressed) {
    // Button just pressed - start timer
    buttonPressStart = currentTime;
    buttonWasPressed = true;
    clearingInProgress = false;
  } else if (buttonCurrentlyPressed && buttonWasPressed && !clearingInProgress) {
    // Button still held - check if held for 2 seconds
    if (currentTime - buttonPressStart >= 2000) {
      // Button held for 2 seconds - clear all stored MAC addresses
      clearingInProgress = true;
      
      // Clear all stored transmitter MAC addresses (but preserve debug monitor pairing)
      persistence_clear();
      transmitterManager.count = 0;
      transmitterManager.slotsUsed = 0;
      
      // Clear all transmitter slots
      for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
        memset(transmitterManager.transmitters[i].mac, 0, 6);
        transmitterManager.transmitters[i].pedalMode = 0;
        transmitterManager.transmitters[i].seenOnBoot = false;
        transmitterManager.transmitters[i].lastSeen = 0;
      }
      
      invalidateSlotCache();
      
      // Reset grace period state to re-enter grace period
      // CRITICAL: Capture millis() once and use it for all timing resets
      // Use currentTime from the loop() function to ensure consistency
      unsigned long clearTime = currentTime;
      
      pairingService.gracePeriodCheckDone = false;
      pairingService.gracePeriodSkipped = false;
      pairingService.initialPingSent = false;
      pairingService.initialPingTime = 0;
      pairingService.slotReassignmentDone = false;
      pairingService.bootTime = clearTime;  // Reset boot time so grace period starts fresh
      
      // Reset LED service boot time to match
      ledService.bootTime = clearTime;
      
      // Ping known transmitters again (will be empty now, but resets state)
      // This will set initialPingSent=true and initialPingTime=millis()
      receiverPairingService_pingKnownTransmittersOnBoot(&pairingService);
      
      // CRITICAL FIX: Ensure initialPingTime is set relative to the new bootTime
      // The ping function sets initialPingTime = millis(), but we need it relative to bootTime
      // Since we just reset bootTime to clearTime, set initialPingTime to be close to bootTime
      if (pairingService.initialPingSent) {
        // Use the same clearTime (or very close) for initialPingTime
        // This ensures timeSinceBoot and timeSincePing are calculated correctly
        pairingService.initialPingTime = clearTime + 10;  // 10ms offset for ping send time
      }
      
      // Broadcast MSG_ONLINE to announce receiver is ready for pairing (debug monitor will respond)
      online_message onlineMsg;
      onlineMsg.deviceType = DEVICE_TYPE_RECEIVER;
      onlineMsg.msgType = MSG_ONLINE;
      WiFi.macAddress(onlineMsg.deviceMAC);
      receiverEspNowTransport_broadcast(&transport, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
      
      DebugService::print( "Boot button held for 2s - cleared all stored MAC addresses");
      DebugService::print( "Receiver will start fresh with no known transmitters");
      DebugService::print( "Grace period restarted - actively seeking new transmitters");
      DebugService::print( "DEBUG: bootTime reset to %lu, initialPingTime=%lu", 
                         pairingService.bootTime, pairingService.initialPingTime);
      
      // Blink LED 5 times to indicate clear operation completed
      #ifdef LED_PIN
      if (LED_PIN != 255) {
        for (int i = 0; i < 5; i++) {
          digitalWrite(LED_PIN, HIGH);
          delay(200);
          digitalWrite(LED_PIN, LOW);
          delay(200);
        }
      }
      #endif
    }
  } else if (!buttonCurrentlyPressed && buttonWasPressed) {
    // Button released - reset state
    buttonWasPressed = false;
    buttonPressStart = 0;
    clearingInProgress = false;
  }
  
  // Update pairing service (handles beacons, pings, replacement logic)
  receiverPairingService_update(&pairingService, currentTime);
  
  // Cache slot calculation - only recalculate periodically or when needed
  // This avoids expensive iteration every loop iteration
  if (currentTime - lastSlotCalculationTime >= SLOT_CALCULATION_CACHE_MS) {
    cachedSlotsUsed = transmitterManager_calculateSlotsUsed(&transmitterManager);
    lastSlotCalculationTime = currentTime;
  }
  
  // Update LED status - green during initial wait (1s after ping sent), blue during grace period, off otherwise
  bool inInitialWait = false;
  if (pairingService.initialPingSent && pairingService.initialPingTime > 0) {
    unsigned long timeSincePing = currentTime - pairingService.initialPingTime;
    inInitialWait = (timeSincePing < INITIAL_PING_WAIT_MS);
  }
  ledService_update(&ledService, currentTime, pairingService.gracePeriodCheckDone, cachedSlotsUsed, inInitialWait);
  
  // Send periodic heartbeat every minute with paired pedal count
  if (currentTime - lastHeartbeatTime >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatTime = currentTime;
    
    // Count paired transmitters (those with MAC addresses in slots)
    // Note: We count all transmitters with MAC addresses, not just those with seenOnBoot=true,
    // because a transmitter can be paired but not yet responded after coming back online
    int pairedCount = 0;
    for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
      bool slotOccupied = false;
      for (int j = 0; j < 6; j++) {
        if (transmitterManager.transmitters[i].mac[j] != 0) {
          slotOccupied = true;
          break;
        }
      }
      if (slotOccupied) {
        pairedCount++;
      }
    }
    
          DebugService::print( "Heartbeat: %d pedal(s) paired (%d/%d slots used)", 
                      pairedCount, cachedSlotsUsed, MAX_PEDAL_SLOTS);
  }
  
  // Adaptive delay: shorter during grace period (needs responsiveness), longer when idle
  if (pairingService.gracePeriodCheckDone) {
    // After grace period - use longer delay (50ms = 20Hz) since we're mostly idle
    delay(50);
  } else {
    // During grace period - use shorter delay (10ms = 100Hz) for responsiveness
    delay(10);
  }
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "domain/TransmitterManager.cpp"
#include "infrastructure/EspNowTransport.cpp"
#include "infrastructure/Persistence.cpp"
#include "infrastructure/LEDService.cpp"
#include "../shared/infrastructure/DebugService.cpp"
#include "shared/debug_format.cpp"
#include "application/PairingService.cpp"
#include "application/KeyboardService.cpp"

// Stub implementations for EspNowTransport functions (needed by DebugService but never called when using function pointers)
// These are only needed for linker satisfaction - the receiver always uses function pointers
#include "../shared/infrastructure/EspNowTransport.h"
bool espNowTransport_addPeer(EspNowTransport* transport, const uint8_t* mac, uint8_t channel) {
  (void)transport; (void)mac; (void)channel;
  return false;  // Never called when receiver uses function pointers
}
bool espNowTransport_send(EspNowTransport* transport, const uint8_t* mac, const uint8_t* data, int len) {
  (void)transport; (void)mac; (void)data; (void)len;
  return false;  // Never called when receiver uses function pointers
}
