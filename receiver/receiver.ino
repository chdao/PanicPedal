#include <WiFi.h>
#include <esp_now.h>

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "domain/TransmitterManager.h"
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

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void sendDebugMonitorStatus(DebugMonitor* monitor);

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 1) return;
  
  uint8_t msgType = data[0];
  
  // Handle debug monitor discovery request (legacy)
  if (msgType == MSG_DEBUG_MONITOR_REQ) {
    debugMonitor_handleDiscoveryRequest(&debugMonitor, senderMAC, channel);
    // debugMonitor_handleDiscoveryRequest already sends confirmation messages
    return;
  }
  
  // Handle debug monitor beacon (new proactive discovery)
  if (msgType == MSG_DEBUG_MONITOR_BEACON && len >= sizeof(debug_monitor_beacon_message)) {
    debug_monitor_beacon_message* beacon = (debug_monitor_beacon_message*)data;
    // Send status callback to provide full status when monitor connects/reconnects
    debugMonitor_handleBeacon(&debugMonitor, beacon->monitorMAC, channel, sendDebugMonitorStatus);
    return;
  }
  
  // Handle transmitter online broadcast
  if (len >= sizeof(transmitter_online_message)) {
    transmitter_online_message* onlineMsg = (transmitter_online_message*)data;
    if (onlineMsg->msgType == MSG_TRANSMITTER_ONLINE) {
      int index = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (index >= 0) {
        debugMonitor_print(&debugMonitor, "Transmitter %d came online - sending MSG_ALIVE", index);
      } else {
        if (transmitterManager.slotsUsed >= MAX_PEDAL_SLOTS) {
          debugMonitor_print(&debugMonitor, "Unknown transmitter came online (receiver full, checking for replacements)");
        } else {
          debugMonitor_print(&debugMonitor, "Unknown transmitter came online (receiver has free slots)");
        }
      }
      receiverPairingService_handleTransmitterOnline(&pairingService, senderMAC, channel);
      return;
    }
  }
  
  // Handle transmitter paired broadcast
  if (len >= sizeof(transmitter_paired_message)) {
    transmitter_paired_message* pairedMsg = (transmitter_paired_message*)data;
    if (pairedMsg->msgType == MSG_TRANSMITTER_PAIRED) {
      // Check if it's paired with us or another receiver
      uint8_t ourMAC[6];
      WiFi.macAddress(ourMAC);
      bool pairedWithUs = macEqual(pairedMsg->receiverMAC, ourMAC);
      int index = transmitterManager_findIndex(&transmitterManager, pairedMsg->transmitterMAC);
      
      // Only log if it's being removed (paired with different receiver)
      if (index >= 0 && !pairedWithUs) {
        debugMonitor_print(&debugMonitor, "Transmitter %d paired with another receiver - removing", index);
      }
      
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
      }
      break;
    }
    
    case MSG_DISCOVERY_REQ: {
      int indexBefore = transmitterManager_findIndex(&transmitterManager, senderMAC);
      bool isKnown = (indexBefore >= 0);
      int slotsBefore = transmitterManager.slotsUsed;
      
      debugMonitor_print(&debugMonitor, "Discovery request from %02X:%02X:%02X:%02X:%02X:%02X (mode=%d, known=%s)",
                         senderMAC[0], senderMAC[1], senderMAC[2], senderMAC[3], senderMAC[4], senderMAC[5], 
                         msg->pedalMode, isKnown ? "yes" : "no");
      
      receiverPairingService_handleDiscoveryRequest(&pairingService, senderMAC, msg->pedalMode, channel, millis());
      
      // Check if discovery was accepted
      int indexAfter = transmitterManager_findIndex(&transmitterManager, senderMAC);
      bool isNowPaired = (indexAfter >= 0);
      bool slotsIncreased = (transmitterManager.slotsUsed > slotsBefore);
      
      if (isKnown && isNowPaired) {
        // Known transmitter reconnected
        debugMonitor_print(&debugMonitor, "Transmitter %d reconnected (slots: %d/%d)", 
                          indexAfter, transmitterManager.slotsUsed, MAX_PEDAL_SLOTS);
      } else if (!isKnown && slotsIncreased && isNowPaired) {
        // New transmitter paired
        debugMonitor_print(&debugMonitor, "Transmitter %d paired successfully (slots: %d/%d)", 
                          indexAfter, transmitterManager.slotsUsed, MAX_PEDAL_SLOTS);
      } else if (transmitterManager.slotsUsed >= MAX_PEDAL_SLOTS) {
        // Rejected: receiver full
        debugMonitor_print(&debugMonitor, "Discovery rejected: receiver full (%d/%d slots)", 
                          transmitterManager.slotsUsed, MAX_PEDAL_SLOTS);
      } else if (!isNowPaired) {
        // Rejected: insufficient slots or grace period expired
        debugMonitor_print(&debugMonitor, "Discovery rejected: insufficient slots or grace period expired");
      }
      
      persistence_save(&transmitterManager);
      break;
    }
    
    case MSG_PEDAL_EVENT: {
      int transmitterIndex = transmitterManager_findIndex(&transmitterManager, senderMAC);
      if (transmitterIndex >= 0) {
        char keyToPress;
        if (transmitterManager.transmitters[transmitterIndex].pedalMode == 0) {
          keyToPress = (msg->key == '1') ? 'l' : 'r';
        } else {
          keyToPress = transmitterManager_getAssignedKey(&transmitterManager, transmitterIndex);
        }
        debugMonitor_print(&debugMonitor, "T%d: '%c' %s", 
                          transmitterIndex, keyToPress, msg->pressed ? "▼" : "▲");
      }
      keyboardService_handlePedalEvent(&keyboardService, senderMAC, msg);
      break;
    }
    
    case MSG_ALIVE: {
      // Handle alive message (silently - these are routine heartbeats)
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
  
  if (!transport.initialized) {
    Serial.println("ERROR: ESP-NOW initialization failed!");
    Serial.println("System will continue but communication may not work.");
    // Continue anyway - might recover on next boot
  }
  
  debugMonitor_init(&debugMonitor, &transport, bootTime);
  debugMonitor_load(&debugMonitor);
  debugMonitor.espNowInitialized = transport.initialized;
  
  // Load persisted state
  persistence_load(&transmitterManager);
  
  ledService_init(&ledService, bootTime);
  
  // Initialize application layer
  receiverPairingService_init(&pairingService, &transmitterManager, &transport, bootTime);
  keyboardService_init(&keyboardService, &transmitterManager);
  
  // Register message callback and add peers (only if ESP-NOW initialized)
  if (transport.initialized) {
    receiverEspNowTransport_registerReceiveCallback(&transport, onMessageReceived);
    
    // Add broadcast peer
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    receiverEspNowTransport_addPeer(&transport, broadcastMAC, 0);
    
    // Add saved transmitters as peers
    for (int i = 0; i < transmitterManager.count; i++) {
      receiverEspNowTransport_addPeer(&transport, transmitterManager.transmitters[i].mac, 0);
    }
    
    // Add saved debug monitor as peer (if it was saved) - must be done before sending messages
    if (debugMonitor.paired) {
      receiverEspNowTransport_addPeer(&transport, debugMonitor.mac, 0);
      delay(DEBUG_MONITOR_PEER_READY_DELAY_MS);
    }
  }
  
  // Don't send status messages here - they will be sent when the monitor's beacon is received
  // This ensures we only send once when the monitor actually connects, not on every boot
}

