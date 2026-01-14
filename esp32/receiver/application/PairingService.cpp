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
  service->initialPingSent = false;
  service->gracePeriodSkipped = false;
  service->slotReassignmentDone = false;
  service->debugCallback = NULL;
  memset(service->pendingNewTransmitterMAC, 0, 6);
  service->waitingForAliveResponses = false;
  service->aliveResponseTimeout = 0;
  memset(service->transmitterResponded, false, sizeof(service->transmitterResponded));
}

void receiverPairingService_setDebugCallback(ReceiverPairingService* service, DebugCallback callback) {
  service->debugCallback = callback;
}

void receiverPairingService_handleDiscoveryRequest(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                    uint8_t pedalMode, uint8_t channel, unsigned long currentTime) {
  if (service->debugCallback) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             txMAC[0], txMAC[1], txMAC[2], txMAC[3], txMAC[4], txMAC[5]);
    service->debugCallback("Handling discovery request from %s (mode=%d, channel=%d)", macStr, pedalMode, channel);
  }
  
  // Reject new pairing requests if grace period was skipped (slots are full)
  if (service->gracePeriodSkipped) {
    if (service->debugCallback) {
      service->debugCallback("Discovery request rejected: grace period skipped (slots full)");
    }
    return;  // Don't accept new pedals if slots are full
  }
  
  // Check if this is a known transmitter (response to initial ping)
  int knownIndex = transmitterManager_findIndex(service->manager, txMAC);
  bool isKnownTransmitter = (knownIndex >= 0);
  
  // Reject NEW pairing requests during initial ping wait period
  // But ACCEPT discovery requests from known transmitters (they're responding to initial ping)
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  if (timeSinceBoot < INITIAL_PING_WAIT && !isKnownTransmitter) {
    if (service->debugCallback) {
      service->debugCallback("Discovery request rejected: still in initial ping wait, not known transmitter");
    }
    return;  // Still waiting for initial ping responses - reject new transmitters
  }
  
  bool inDiscoveryPeriod = (timeSinceBoot < TRANSMITTER_TIMEOUT);
  
  // After grace period, only accept known transmitters
  if (!inDiscoveryPeriod && !isKnownTransmitter) {
    if (service->debugCallback) {
      service->debugCallback("Discovery request rejected: after grace period, not known transmitter (timeSinceBoot=%lu, known=%d)",
                            timeSinceBoot, isKnownTransmitter);
    }
    return;  // Rejected
  }
  
  if (service->debugCallback) {
    service->debugCallback("Discovery request accepted: isKnown=%d, inDiscoveryPeriod=%d, timeSinceBoot=%lu",
                          isKnownTransmitter, inDiscoveryPeriod, timeSinceBoot);
  }
  
  int slotsNeeded = (pedalMode == 0) ? 2 : 1;
  
  if (isKnownTransmitter) {
    // Transmitter already exists - check if it can fit with the NEW pedal mode from discovery request
    // Check BEFORE marking as seen, so we can check if it was previously responsive
    bool wasAlreadyResponsive = service->manager->transmitters[knownIndex].seenOnBoot;
    int currentSlotsUsed = transmitterManager_calculateSlotsUsed(service->manager);
    int oldTransmitterSlots = (service->manager->transmitters[knownIndex].pedalMode == 0) ? 2 : 1;
    
    // If existing transmitter is not responsive yet, its old slots aren't counted in currentSlotsUsed
    // So we need to account for the NEW slots it needs when checking availability
    if (!wasAlreadyResponsive) {
      // Existing transmitter is becoming responsive - check if NEW pedal mode fits
      if (currentSlotsUsed + slotsNeeded > MAX_PEDAL_SLOTS) {
        if (service->debugCallback) {
          service->debugCallback("Discovery request rejected: existing transmitter would exceed slots (current=%d, new mode needs=%d)", 
                                currentSlotsUsed, slotsNeeded);
        }
        return;
      }
    } else {
      // Existing transmitter is already responsive - check if pedal mode change fits
      if (slotsNeeded != oldTransmitterSlots) {
        // Pedal mode changed - check if new mode fits
        int slotsAfterChange = currentSlotsUsed - oldTransmitterSlots + slotsNeeded;
        if (slotsAfterChange > MAX_PEDAL_SLOTS) {
          if (service->debugCallback) {
            service->debugCallback("Discovery request rejected: pedal mode change would exceed slots (old=%d, new=%d)", 
                                  oldTransmitterSlots, slotsNeeded);
          }
          return;
        }
      }
    }
    // Existing transmitter can fit with new pedal mode - mark as seen and proceed
    service->manager->transmitters[knownIndex].seenOnBoot = true;
    service->manager->transmitters[knownIndex].lastSeen = currentTime;
  } else {
    // New transmitter - check if there are free slots
    int currentSlotsUsed = transmitterManager_calculateSlotsUsed(service->manager);
    if (currentSlotsUsed + slotsNeeded > MAX_PEDAL_SLOTS) {
      if (service->debugCallback) {
        service->debugCallback("Discovery request rejected: not enough slots for new transmitter (current=%d, needed=%d)", 
                              currentSlotsUsed, slotsNeeded);
      }
      return;  // Not enough slots
    }
  }
  
  // Add as peer and send response
  receiverEspNowTransport_addPeer(service->transport, txMAC, channel);
  
  struct_message response = {MSG_DISCOVERY_RESP, 0, false, 0};
  bool sent = receiverEspNowTransport_send(service->transport, txMAC, (uint8_t*)&response, sizeof(response));
  if (service->debugCallback) {
    if (sent) {
      service->debugCallback("Discovery response sent successfully");
    } else {
      service->debugCallback("Discovery response send FAILED");
    }
  }
  if (sent) {
    // Check if transmitter already exists
    int existingIndex = transmitterManager_findIndex(service->manager, txMAC);
    
    // Count how many responsive transmitters exist (excluding this one)
    int responsiveCount = 0;
    for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
      bool slotOccupied = false;
      for (int j = 0; j < 6; j++) {
        if (service->manager->transmitters[i].mac[j] != 0) {
          slotOccupied = true;
          break;
        }
      }
      if (slotOccupied && service->manager->transmitters[i].seenOnBoot && i != existingIndex) {
        responsiveCount++;
      }
    }
    
    // If this is the first responsive transmitter (or only one), ensure it goes to slot 0
    if (responsiveCount == 0) {
      // If transmitter already exists, just update it
      if (existingIndex >= 0) {
        // Update existing transmitter
        service->manager->transmitters[existingIndex].seenOnBoot = true;
        service->manager->transmitters[existingIndex].lastSeen = millis();
        service->manager->transmitters[existingIndex].pedalMode = pedalMode;
      } else {
        // New transmitter - find first empty slot (starting from 0)
        int emptyIndex = -1;
        for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
          bool isEmpty = true;
          for (int j = 0; j < 6; j++) {
            if (service->manager->transmitters[i].mac[j] != 0) {
              isEmpty = false;
              break;
            }
          }
          if (isEmpty) {
            emptyIndex = i;
            break;
          }
        }
        
        if (emptyIndex >= 0) {
          // Found empty slot - assign transmitter there
          memcpy(service->manager->transmitters[emptyIndex].mac, txMAC, 6);
          service->manager->transmitters[emptyIndex].pedalMode = pedalMode;
          service->manager->transmitters[emptyIndex].seenOnBoot = true;
          service->manager->transmitters[emptyIndex].lastSeen = millis();
          
          // Update count if needed
          if (emptyIndex >= service->manager->count) {
            service->manager->count = emptyIndex + 1;
          }
        }
        // If no empty slot, receiver is full - can't add new transmitter
      }
    } else {
      // Use normal add function for subsequent transmitters
      // But if transmitter already exists, just update it
      if (existingIndex >= 0) {
        service->manager->transmitters[existingIndex].seenOnBoot = true;
        service->manager->transmitters[existingIndex].lastSeen = millis();
        service->manager->transmitters[existingIndex].pedalMode = pedalMode;
      } else {
        transmitterManager_add(service->manager, txMAC, pedalMode);
      }
    }
  }
}

