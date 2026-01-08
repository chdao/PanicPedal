#include "PairingService.h"
#include <WiFi.h>
#include <string.h>
#include <Arduino.h>

void receiverPairingService_init(ReceiverPairingService* service, TransmitterManager* manager, 
                                  ReceiverEspNowTransport* transport, unsigned long bootTime) {
  service->manager = manager;
  service->transport = transport;
  service->bootTime = bootTime;
  service->lastBeaconTime = 0;
  service->gracePeriodCheckDone = false;
  memset(service->pendingNewTransmitterMAC, 0, 6);
  service->waitingForAliveResponses = false;
  service->aliveResponseTimeout = 0;
  memset(service->transmitterResponded, false, sizeof(service->transmitterResponded));
}

void receiverPairingService_handleDiscoveryRequest(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                    uint8_t pedalMode, uint8_t channel, unsigned long currentTime) {
  int knownIndex = transmitterManager_findIndex(service->manager, txMAC);
  bool isKnownTransmitter = (knownIndex >= 0);
  
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  bool inDiscoveryPeriod = (timeSinceBoot < TRANSMITTER_TIMEOUT);
  
  // After grace period, only accept known transmitters
  if (!inDiscoveryPeriod && !isKnownTransmitter) {
    return;  // Rejected
  }
  
  if (isKnownTransmitter) {
    // Mark as seen
    service->manager->transmitters[knownIndex].seenOnBoot = true;
    service->manager->transmitters[knownIndex].lastSeen = currentTime;
  }
  
  // Check if receiver is full
  if (service->manager->slotsUsed >= MAX_PEDAL_SLOTS) {
    return;  // Receiver full
  }
  
  int slotsNeeded = (pedalMode == 0) ? 2 : 1;
  if (!transmitterManager_hasFreeSlots(service->manager, slotsNeeded)) {
    return;  // Not enough slots
  }
  
  // Add as peer and send response
  receiverEspNowTransport_addPeer(service->transport, txMAC, channel);
  
  struct_message response = {MSG_DISCOVERY_RESP, 0, false, 0};
  if (receiverEspNowTransport_send(service->transport, txMAC, (uint8_t*)&response, sizeof(response))) {
    transmitterManager_add(service->manager, txMAC, pedalMode);
  }
}

void receiverPairingService_handleTransmitterOnline(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                     uint8_t channel) {
  int transmitterIndex = transmitterManager_findIndex(service->manager, txMAC);
  
  if (transmitterIndex >= 0) {
    // Known transmitter
    if (service->manager->slotsUsed >= MAX_PEDAL_SLOTS) {
      return;  // Receiver full, don't send MSG_ALIVE
    }
    
    receiverEspNowTransport_addPeer(service->transport, txMAC, channel);
    
    struct_message alive = {MSG_ALIVE, 0, false, 0};
    receiverEspNowTransport_send(service->transport, txMAC, (uint8_t*)&alive, sizeof(alive));
    
    service->manager->transmitters[transmitterIndex].lastSeen = millis();
  } else {
    // Unknown transmitter - if receiver is full, try to replace unresponsive transmitters
    if (service->manager->slotsUsed >= MAX_PEDAL_SLOTS) {
      memcpy(service->pendingNewTransmitterMAC, txMAC, 6);
      
      // Ping all paired transmitters
      struct_message ping = {MSG_ALIVE, 0, false, 0};
      for (int i = 0; i < service->manager->count; i++) {
        service->transmitterResponded[i] = false;
        receiverEspNowTransport_send(service->transport, service->manager->transmitters[i].mac, 
                                     (uint8_t*)&ping, sizeof(ping));
      }
      
      service->waitingForAliveResponses = true;
      service->aliveResponseTimeout = millis() + ALIVE_RESPONSE_TIMEOUT;
    }
  }
}

void receiverPairingService_handleTransmitterPaired(ReceiverPairingService* service, 
                                                     const transmitter_paired_message* msg) {
  const uint8_t* txMAC = msg->transmitterMAC;
  const uint8_t* rxMAC = msg->receiverMAC;
  
  uint8_t ourMAC[6];
  WiFi.macAddress(ourMAC);
  
  int transmitterIndex = transmitterManager_findIndex(service->manager, txMAC);
  bool pairedWithUs = (memcmp(rxMAC, ourMAC, 6) == 0);
  
  if (transmitterIndex >= 0 && !pairedWithUs) {
    // Transmitter paired with another receiver - remove it
    transmitterManager_remove(service->manager, transmitterIndex);
  } else if (transmitterIndex >= 0 && pairedWithUs) {
    // Transmitter paired with us - update last seen
    service->manager->transmitters[transmitterIndex].lastSeen = millis();
    if (!service->gracePeriodCheckDone) {
      service->manager->transmitters[transmitterIndex].seenOnBoot = true;
    }
  }
}

