#include "PairingService.h"
#include <WiFi.h>
#include <string.h>
#include <Arduino.h>
#include "../shared/messages.h"

// Forward declarations for utility functions
bool isValidMAC(const uint8_t* mac);
bool macEqual(const uint8_t* mac1, const uint8_t* mac2);
void macCopy(uint8_t* dest, const uint8_t* src);
int getSlotsNeeded(uint8_t pedalMode);

void pairingService_init(PairingService* service, PairingState* state, EspNowTransport* transport, uint8_t pedalMode, unsigned long bootTime) {
  service->pairingState = state;
  service->transport = transport;
  service->pedalMode = pedalMode;
  service->bootTime = bootTime;
  service->onPaired = nullptr;
}

void pairingService_handleBeacon(PairingService* service, const uint8_t* senderMAC, const beacon_message* beacon) {
  // Validate MAC addresses
  if (!isValidMAC(senderMAC) || !isValidMAC(beacon->receiverMAC)) {
    return;  // Invalid MAC addresses, ignore beacon
  }
  
  int slotsNeeded = getSlotsNeeded(service->pedalMode);
  
  if (beacon->availableSlots >= slotsNeeded) {
    pairingState_setDiscoveredReceiver(service->pairingState, beacon->receiverMAC, beacon->availableSlots);
  } else {
    pairingState_clearDiscoveredReceiver(service->pairingState);
  }
}

void pairingService_handleDiscoveryResponse(PairingService* service, const uint8_t* senderMAC, uint8_t channel) {
  if (!service->pairingState->waitingForDiscoveryResponse) {
    return;  // Not waiting for response
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
  if (pairingState_isPaired(service->pairingState)) {
    return;  // Already paired, ignore
  }
  
  // Check if this is the discovered receiver
  bool isDiscovered = macEqual(senderMAC, service->pairingState->discoveredReceiverMAC) && 
                      service->pairingState->receiverBeaconReceived;
  
  if (isDiscovered) {
    int slotsNeeded = getSlotsNeeded(service->pedalMode);
    if (service->pairingState->discoveredAvailableSlots < slotsNeeded) {
      return;  // Not enough slots
    }
    
    // Pair immediately
    pairingState_setPaired(service->pairingState, senderMAC);
    espNowTransport_addPeer(service->transport, senderMAC, channel);
    
    // Clear waiting flag since we're now paired
    service->pairingState->waitingForDiscoveryResponse = false;
    service->pairingState->discoveryRequestTime = 0;
    
    if (service->onPaired) {
      service->onPaired(senderMAC);
    }
  } else {
    // Unknown receiver - send discovery request if we have beacon info
    if (service->pairingState->receiverBeaconReceived) {
      int slotsNeeded = getSlotsNeeded(service->pedalMode);
      if (service->pairingState->discoveredAvailableSlots < slotsNeeded) {
        pairingState_clearDiscoveredReceiver(service->pairingState);
        return;
      }
    }
    
    pairingState_clearDiscoveredReceiver(service->pairingState);
    espNowTransport_addPeer(service->transport, senderMAC, channel);
    
    struct_message discovery = {MSG_DISCOVERY_REQ, 0, false, service->pedalMode};
    espNowTransport_send(service->transport, senderMAC, (uint8_t*)&discovery, sizeof(discovery));
    
    service->pairingState->waitingForDiscoveryResponse = true;
    service->pairingState->discoveryRequestTime = millis();
  }
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

void pairingService_broadcastOnline(PairingService* service) {
  uint8_t transmitterMAC[6];
  WiFi.macAddress(transmitterMAC);
  
  transmitter_online_message onlineMsg;
  onlineMsg.msgType = MSG_TRANSMITTER_ONLINE;
  macCopy(onlineMsg.transmitterMAC, transmitterMAC);
  
  espNowTransport_broadcast(service->transport, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
}

void pairingService_broadcastPaired(PairingService* service, const uint8_t* receiverMAC) {
  uint8_t transmitterMAC[6];
  WiFi.macAddress(transmitterMAC);
  
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