void receiverPairingService_handleTransmitterOnline(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                     uint8_t channel) {
  int transmitterIndex = transmitterManager_findIndex(service->manager, txMAC);
  
  if (transmitterIndex >= 0) {
    // Known transmitter
    int currentSlots = transmitterManager_calculateSlotsUsed(service->manager);
    if (currentSlots >= MAX_PEDAL_SLOTS) {
      // Receiver full - still update last seen but don't send MSG_ALIVE
      service->manager->transmitters[transmitterIndex].lastSeen = millis();
      return;
    }
    
    // Check if this transmitter is currently paired (responsive/seen on boot)
    bool isCurrentlyPaired = service->manager->transmitters[transmitterIndex].seenOnBoot;
    
    // Only send MSG_ALIVE if transmitter is currently paired
    // This allows re-pairing after deep sleep for paired transmitters
    if (isCurrentlyPaired) {
      receiverEspNowTransport_addPeer(service->transport, txMAC, channel);
      
      struct_message alive = {MSG_ALIVE, 0, false, 0};
      receiverEspNowTransport_send(service->transport, txMAC, (uint8_t*)&alive, sizeof(alive));
      
      if (service->debugCallback) {
        service->debugCallback("Paired transmitter came online - requesting discovery");
      }
    }
    
    service->manager->transmitters[transmitterIndex].lastSeen = millis();
  } else {
    // Unknown transmitter (or previously known but removed)
    int currentSlots = transmitterManager_calculateSlotsUsed(service->manager);
    unsigned long timeSinceBoot = millis() - service->bootTime;
    bool gracePeriodEnded = (timeSinceBoot >= TRANSMITTER_TIMEOUT);
    
    if (currentSlots >= MAX_PEDAL_SLOTS) {
      // Receiver full - try to replace unresponsive transmitters
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
    } else if (gracePeriodEnded) {
      // Grace period ended and slots available - send MSG_ALIVE to request discovery
      // This allows previously paired transmitters (that were removed) to reconnect
      receiverEspNowTransport_addPeer(service->transport, txMAC, channel);
      
      struct_message alive = {MSG_ALIVE, 0, false, 0};
      receiverEspNowTransport_send(service->transport, txMAC, (uint8_t*)&alive, sizeof(alive));
      
      if (service->debugCallback) {
        service->debugCallback("Unknown transmitter came online after grace period - requesting discovery");
      }
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
    // Transmitter paired with another receiver - don't remove it
    // The transmitter will send DELETE_RECORD if it wants to be removed
    // Just update last seen to keep it in the list
    service->manager->transmitters[transmitterIndex].lastSeen = millis();
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
    // Known transmitter responded - keep it in its original slot
    bool wasSeen = service->manager->transmitters[transmitterIndex].seenOnBoot;
    
    service->manager->transmitters[transmitterIndex].lastSeen = millis();
    
    if (service->waitingForAliveResponses) {
      service->transmitterResponded[transmitterIndex] = true;
    }
    
    if (!wasSeen) {
      // First time this transmitter responded - mark as seen
      // Keep it in its original slot (don't reorder)
      // calculateSlotsUsed() will count this transmitter now that seenOnBoot = true
      service->manager->transmitters[transmitterIndex].seenOnBoot = true;
    }
  }
}

void receiverPairingService_sendBeacon(ReceiverPairingService* service) {
  unsigned long timeSinceBoot = millis() - service->bootTime;
  if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
    return;  // Grace period ended
  }
  
  // Use calculated slots (only responsive transmitters) during grace period
  int currentSlots = transmitterManager_calculateSlotsUsed(service->manager);
  if (currentSlots >= MAX_PEDAL_SLOTS) {
    return;  // Receiver full (based on responsive transmitters only)
  }
  
  beacon_message beacon;
  beacon.msgType = MSG_BEACON;
  WiFi.macAddress(beacon.receiverMAC);
  beacon.availableSlots = transmitterManager_getAvailableSlots(service->manager);
  beacon.totalSlots = MAX_PEDAL_SLOTS;
  
  receiverEspNowTransport_broadcast(service->transport, (uint8_t*)&beacon, sizeof(beacon));
}

