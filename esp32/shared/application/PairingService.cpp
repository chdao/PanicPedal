#include "PairingService.h"
#include <WiFi.h>
#include <string.h>
#include <Arduino.h>
#include "../messages.h"
// Note: PairingState.h and EspNowTransport.h must be included before this file
// They are included by each project's .ino file

// Forward declarations for utility functions
bool isValidMAC(const uint8_t* mac);
bool macEqual(const uint8_t* mac1, const uint8_t* mac2);
void macCopy(uint8_t* dest, const uint8_t* src);
int getSlotsNeeded(uint8_t pedalMode);

// Forward declaration for debug function (defined in transmitter.ino)
extern void debugPrint(const char* format, ...);
extern bool debugEnabled;  // Runtime debug flag (can be checked for conditional logic)

// Helper function to format MAC address (only when debug enabled, power optimized)
static inline const char* formatMAC(const uint8_t* mac) {
  static char macStr[18];  // Reuse static buffer to avoid stack allocations
  if (mac) {
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    macStr[0] = '\0';
  }
  return macStr;
}

void pairingService_init(PairingService* service, PairingState* state, EspNowTransport* transport, uint8_t pedalMode, unsigned long bootTime) {
  service->pairingState = state;
  service->transport = transport;
  service->pedalMode = pedalMode;
  service->bootTime = bootTime;
  service->onPaired = nullptr;
  service->hasPendingDiscovery = false;
  memset(service->pendingDiscoveryMAC, 0, 6);
  service->pendingDiscoveryChannel = 0;
}

void pairingService_handleBeacon(PairingService* service, const uint8_t* senderMAC, const beacon_message* beacon) {
  // Validate MAC addresses
  if (!isValidMAC(senderMAC) || !isValidMAC(beacon->receiverMAC)) {
    return;  // Invalid MAC addresses, ignore beacon
  }
  
  int slotsNeeded = getSlotsNeeded(service->pedalMode);
  
  // Check if this beacon is from a previously paired receiver (matches pairedReceiverMAC)
  // This works even if currently paired (for reconnection) or if pairing was lost
  bool isPreviouslyPaired = macEqual(beacon->receiverMAC, service->pairingState->pairedReceiverMAC) &&
                            !macEqual(service->pairingState->pairedReceiverMAC, (uint8_t[6]){0,0,0,0,0,0});
  
  if (beacon->availableSlots >= slotsNeeded) {
    // Beacons don't include channel info, use 0 (ESP-NOW will use current WiFi channel)
    pairingState_setDiscoveredReceiver(service->pairingState, beacon->receiverMAC, beacon->availableSlots, 0);
    
    // If this is a previously paired receiver, automatically send discovery request
    // This handles both reconnection scenarios and cases where pairing was lost
    if (isPreviouslyPaired && !pairingState_isPaired(service->pairingState)) {
      if (debugEnabled) {
        debugPrint("Beacon from previously paired receiver: %s - sending discovery request", formatMAC(beacon->receiverMAC));
      }
      
      // Automatically initiate pairing with previously paired receiver
      pairingService_initiatePairing(service, beacon->receiverMAC, 0);
    }
  } else {
    pairingState_clearDiscoveredReceiver(service->pairingState);
  }
}

void pairingService_handleDiscoveryResponse(PairingService* service, const uint8_t* senderMAC, uint8_t channel) {
  if (debugEnabled) {
    debugPrint("Received MSG_DISCOVERY_RESP from receiver: %s (waiting=%s)", 
               formatMAC(senderMAC), service->pairingState->waitingForDiscoveryResponse ? "true" : "false");
  }
  
  if (!service->pairingState->waitingForDiscoveryResponse) {
    if (debugEnabled) {
      debugPrint("Ignoring discovery response - not waiting for one");
    }
    return;  // Not waiting for response
  }
  
  if (debugEnabled) {
    debugPrint("Processing discovery response - pairing with receiver");
  }
  
  pairingState_setPaired(service->pairingState, senderMAC);
  espNowTransport_addPeer(service->transport, senderMAC, channel);
  
  // Clear waiting flag since we're now paired
  service->pairingState->waitingForDiscoveryResponse = false;
  service->pairingState->discoveryRequestTime = 0;
  
  pairingService_broadcastPaired(service, senderMAC);
  
  if (service->onPaired) {
    service->onPaired(senderMAC);
  }
}

