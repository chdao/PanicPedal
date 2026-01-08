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

// Forward declaration
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);

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
  
  // Handle transmitter online broadcast
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
      }
      break;
    }
    
    case MSG_DISCOVERY_REQ: {
      debugMonitor_print(&debugMonitor, "Discovery request from %02X:%02X:%02X:%02X:%02X:%02X (mode=%d)",
                         senderMAC[0], senderMAC[1], senderMAC[2], senderMAC[3], senderMAC[4], senderMAC[5], msg->pedalMode);
      receiverPairingService_handleDiscoveryRequest(&pairingService, senderMAC, msg->pedalMode, channel, millis());
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
        debugMonitor_print(&debugMonitor, "Pedal event: transmitter %d, key '%c' %s", 
                          transmitterIndex, keyToPress, msg->pressed ? "PRESSED" : "RELEASED");
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
    debugMonitor_print(&debugMonitor, "Pedal slots used: %d/%d", transmitterManager.slotsUsed, MAX_PEDAL_SLOTS);
  }
  
  debugMonitor_print(&debugMonitor, "=== Receiver Ready ===");
}

void loop() {
  unsigned long currentTime = millis();
  
  // Update pairing service (handles beacons, pings, replacement logic)
  receiverPairingService_update(&pairingService, currentTime);
  
  // Update LED status
  ledService_update(&ledService, currentTime);
  
  delay(10);
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "domain/TransmitterManager.cpp"
#include "infrastructure/EspNowTransport.cpp"
#include "infrastructure/Persistence.cpp"
#include "infrastructure/LEDService.cpp"
#include "infrastructure/DebugMonitor.cpp"
#include "application/PairingService.cpp"
#include "application/KeyboardService.cpp"