// Ping known transmitters immediately on boot (before grace period/pairing)
// Sends MSG_ALIVE to trigger transmitters to check pairing and send discovery requests if not paired
void receiverPairingService_pingKnownTransmittersOnBoot(ReceiverPairingService* service) {
  if (service->initialPingSent) {
    return;  // Already sent initial ping
  }
  
  // Send MSG_ALIVE to ALL known transmitters (from EEPROM) immediately on boot
  // This triggers them to:
  // - If paired: respond with MSG_ALIVE or MSG_TRANSMITTER_ONLINE
  // - If not paired: send MSG_DISCOVERY_REQ to re-pair
  // This happens before pairing/grace period to restore previous pairings
  struct_message alive = {MSG_ALIVE, 0, false, 0};
  int pingCount = 0;
  
  if (service->debugCallback) {
    service->debugCallback("Sending initial ping (MSG_ALIVE) to known transmitters to request discovery...");
  }
  
  for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
    // Check if slot is occupied
    bool slotOccupied = false;
    for (int j = 0; j < 6; j++) {
      if (service->manager->transmitters[i].mac[j] != 0) {
        slotOccupied = true;
        break;
      }
    }
    
    // Send MSG_ALIVE to all known transmitters to trigger discovery requests
    if (slotOccupied) {
      uint8_t* mac = service->manager->transmitters[i].mac;
      receiverEspNowTransport_addPeer(service->transport, mac, 0);
      receiverEspNowTransport_send(service->transport, mac, (uint8_t*)&alive, sizeof(alive));
      pingCount++;
      
      if (service->debugCallback) {
        service->debugCallback("Sent MSG_ALIVE to transmitter %02X:%02X:%02X:%02X:%02X:%02X (requesting discovery)", 
                               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      }
    }
  }
  
  if (pingCount > 0) {
    service->initialPingSent = true;
    if (service->debugCallback) {
      service->debugCallback("Initial ping complete: %d transmitter(s) pinged, waiting for discovery requests", pingCount);
    }
  } else {
    if (service->debugCallback) {
      service->debugCallback("No known transmitters to ping");
    }
  }
}