void pairingService_handleAlive(PairingService* service, const uint8_t* senderMAC, uint8_t channel) {
  if (debugEnabled) {
    debugPrint("Handling MSG_ALIVE from receiver: %s (channel=%d)", formatMAC(senderMAC), channel);
  }
  
  // Check if we're currently paired
  bool isCurrentlyPaired = pairingState_isPaired(service->pairingState);
  
  if (debugEnabled) {
    debugPrint("Currently paired: %s", isCurrentlyPaired ? "true" : "false");
  }
  
  if (isCurrentlyPaired) {
    // Check if sender is our paired receiver
    bool isPairedReceiver = macEqual(senderMAC, service->pairingState->pairedReceiverMAC);
    
    if (isPairedReceiver) {
      // Already paired to this receiver - defer discovery request to main loop
      // (can't send ESP-NOW messages from within ESP-NOW callback)
      if (debugEnabled) {
        debugPrint("MSG_ALIVE from paired receiver: %s - deferring discovery request", formatMAC(senderMAC));
      }
      
      // Defer discovery request to main loop
      service->hasPendingDiscovery = true;
      macCopy(service->pendingDiscoveryMAC, senderMAC);
      service->pendingDiscoveryChannel = channel;
      
      return;
    } else {
      // Paired to a different receiver - send DELETE_RECORD to tell this receiver to remove us from its list
      if (debugEnabled) {
        debugPrint("MSG_ALIVE from different receiver (%s) - we're paired to %s - sending DELETE_RECORD", 
                   formatMAC(senderMAC), formatMAC(service->pairingState->pairedReceiverMAC));
      }
      
      espNowTransport_addPeer(service->transport, senderMAC, channel);
      struct_message deleteMsg = {MSG_DELETE_RECORD, 0, false, 0};
      bool sent = espNowTransport_send(service->transport, senderMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
      
      if (debugEnabled) {
        debugPrint("DELETE_RECORD %s to different receiver", sent ? "sent successfully" : "send FAILED");
      }
      return;
    }
  }
  
  // Not currently paired - MSG_ALIVE is a directed request for discovery
  // Automatically send discovery request (don't wait for pedal press)
  if (debugEnabled) {
    debugPrint("MSG_ALIVE from receiver (not paired): %s - sending discovery request automatically", formatMAC(senderMAC));
  }
  
  // Store receiver info and defer discovery request to main loop
  // (can't send ESP-NOW messages from within ESP-NOW callback)
  pairingState_setDiscoveredReceiver(service->pairingState, senderMAC, 2, channel);
  
  // Defer discovery request to main loop
  service->hasPendingDiscovery = true;
  macCopy(service->pendingDiscoveryMAC, senderMAC);
  service->pendingDiscoveryChannel = channel;
  service->pairingState->waitingForDiscoveryResponse = true;
  service->pairingState->discoveryRequestTime = millis();
}

void pairingService_initiatePairing(PairingService* service, const uint8_t* receiverMAC, uint8_t channel) {
  // Validate MAC address
  if (!isValidMAC(receiverMAC)) {
    return;  // Invalid MAC address
  }
  
  if (pairingState_isPaired(service->pairingState)) {
    return;  // Already paired
  }
  
  if (!service->pairingState->receiverBeaconReceived) {
    return;  // No beacon received yet
  }
  
  int slotsNeeded = getSlotsNeeded(service->pedalMode);
  if (service->pairingState->discoveredAvailableSlots < slotsNeeded) {
    return;  // Not enough slots
  }
  
  espNowTransport_addPeer(service->transport, receiverMAC, channel);
  
  struct_message discovery = {MSG_DISCOVERY_REQ, 0, false, service->pedalMode};
  espNowTransport_send(service->transport, receiverMAC, (uint8_t*)&discovery, sizeof(discovery));
  
  service->pairingState->waitingForDiscoveryResponse = true;
  service->pairingState->discoveryRequestTime = millis();
}

// Helper function to get cached transmitter MAC address
static void getCachedTransmitterMAC(uint8_t* mac) {
  static uint8_t cachedMAC[6] = {0};
  static bool macCached = false;
  
  if (!macCached) {
    WiFi.macAddress(cachedMAC);
    macCached = true;
  }
  memcpy(mac, cachedMAC, 6);
}

void pairingService_broadcastOnline(PairingService* service) {
  uint8_t transmitterMAC[6];
  getCachedTransmitterMAC(transmitterMAC);
  
  transmitter_online_message onlineMsg;
  onlineMsg.msgType = MSG_TRANSMITTER_ONLINE;
  macCopy(onlineMsg.transmitterMAC, transmitterMAC);
  
  // Debug: Log that we're broadcasting online
  extern void debugPrint(const char* format, ...);
  debugPrint("Broadcasting TRANSMITTER_ONLINE message");
  
  espNowTransport_broadcast(service->transport, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
}

void pairingService_broadcastPaired(PairingService* service, const uint8_t* receiverMAC) {
  uint8_t transmitterMAC[6];
  getCachedTransmitterMAC(transmitterMAC);
  
  transmitter_paired_message pairedMsg;
  pairedMsg.msgType = MSG_TRANSMITTER_PAIRED;
  macCopy(pairedMsg.transmitterMAC, transmitterMAC);
  macCopy(pairedMsg.receiverMAC, receiverMAC);
  
  espNowTransport_broadcast(service->transport, (uint8_t*)&pairedMsg, sizeof(pairedMsg));
}

bool pairingService_checkDiscoveryTimeout(PairingService* service, unsigned long currentTime) {
  if (!service->pairingState->waitingForDiscoveryResponse) {
    return false;  // Not waiting
  }
  
  if (currentTime - service->pairingState->discoveryRequestTime > 5000) {  // 5 second timeout
    service->pairingState->waitingForDiscoveryResponse = false;
    service->pairingState->discoveryRequestTime = 0;
    return true;  // Timeout occurred
  }
  
  return false;  // Still waiting
}

// Utility functions
bool isValidMAC(const uint8_t* mac) {
  if (!mac) return false;
  // Check if MAC is not all zeros and not all 0xFF
  bool allZero = true;
  bool allFF = true;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0) allZero = false;
    if (mac[i] != 0xFF) allFF = false;
  }
  return !allZero && !allFF;
}

