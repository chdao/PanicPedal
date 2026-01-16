#include "PairingService.h"
#include <WiFi.h>
#include <string.h>
#include <Arduino.h>
#include "../domain/SlotManager.h"
#include "../shared/domain/PedalSlots.h"
#include "../shared/config.h"

void receiverPairingService_init(ReceiverPairingService* service, TransmitterManager* manager, 
                                  ReceiverEspNowTransport* transport, unsigned long bootTime) {
  service->manager = manager;
  service->transport = transport;
  service->bootTime = bootTime;
  service->lastBeaconTime = 0;
  service->initialPingTime = 0;  // Will be set when ping is actually sent
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
  
  int slotsNeeded = getSlotsNeeded(pedalMode);
  
  if (isKnownTransmitter) {
    // Known transmitter - check if new pedal mode fits using SlotManager
    bool wasAlreadyResponsive = service->manager->transmitters[knownIndex].seenOnBoot;
    SlotAvailabilityResult result;
    
    if (!wasAlreadyResponsive) {
      // Becoming responsive - check if new mode fits
      result = slotManager_checkReconnection(service->manager, knownIndex, slotsNeeded);
    } else {
      // Already responsive - check if mode changed
      result = slotManager_checkModeChange(service->manager, knownIndex, slotsNeeded);
    }
    
    if (!result.canFit) {
      if (service->debugCallback) {
        service->debugCallback("Discovery request rejected: existing transmitter would exceed slots (current=%d, needed=%d, after=%d)", 
                              result.currentSlotsUsed, slotsNeeded, result.slotsAfterChange);
      }
      return;
    }
    
    service->manager->transmitters[knownIndex].seenOnBoot = true;
    service->manager->transmitters[knownIndex].lastSeen = currentTime;
  } else {
    // New transmitter - check if slots available using SlotManager
    if (!slotManager_canFitNewTransmitter(service->manager, slotsNeeded)) {
      if (service->debugCallback) {
        int currentSlots = slotManager_getCurrentSlotsUsed(service->manager);
        service->debugCallback("Discovery request rejected: not enough slots for new transmitter (current=%d, needed=%d)", 
                              currentSlots, slotsNeeded);
      }
      return;
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
    // Known transmitter (was paired before) - use SlotManager for checks
    int slotsNeeded = getSlotsNeeded(service->manager->transmitters[transmitterIndex].pedalMode);
    bool isCurrentlyPaired = service->manager->transmitters[transmitterIndex].seenOnBoot;
    
    if (service->debugCallback) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               txMAC[0], txMAC[1], txMAC[2], txMAC[3], txMAC[4], txMAC[5]);
      int currentSlots = slotManager_getCurrentSlotsUsed(service->manager);
      service->debugCallback("Known transmitter %s came online (currently paired: %s, active slots: %d/%d, needs: %d)", 
                             macStr, isCurrentlyPaired ? "yes" : "no", currentSlots, MAX_PEDAL_SLOTS, slotsNeeded);
    }
    
    // Send MSG_PAIRING_CONFIRMED to known transmitters when they send MSG_TRANSMITTER_ONLINE:
    // - If currently paired: Always send (reconfirm pairing, transmitter will respond with MSG_PAIRING_CONFIRMED_ACK)
    // - If not currently paired: Send only if slots are available (they're coming back online)
    bool shouldRespond = false;
    if (isCurrentlyPaired) {
      // Currently paired transmitter - always send MSG_PAIRING_CONFIRMED to reconfirm pairing
      // Transmitter will respond with MSG_PAIRING_CONFIRMED_ACK (no loop since it's a different message type)
      shouldRespond = true;
      if (service->debugCallback) {
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 txMAC[0], txMAC[1], txMAC[2], txMAC[3], txMAC[4], txMAC[5]);
        service->debugCallback("Transmitter %s is already paired - sending MSG_PAIRING_CONFIRMED to reconfirm", macStr);
      }
    } else {
      // NOT currently paired - check if slots available using SlotManager
      SlotAvailabilityResult result = slotManager_checkReconnection(service->manager, transmitterIndex, slotsNeeded);
      if (result.canFit) {
        shouldRespond = true;
        if (service->debugCallback) {
          char macStr[18];
          snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                   txMAC[0], txMAC[1], txMAC[2], txMAC[3], txMAC[4], txMAC[5]);
          service->debugCallback("Transmitter %s not currently paired but slots available - sending MSG_PAIRING_CONFIRMED", macStr);
        }
      } else {
        if (service->debugCallback) {
          char macStr[18];
          snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                   txMAC[0], txMAC[1], txMAC[2], txMAC[3], txMAC[4], txMAC[5]);
          service->debugCallback("Transmitter %s not currently paired and slots full (%d + %d > %d) - not responding", 
                                 macStr, result.currentSlotsUsed, slotsNeeded, MAX_PEDAL_SLOTS);
        }
        service->manager->transmitters[transmitterIndex].lastSeen = millis();
        return;
      }
    }
    
    // shouldRespond should always be true here if we reach this point
    // (either currently paired, or slots available)
    if (!shouldRespond) {
      return;  // Safety check (should never happen)
    }
    
    // Send pairing confirmation (either currently paired or slots available)
    receiverEspNowTransport_addPeer(service->transport, txMAC, channel);
    
    // Send pairing confirmation message ("You're paired with me")
    pairing_confirmed_message confirm;
    confirm.msgType = MSG_PAIRING_CONFIRMED;
    WiFi.macAddress(confirm.receiverMAC);
    
    bool sent = receiverEspNowTransport_send(service->transport, txMAC, (uint8_t*)&confirm, sizeof(confirm));
    
    if (service->debugCallback) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               txMAC[0], txMAC[1], txMAC[2], txMAC[3], txMAC[4], txMAC[5]);
      bool wasResponsive = service->manager->transmitters[transmitterIndex].seenOnBoot;
      if (sent) {
        service->debugCallback("Sent MSG_PAIRING_CONFIRMED to known transmitter %s (was responsive: %s)", 
                               macStr, wasResponsive ? "yes" : "no");
      } else {
        service->debugCallback("Failed to send MSG_PAIRING_CONFIRMED to known transmitter %s", macStr);
      }
    }
    
    // Mark as seen now (since we're confirming pairing)
    service->manager->transmitters[transmitterIndex].seenOnBoot = true;
    
    service->manager->transmitters[transmitterIndex].lastSeen = millis();
  } else {
    // Unknown transmitter (or previously known but removed)
    int currentSlots = slotManager_getCurrentSlotsUsed(service->manager);
    unsigned long timeSinceBoot = millis() - service->bootTime;
    bool gracePeriodEnded = (timeSinceBoot >= TRANSMITTER_TIMEOUT);
    
    if (slotManager_areAllSlotsFull(service->manager)) {
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
  
  // Use SlotManager to check if receiver is full
  if (slotManager_areAllSlotsFull(service->manager)) {
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
// Sends MSG_PAIRING_CONFIRMED ONLY to previously known transmitters that are NOT currently paired
// This happens BEFORE grace period so known transmitters get priority
// Transmitters will accept this immediately and restore their pairing state
void receiverPairingService_pingKnownTransmittersOnBoot(ReceiverPairingService* service) {
  if (service->initialPingSent) {
    return;  // Already sent initial ping
  }
  
  // Send MSG_PAIRING_CONFIRMED ONLY to known transmitters that are NOT currently paired (seenOnBoot = false)
  // On boot, all transmitters loaded from EEPROM start with seenOnBoot = false
  // This gives them priority over new transmitters during grace period
  pairing_confirmed_message confirm;
  confirm.msgType = MSG_PAIRING_CONFIRMED;
  WiFi.macAddress(confirm.receiverMAC);
  int pingCount = 0;
  
  if (service->debugCallback) {
    service->debugCallback("Sending MSG_PAIRING_CONFIRMED to previously known transmitters (not currently paired)...");
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
    
    // Only send MSG_PAIRING_CONFIRMED to known transmitters that are NOT currently paired
    // (seenOnBoot = false means they haven't responded yet, so they're not currently paired)
    if (slotOccupied && !service->manager->transmitters[i].seenOnBoot) {
      uint8_t* mac = service->manager->transmitters[i].mac;
      receiverEspNowTransport_addPeer(service->transport, mac, 0);
      bool sent = receiverEspNowTransport_send(service->transport, mac, (uint8_t*)&confirm, sizeof(confirm));
      pingCount++;
      
      if (service->debugCallback) {
        if (sent) {
          service->debugCallback("Sent MSG_PAIRING_CONFIRMED to previously known transmitter %02X:%02X:%02X:%02X:%02X:%02X (not currently paired)", 
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
          service->debugCallback("Failed to send MSG_PAIRING_CONFIRMED to transmitter %02X:%02X:%02X:%02X:%02X:%02X", 
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
      }
      
      // Don't mark as seen yet - wait for MSG_TRANSMITTER_ONLINE response
      // The transmitter will send MSG_TRANSMITTER_ONLINE to acknowledge it received MSG_PAIRING_CONFIRMED
    }
  }
  
  // Always set initialPingSent and initialPingTime, even if no transmitters to ping
  // This ensures grace period timing is correct
  service->initialPingSent = true;
  service->initialPingTime = millis();  // Record when ping was actually sent
  
  if (pingCount > 0) {
    if (service->debugCallback) {
      service->debugCallback("Initial pairing confirmation complete: %d previously known transmitter(s) notified (before grace period)", pingCount);
    }
  } else {
    if (service->debugCallback) {
      service->debugCallback("No previously known transmitters to notify");
    }
  }
}

void receiverPairingService_pingKnownTransmitters(ReceiverPairingService* service) {
  unsigned long timeSinceBoot = millis() - service->bootTime;
  if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
    return;  // Grace period ended
  }
  
  // During grace period, send MSG_PAIRING_CONFIRMED to unresponsive known transmitters periodically
  // But only send once per transmitter (don't spam) - wait for them to respond with pedal events or MSG_TRANSMITTER_ONLINE
  // We already sent MSG_PAIRING_CONFIRMED on boot, so we don't need to keep sending it repeatedly
  // The transmitter will send a pedal event or MSG_TRANSMITTER_ONLINE when it comes online, which will mark it as seen
  
  // Actually, we don't need to send MSG_PAIRING_CONFIRMED repeatedly - we already sent it on boot
  // If the transmitter is online, it will send pedal events or MSG_TRANSMITTER_ONLINE
  // If it's offline, sending repeatedly won't help
  // So we can remove this periodic ping entirely, or just skip it
  // The initial ping on boot is sufficient
}

void receiverPairingService_update(ReceiverPairingService* service, unsigned long currentTime) {
    unsigned long timeSinceBoot = currentTime - service->bootTime;
  
  // Wait 1 second after initial ping was sent, then check slot reassignment (only once)
  // Only check if ping was actually sent and 1 second has elapsed since then
  if (!service->slotReassignmentDone && service->initialPingSent && service->initialPingTime > 0) {
    unsigned long timeSincePing = currentTime - service->initialPingTime;
    if (timeSincePing >= INITIAL_PING_WAIT) {
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
      // No known transmitters responded after initial wait - they might come online later during grace period
      if (service->debugCallback) {
        service->debugCallback("No known pedals replied to initial ping - preserving loaded transmitters");
      }
      // Don't modify anything - transmitters remain loaded from EEPROM
    }
    }
  }
  
  // Continue with grace period logic
  // Check continuously if all slots are filled - end grace period early if so
  // Note: Only bypass if transmitters RESPONDED during initial wait, not just if slots are reserved
  // Only start grace period checks after initial ping was sent and 1 second has elapsed
  if (!service->gracePeriodCheckDone && service->initialPingSent && service->initialPingTime > 0) {
    unsigned long timeSincePing = currentTime - service->initialPingTime;
    if (timeSincePing >= INITIAL_PING_WAIT) {
    
    // Check if all slots are filled by responsive transmitters (those that responded)
    bool allSlotsFilled = slotManager_areAllSlotsFull(service->manager);
    
    if (allSlotsFilled) {
      // All slots filled by responsive transmitters - bypass/end grace period
      service->gracePeriodCheckDone = true;
      service->gracePeriodSkipped = true;
      
      // Don't remove unresponsive transmitters - they stay in EEPROM until DELETE_RECORD is received
      // Recalculate slotsUsed based on responsive transmitters only
      int currentSlots = slotManager_getCurrentSlotsUsed(service->manager);
      service->manager->slotsUsed = currentSlots;
      
      // Count paired transmitters (responsive ones)
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
        if (timeSinceBoot <= INITIAL_PING_WAIT + 100) {
          // Slots filled immediately after initial wait - bypass grace period
          service->debugCallback("All slots filled immediately - bypassing grace period: %d pedal(s) paired (%d/%d slots used)", 
                                pairedCount, currentSlots, MAX_PEDAL_SLOTS);
        } else {
          // Slots filled during grace period - ended early
          service->debugCallback("Grace period ended early: %d pedal(s) paired (%d/%d slots used)", 
                                pairedCount, currentSlots, MAX_PEDAL_SLOTS);
        }
      }
      return;  // End grace period, don't send beacons
    }
    
    // Slots are available - continue grace period normally
    // Grace period will continue until timeout or slots fill up (checked continuously above)
    if (timeSinceBoot >= TRANSMITTER_TIMEOUT) {
      // Grace period timeout reached
      service->gracePeriodCheckDone = true;
      
      // After grace period timeout, update slotsUsed to match only responsive transmitters
      // Don't remove unresponsive transmitters - they stay in EEPROM until DELETE_RECORD is received
      // Recalculate slotsUsed based on responsive transmitters only
      int finalSlots = slotManager_getCurrentSlotsUsed(service->manager);
      service->manager->slotsUsed = finalSlots;
      
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
        int reservedSlots = transmitterManager_calculateReservedSlots(service->manager);
        if (pairedCount == 0) {
          if (reservedSlots >= MAX_PEDAL_SLOTS) {
            // All slots reserved by known transmitters, but none responded
            service->debugCallback("Grace period ended: All slots reserved by known transmitters (%d/%d), but none replied - preserving loaded transmitters", 
                                  reservedSlots, MAX_PEDAL_SLOTS);
          } else {
            // No slots reserved, no pedals paired
            service->debugCallback("Grace period ended: No pedals paired - preserving loaded transmitters");
          }
        } else {
          service->debugCallback("Grace period ended: %d pedal(s) paired (%d/%d slots used)", 
                                pairedCount, finalSlots, MAX_PEDAL_SLOTS);
        }
      }
    }
    // Note: If slots fill up during grace period, the check at the top of this block will catch it
    // and end grace period early (already handled above)
    }  // Close: if (timeSincePing >= INITIAL_PING_WAIT)
  }  // Close: if (!service->gracePeriodCheckDone && service->initialPingSent && service->initialPingTime > 0)
  
  // Send beacons during grace period only (after initial wait)
  // Only send beacons if slots are still available (known transmitters haven't filled all slots)
  // Note: We don't ping known transmitters periodically - we already sent MSG_PAIRING_CONFIRMED on boot
  // If they're online, they'll respond with pedal events or MSG_TRANSMITTER_ONLINE
  // Only send beacons after initial ping was sent and 1 second has elapsed
  if (!service->gracePeriodCheckDone && service->initialPingSent && service->initialPingTime > 0) {
    unsigned long timeSincePing = currentTime - service->initialPingTime;
    if (timeSincePing >= INITIAL_PING_WAIT) {
      int currentSlots = transmitterManager_calculateSlotsUsed(service->manager);
      bool slotsAvailable = (currentSlots < MAX_PEDAL_SLOTS);
      
      // Only send beacons if slots are available (allows new pairing)
      // If all known transmitters responded, no beacons = no new pairing
      if (slotsAvailable && (currentTime - service->lastBeaconTime > BEACON_INTERVAL)) {
        receiverPairingService_sendBeacon(service);
        service->lastBeaconTime = currentTime;
      }
    }
  }
  
  // Check for transmitter replacement timeout
  if (service->waitingForAliveResponses && currentTime >= service->aliveResponseTimeout) {
    // Don't remove unresponsive transmitters - they stay in EEPROM until DELETE_RECORD is received
    // Just send MSG_ALIVE to new transmitter if we have free slots
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    int currentSlots = slotManager_getCurrentSlotsUsed(service->manager);
    if (!slotManager_areAllSlotsFull(service->manager) && 
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