void receiverPairingService_pingKnownTransmitters(ReceiverPairingService* service) {
  unsigned long timeSinceBoot = millis() - service->bootTime;
  if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
    return;  // Grace period ended
  }
  
  // During grace period, ping unresponsive known transmitters periodically
  struct_message ping = {MSG_ALIVE, 0, false, 0};
  for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
    // Check if slot is occupied
    bool slotOccupied = false;
    for (int j = 0; j < 6; j++) {
      if (service->manager->transmitters[i].mac[j] != 0) {
        slotOccupied = true;
        break;
      }
    }
    
    // Ping unresponsive known transmitters
    if (slotOccupied && !service->manager->transmitters[i].seenOnBoot) {
      receiverEspNowTransport_send(service->transport, service->manager->transmitters[i].mac, 
                                   (uint8_t*)&ping, sizeof(ping));
    }
  }
}

void receiverPairingService_update(ReceiverPairingService* service, unsigned long currentTime) {
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  
  // Wait 1 second after initial ping, then check slot reassignment (only once)
  if (!service->slotReassignmentDone && timeSinceBoot >= INITIAL_PING_WAIT) {
    service->slotReassignmentDone = true;
    
    // Count how many transmitters responded to initial ping
    int responsiveCount = 0;
    uint8_t responsiveMACs[2][6] = {0};
    uint8_t responsivePedalModes[2] = {0};
    
    for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
      bool slotOccupied = false;
      for (int j = 0; j < 6; j++) {
        if (service->manager->transmitters[i].mac[j] != 0) {
          slotOccupied = true;
          break;
        }
      }
      if (slotOccupied && service->manager->transmitters[i].seenOnBoot) {
        if (responsiveCount < 2) {
          memcpy(responsiveMACs[responsiveCount], service->manager->transmitters[i].mac, 6);
          responsivePedalModes[responsiveCount] = service->manager->transmitters[i].pedalMode;
          responsiveCount++;
        }
      }
    }
    
    // If only 1 pedal responded, mark it as seen (keep it in its current slot)
    // If 2 pedals responded, keep their slot assignments
    if (responsiveCount == 1) {
      // Find which slot the responsive transmitter is currently in
      int currentSlot = transmitterManager_findIndex(service->manager, responsiveMACs[0]);
      
      if (currentSlot >= 0) {
        // Transmitter already exists - just mark as seen
        service->manager->transmitters[currentSlot].seenOnBoot = true;
        service->manager->transmitters[currentSlot].lastSeen = millis();
      }
      
      if (service->debugCallback) {
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 responsiveMACs[0][0], responsiveMACs[0][1], responsiveMACs[0][2],
                 responsiveMACs[0][3], responsiveMACs[0][4], responsiveMACs[0][5]);
        service->debugCallback("Single pedal responded - keeping in slot %d: %s", currentSlot + 1, macStr);
      }
    } else if (responsiveCount == 2) {
      // Both pedals responded - keep their slot assignments (already in correct slots)
      if (service->debugCallback) {
        service->debugCallback("Both pedals responded - keeping slot assignments");
      }
    } else if (responsiveCount == 0) {
      // No transmitters responded - preserve all loaded transmitters (don't clear them)
      // They might come online later, so keep them in their current slots
      if (service->debugCallback) {
        service->debugCallback("No pedals responded to initial ping - preserving loaded transmitters");
      }
      // Don't modify anything - transmitters remain loaded from EEPROM
    }
  }
  
  // Continue with grace period logic
  if (!service->gracePeriodCheckDone && timeSinceBoot >= INITIAL_PING_WAIT) {
    
    // Check if all slots are filled by responsive transmitters
    int currentSlots = transmitterManager_calculateSlotsUsed(service->manager);
    bool allSlotsFilled = (currentSlots >= MAX_PEDAL_SLOTS);
    
    if (allSlotsFilled) {
      // Skip grace period entirely - slots are full, no new pairing allowed
      service->gracePeriodCheckDone = true;
      service->gracePeriodSkipped = true;
      
      // Don't remove unresponsive transmitters - they stay in EEPROM until DELETE_RECORD is received
      // Recalculate slotsUsed based on responsive transmitters only
      service->manager->slotsUsed = transmitterManager_calculateSlotsUsed(service->manager);
      
      // Count paired transmitters
      int pairedCount = 0;
      for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
        bool slotOccupied = false;
        for (int j = 0; j < 6; j++) {
          if (service->manager->transmitters[i].mac[j] != 0) {
            slotOccupied = true;
            break;
          }
        }
        if (slotOccupied && service->manager->transmitters[i].seenOnBoot) {
          pairedCount++;
        }
      }
      
      if (service->debugCallback) {
        service->debugCallback("Grace period skipped: %d pedal(s) paired (%d/%d slots used)", 
                              pairedCount, currentSlots, MAX_PEDAL_SLOTS);
      }
      return;  // Skip grace period, don't send beacons
    }
    
    // Slots are available - start grace period normally
    // Grace period will continue until timeout or slots fill up
    if (timeSinceBoot > TRANSMITTER_TIMEOUT) {
      service->gracePeriodCheckDone = true;
      
      // After grace period timeout, update slotsUsed to match only responsive transmitters
      // Don't remove unresponsive transmitters - they stay in EEPROM until DELETE_RECORD is received
      // Recalculate slotsUsed based on responsive transmitters only
      service->manager->slotsUsed = transmitterManager_calculateSlotsUsed(service->manager);
      
      // Count paired transmitters
      int pairedCount = 0;
      for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
        bool slotOccupied = false;
        for (int j = 0; j < 6; j++) {
          if (service->manager->transmitters[i].mac[j] != 0) {
            slotOccupied = true;
            break;
          }
        }
        if (slotOccupied && service->manager->transmitters[i].seenOnBoot) {
          pairedCount++;
        }
      }
      
      if (service->debugCallback) {
        int finalSlots = transmitterManager_calculateSlotsUsed(service->manager);
        service->debugCallback("Grace period ended: %d pedal(s) paired (%d/%d slots used)", 
                              pairedCount, finalSlots, MAX_PEDAL_SLOTS);
      }
    } else {
      // Check if slots filled up during grace period
      int currentSlotsDuringGrace = transmitterManager_calculateSlotsUsed(service->manager);
      if (currentSlotsDuringGrace >= MAX_PEDAL_SLOTS) {
        // All slots filled - end grace period early
        service->gracePeriodCheckDone = true;
        
        // Don't remove unresponsive transmitters - they stay in EEPROM until DELETE_RECORD is received
        // Recalculate slotsUsed based on responsive transmitters only
        service->manager->slotsUsed = transmitterManager_calculateSlotsUsed(service->manager);
        
        // Count paired transmitters
        int pairedCount = 0;
        for (int i = 0; i < MAX_PEDAL_SLOTS; i++) {
          bool slotOccupied = false;
          for (int j = 0; j < 6; j++) {
            if (service->manager->transmitters[i].mac[j] != 0) {
              slotOccupied = true;
              break;
            }
          }
          if (slotOccupied && service->manager->transmitters[i].seenOnBoot) {
            pairedCount++;
          }
        }
        
        if (service->debugCallback) {
          service->debugCallback("Grace period ended early: %d pedal(s) paired (%d/%d slots used)", 
                                pairedCount, currentSlotsDuringGrace, MAX_PEDAL_SLOTS);
        }
      }
    }
  }
  
  // Send beacon and ping during grace period only (after initial wait)
  // Only send beacons if slots are still available (known transmitters haven't filled all slots)
  if (!service->gracePeriodCheckDone && timeSinceBoot >= INITIAL_PING_WAIT) {
    int currentSlots = transmitterManager_calculateSlotsUsed(service->manager);
    bool slotsAvailable = (currentSlots < MAX_PEDAL_SLOTS);
    
    // Ping unresponsive known transmitters periodically
    if (currentTime - service->lastBeaconTime > BEACON_INTERVAL) {
      receiverPairingService_pingKnownTransmitters(service);
      service->lastBeaconTime = currentTime;
    }
    
    // Only send beacons if slots are available (allows new pairing)
    // If all known transmitters responded, no beacons = no new pairing
    if (slotsAvailable && (currentTime - service->lastBeaconTime > BEACON_INTERVAL)) {
      receiverPairingService_sendBeacon(service);
      service->lastBeaconTime = currentTime;
    }
  }
  
  // Check for transmitter replacement timeout
  if (service->waitingForAliveResponses && currentTime >= service->aliveResponseTimeout) {
    // Don't remove unresponsive transmitters - they stay in EEPROM until DELETE_RECORD is received
    // Just send MSG_ALIVE to new transmitter if we have free slots
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    int currentSlots = transmitterManager_calculateSlotsUsed(service->manager);
    if (currentSlots < MAX_PEDAL_SLOTS && 
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