bool macEqual(const uint8_t* mac1, const uint8_t* mac2) {
  if (!mac1 || !mac2) return false;
  return memcmp(mac1, mac2, 6) == 0;
}

void macCopy(uint8_t* dest, const uint8_t* src) {
  if (dest && src) {
    memcpy(dest, src, 6);
  }
}

int getSlotsNeeded(uint8_t pedalMode) {
  return (pedalMode == 0) ? 2 : 1;  // 0=DUAL needs 2 slots, 1=SINGLE needs 1 slot
}

void pairingService_processPendingDiscovery(PairingService* service) {
  if (!service->hasPendingDiscovery) {
    return;  // No pending discovery request
  }
  
  // Clear the flag first to avoid re-processing
  service->hasPendingDiscovery = false;
  uint8_t receiverMAC[6];
  uint8_t channel = service->pendingDiscoveryChannel;
  memcpy(receiverMAC, service->pendingDiscoveryMAC, 6);
  memset(service->pendingDiscoveryMAC, 0, 6);
  
  if (debugEnabled) {
    debugPrint("Processing deferred discovery request to receiver: %s", formatMAC(receiverMAC));
  }
  
  // Now send the discovery request from main loop context (safe!)
  struct_message discovery = {MSG_DISCOVERY_REQ, 0, false, service->pedalMode};
  bool sent = espNowTransport_send(service->transport, receiverMAC, (uint8_t*)&discovery, sizeof(discovery));
  
  if (debugEnabled) {
    debugPrint("Discovery request %s (from main loop)", sent ? "sent successfully" : "send FAILED");
  }
}
