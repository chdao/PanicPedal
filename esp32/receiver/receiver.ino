#include <WiFi.h>
#include <esp_now.h>
#include <stdarg.h>

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "shared/config.h"
#include "domain/TransmitterManager.h"
#include "domain/SlotManager.h"
#include "domain/SlotManager.cpp"  // Force compilation of SlotManager
#include "infrastructure/EspNowTransport.h"
#include "infrastructure/Persistence.h"
#include "infrastructure/LEDService.h"
#include "infrastructure/DebugMonitor.h"
#include "application/PairingService.h"
#include "application/KeyboardService.h"

// Domain layer instances
TransmitterManager transmitterManager;
ReceiverEspNowTransport transport;
LEDService ledService;
DebugMonitor debugMonitor;

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

// Wrapper function for debug callback
void pairingServiceDebugCallback(const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  debugMonitor_print(&debugMonitor, "%s", buffer);
}

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 1) return;
  
  uint8_t msgType = data[0];
  
  // Handle debug monitor discovery request
  if (msgType == MSG_DEBUG_MONITOR_REQ) {
    debugMonitor_handleDiscoveryRequest(&debugMonitor, senderMAC, channel);
    
    // Send immediate confirmation that pairing succeeded
    debugMonitor_print(&debugMonitor, "Debug monitor discovery request received and processed");
    return;
  }
  
  // Handle transmitter online broadcast (only when transmitter comes online, not as response to MSG_PAIRING_CONFIRMED)
  if (len >= sizeof(transmitter_online_message)) {
    transmitter_online_message* onlineMsg = (transmitter_online_message*)data;
    if (onlineMsg->msgType == MSG_TRANSMITTER_ONLINE) {
      int index = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (index >= 0) {
        debugMonitor_print(&debugMonitor, "Received MSG_TRANSMITTER_ONLINE from known transmitter %d", index);
      } else {
        debugMonitor_print(&debugMonitor, "Received MSG_TRANSMITTER_ONLINE from unknown transmitter");
      }
      receiverPairingService_handleTransmitterOnline(&pairingService, senderMAC, channel);
      return;
    }
  }
  
  // Handle pairing confirmed message from transmitter (acknowledgment that it received our MSG_PAIRING_CONFIRMED)
  if (len >= sizeof(pairing_confirmed_message)) {
    pairing_confirmed_message* confirm = (pairing_confirmed_message*)data;
    if (confirm->msgType == MSG_PAIRING_CONFIRMED) {
      int transmitterIndex = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (transmitterIndex >= 0) {
        // Known transmitter acknowledging our MSG_PAIRING_CONFIRMED - mark as seen
        if (!transmitterManager.transmitters[transmitterIndex].seenOnBoot) {
          transmitterManager.transmitters[transmitterIndex].seenOnBoot = true;
          transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
          debugMonitor_print(&debugMonitor, "Known transmitter %d acknowledged MSG_PAIRING_CONFIRMED - marking as paired", transmitterIndex);
          invalidateSlotCache();  // Invalidate cache when transmitter becomes responsive
        } else {
          // Already marked as seen - just update last seen time
          transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
        }
      } else {
        debugMonitor_print(&debugMonitor, "Received MSG_PAIRING_CONFIRMED from unknown transmitter");
      }
      return;  // Don't process further - this is handled
    }
  }
  
  // Handle transmitter paired broadcast
  if (len >= sizeof(transmitter_paired_message)) {
    transmitter_paired_message* pairedMsg = (transmitter_paired_message*)data;
    if (pairedMsg->msgType == MSG_TRANSMITTER_PAIRED) {
      debugMonitor_print(&debugMonitor, "Received MSG_TRANSMITTER_PAIRED");
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
        debugMonitor_print(&debugMonitor, "Received delete record request from transmitter %d - removing", index);
        transmitterManager_remove(&transmitterManager, index);
        persistence_save(&transmitterManager);
        invalidateSlotCache();  // Invalidate cache when transmitter removed
      }
      break;
    }
    
    case MSG_DISCOVERY_REQ: {
      debugMonitor_print(&debugMonitor, "Discovery request from %02X:%02X:%02X:%02X:%02X:%02X (mode=%d)",
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
          struct_message alive = {MSG_ALIVE, 0, false, 0};
          receiverEspNowTransport_send(&transport, senderMAC, (uint8_t*)&alive, sizeof(alive));
          
          debugMonitor_print(&debugMonitor, "Unknown transmitter sent pedal event during grace period - requesting discovery");
        }
      } else {
        // Known transmitter - mark as seen (it's responding after receiving MSG_PAIRING_CONFIRMED)
        if (!transmitterManager.transmitters[transmitterIndex].seenOnBoot) {
          transmitterManager.transmitters[transmitterIndex].seenOnBoot = true;
          transmitterManager.transmitters[transmitterIndex].lastSeen = millis();
          debugMonitor_print(&debugMonitor, "Known transmitter %d responded with pedal event - marking as paired", transmitterIndex);
        }
        
        // Handle pedal event normally
        char keyToPress;
        if (transmitterManager.transmitters[transmitterIndex].pedalMode == 0) {
          keyToPress = (msg->key == '1') ? 'l' : 'r';
        } else {
          keyToPress = transmitterManager_getAssignedKey(&transmitterManager, transmitterIndex);
        }
        // Use standardized pedal event format: T%d: '%c' ▼/▲
        debugMonitor_print(&debugMonitor, "T%d: '%c' %s", 
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
  
  // Initialize domain layer
  transmitterManager_init(&transmitterManager);
  
  // Initialize infrastructure layer first (needed for debug monitor)
  receiverEspNowTransport_init(&transport);
  debugMonitor_init(&debugMonitor, &transport, bootTime);
  debugMonitor_load(&debugMonitor);
  debugMonitor.espNowInitialized = true;
  
  // Load persisted state
  persistence_load(&transmitterManager);
  
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
  
  // Add saved debug monitor as peer (if it was saved)
  if (debugMonitor.paired) {
    receiverEspNowTransport_addPeer(&transport, debugMonitor.mac, 0);
    // Small delay to ensure peer is ready before sending messages
    delay(50);
    
    // Send debug messages now that ESP-NOW is fully initialized
    debugMonitor_print(&debugMonitor, "ESP-NOW initialized");
    debugMonitor_print(&debugMonitor, "Loaded %d transmitter(s) from EEPROM", transmitterManager.count);
    // Show slots used based on responsive transmitters only (not stored slotsUsed)
    int responsiveSlots = transmitterManager_calculateSlotsUsed(&transmitterManager);
    debugMonitor_print(&debugMonitor, "Pedal slots used: %d/%d (responsive transmitters only)", responsiveSlots, MAX_PEDAL_SLOTS);
  }
  
  // Ping known transmitters immediately on boot (before pairing/grace period)
  // This restores previous pairings if transmitters are still online
  receiverPairingService_pingKnownTransmittersOnBoot(&pairingService);
  
  debugMonitor_print(&debugMonitor, "=== Receiver Ready ===");
}

void loop() {
  unsigned long currentTime = millis();
  
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
    
    debugMonitor_print(&debugMonitor, "Heartbeat: %d pedal(s) paired (%d/%d slots used)", 
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
#include "shared/debug_format.cpp"
#include "infrastructure/DebugMonitor.cpp"
#include "application/PairingService.cpp"
#include "application/KeyboardService.cpp"
