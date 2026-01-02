#include "PairingService.h"
#include <WiFi.h>
#include <string.h>
#include <Arduino.h>
#include "../shared/messages.h"

void pairingService_init(PairingService* service, PairingState* state, EspNowTransport* transport, uint8_t pedalMode, unsigned long bootTime) {
  service->pairingState = state;
  service->transport = transport;
  service->pedalMode = pedalMode;
  service->bootTime = bootTime;
  service->onPaired = nullptr;
}

void pairingService_handleBeacon(PairingService* service, const uint8_t* senderMAC, const beacon_message* beacon) {
  int slotsNeeded = (service->pedalMode == 0) ? 2 : 1;
  
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
  bool isDiscovered = (memcmp(senderMAC, service->pairingState->discoveredReceiverMAC, 6) == 0) && 
                      service->pairingState->receiverBeaconReceived;
  
  if (isDiscovered) {
    int slotsNeeded = (service->pedalMode == 0) ? 2 : 1;
    if (service->pairingState->discoveredAvailableSlots < slotsNeeded) {
      return;  // Not enough slots
    }
    
    // Pair immediately
    pairingState_setPaired(service->pairingState, senderMAC);
    espNowTransport_addPeer(service->transport, senderMAC, channel);
    
    if (service->onPaired) {
      service->onPaired(senderMAC);
    }
  } else {
    // Unknown receiver - send discovery request if we have beacon info
    if (service->pairingState->receiverBeaconReceived) {
      int slotsNeeded = (service->pedalMode == 0) ? 2 : 1;
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
  if (pairingState_isPaired(service->pairingState)) {
    return;  // Already paired
  }
  
  if (!service->pairingState->receiverBeaconReceived) {
    return;  // No beacon received yet
  }
  
  int slotsNeeded = (service->pedalMode == 0) ? 2 : 1;
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
  memcpy(onlineMsg.transmitterMAC, transmitterMAC, 6);
  
  espNowTransport_broadcast(service->transport, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
}

void pairingService_broadcastPaired(PairingService* service, const uint8_t* receiverMAC) {
  uint8_t transmitterMAC[6];
  WiFi.macAddress(transmitterMAC);
  
  transmitter_paired_message pairedMsg;
  pairedMsg.msgType = MSG_TRANSMITTER_PAIRED;
  memcpy(pairedMsg.transmitterMAC, transmitterMAC, 6);
  memcpy(pairedMsg.receiverMAC, receiverMAC, 6);
  
  espNowTransport_broadcast(service->transport, (uint8_t*)&pairedMsg, sizeof(pairedMsg));
}

bool pairingService_checkDiscoveryTimeout(PairingService* service, unsigned long currentTime) {
  if (service->pairingState->waitingForDiscoveryResponse && 
      !pairingState_isPaired(service->pairingState) && 
      service->pairingState->discoveryRequestTime > 0) {
    if (currentTime - service->pairingState->discoveryRequestTime > 5000) {
      service->pairingState->waitingForDiscoveryResponse = false;
      service->pairingState->discoveryRequestTime = 0;
      return true;  // Timeout occurred
    }
  }
  return false;
}