void receiverPairingService_handleAlive(ReceiverPairingService* service, const uint8_t* txMAC) {
  int transmitterIndex = transmitterManager_findIndex(service->manager, txMAC);
  if (transmitterIndex >= 0) {
    service->manager->transmitters[transmitterIndex].lastSeen = millis();
    
    if (service->waitingForAliveResponses) {
      service->transmitterResponded[transmitterIndex] = true;
    }
    
    if (!service->gracePeriodCheckDone) {
      service->manager->transmitters[transmitterIndex].seenOnBoot = true;
    }
  }
}

void receiverPairingService_sendBeacon(ReceiverPairingService* service) {
  unsigned long timeSinceBoot = millis() - service->bootTime;
  if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
    return;  // Grace period ended
  }
  
  if (service->manager->slotsUsed >= MAX_PEDAL_SLOTS) {
    return;  // Receiver full
  }
  
  beacon_message beacon;
  beacon.msgType = MSG_BEACON;
  WiFi.macAddress(beacon.receiverMAC);
  beacon.availableSlots = transmitterManager_getAvailableSlots(service->manager);
  beacon.totalSlots = MAX_PEDAL_SLOTS;
  
  receiverEspNowTransport_broadcast(service->transport, (uint8_t*)&beacon, sizeof(beacon));
}

void receiverPairingService_pingKnownTransmitters(ReceiverPairingService* service) {
  unsigned long timeSinceBoot = millis() - service->bootTime;
  if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
    return;  // Grace period ended
  }
  
  if (service->manager->count == 0) return;
  
  struct_message ping = {MSG_ALIVE, 0, false, 0};
  for (int i = 0; i < service->manager->count; i++) {
    if (!service->manager->transmitters[i].seenOnBoot) {
      receiverEspNowTransport_send(service->transport, service->manager->transmitters[i].mac, 
                                   (uint8_t*)&ping, sizeof(ping));
    }
  }
}

void receiverPairingService_update(ReceiverPairingService* service, unsigned long currentTime) {
  // Mark grace period as done
  if (!service->gracePeriodCheckDone) {
    unsigned long timeSinceBoot = currentTime - service->bootTime;
    if (timeSinceBoot > TRANSMITTER_TIMEOUT) {
      service->gracePeriodCheckDone = true;
    }
  }
  
  // Send beacon and ping during grace period
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  if (timeSinceBoot < TRANSMITTER_TIMEOUT && 
      (currentTime - service->lastBeaconTime > BEACON_INTERVAL)) {
    receiverPairingService_sendBeacon(service);
    receiverPairingService_pingKnownTransmitters(service);
    service->lastBeaconTime = currentTime;
  }
  
  // Check for transmitter replacement timeout
  if (service->waitingForAliveResponses && currentTime >= service->aliveResponseTimeout) {
    // Remove unresponsive transmitters
    for (int i = service->manager->count - 1; i >= 0; i--) {
      if (!service->transmitterResponded[i]) {
        transmitterManager_remove(service->manager, i);
      }
    }
    
    // Send MSG_ALIVE to new transmitter if we have free slots
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    if (service->manager->slotsUsed < MAX_PEDAL_SLOTS && 
        memcmp(service->pendingNewTransmitterMAC, broadcastMAC, 6) != 0) {
      receiverEspNowTransport_addPeer(service->transport, service->pendingNewTransmitterMAC, 0);
      
      struct_message alive = {MSG_ALIVE, 0, false, 0};
      receiverEspNowTransport_send(service->transport, service->pendingNewTransmitterMAC, 
                                   (uint8_t*)&alive, sizeof(alive));
    }
    
    // Clear replacement state
    service->waitingForAliveResponses = false;
    memset(service->pendingNewTransmitterMAC, 0, 6);
    service->aliveResponseTimeout = 0;
  }
}