// Send full status to debug monitor (used when monitor connects/reconnects)
void sendDebugMonitorStatus(DebugMonitor* monitor) {
  if (!monitor->paired || !monitor->espNowInitialized) return;
  if (monitor->statusSent) return;  // Already sent, don't send again
  
  debugMonitor_print(monitor, "ESP-NOW initialized");
  debugMonitor_print(monitor, "Loaded %d transmitter(s) from storage", transmitterManager.count);
  debugMonitor_print(monitor, "Pedal slots used: %d/%d", transmitterManager.slotsUsed, MAX_PEDAL_SLOTS);
  debugMonitor_print(monitor, "=== Receiver Ready ===");
  
  monitor->statusSent = true;  // Mark as sent
}

void loop() {
  unsigned long currentTime = millis();
  
  // Update pairing service (handles beacons, pings, replacement logic)
  receiverPairingService_update(&pairingService, currentTime);
  
  // Update debug monitor (checks for beacons, manages connection)
  debugMonitor_update(&debugMonitor, currentTime);
  
  // Update LED status
  ledService_update(&ledService, currentTime);
  
  #define LOOP_DELAY_MS 10
  delay(LOOP_DELAY_MS);
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "domain/TransmitterManager.cpp"
#include "infrastructure/EspNowTransport.cpp"
#include "infrastructure/Persistence.cpp"
#include "infrastructure/LEDService.cpp"
#include "infrastructure/DebugMonitor.cpp"
#include "application/PairingService.cpp"
#include "application/KeyboardService.cpp"
