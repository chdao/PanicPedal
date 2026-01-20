#include "PairingService.h"
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>
#include <Arduino.h>
#include "../messages.h"
#include "../domain/MacUtils.h"
#include "../domain/PedalSlots.h"
#include "../infrastructure/TransmitterUtils.h"
// Note: PairingState.h and EspNowTransport.h must be included before this file
// They are included by each project's .ino file

// Forward declaration for debug function (defined in transmitter.ino)
extern void debugPrint(const char* format, ...);
#include "../infrastructure/DebugService.h"

static inline const char* formatMAC(const uint8_t* mac) {
  // Only use for debug prints; uses a static buffer to keep call sites compact.
  static char macStr[18];
  transmitterUtils_formatMAC(macStr, sizeof(macStr), mac);
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
  service->pairingConfirmedSentTime = 0;
  service->waitingForPairingConfirmedAck = false;
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
                            !macIsZero(service->pairingState->pairedReceiverMAC);
  
  if (beacon->availableSlots >= slotsNeeded) {
    // Beacons don't include channel info, use 0 (ESP-NOW will use current WiFi channel)
    pairingState_setDiscoveredReceiver(service->pairingState, beacon->receiverMAC, beacon->availableSlots, 0);
    
    // If this is a previously paired receiver, automatically send discovery request
    // This handles both reconnection scenarios and cases where pairing was lost
    if (isPreviouslyPaired && !pairingState_isPaired(service->pairingState)) {
      if (DebugService::isEnabled()) {
        debugPrint("Beacon from previously paired receiver: %s (slots=%d/%d) - sending discovery request", 
                   formatMAC(beacon->receiverMAC), beacon->availableSlots, beacon->totalSlots);
      }
      
      // Automatically initiate pairing with previously paired receiver
      pairingService_initiatePairing(service, beacon->receiverMAC, 0);
    }
  } else {
    // Not enough slots available - clear discovered receiver state
    if (DebugService::isEnabled()) {
      debugPrint("Beacon from receiver %s has insufficient slots (%d/%d, need %d) - clearing discovery state", 
                 formatMAC(beacon->receiverMAC), beacon->availableSlots, beacon->totalSlots, slotsNeeded);
    }
    pairingState_clearDiscoveredReceiver(service->pairingState);
  }
}

void pairingService_handleDiscoveryResponse(PairingService* service, const uint8_t* senderMAC, uint8_t channel) {
  if (DebugService::isEnabled()) {
    debugPrint("Received MSG_DISCOVERY_RESP from receiver: %s (waiting=%s)", 
               formatMAC(senderMAC), service->pairingState->waitingForDiscoveryResponse ? "true" : "false");
  }
  
  if (!service->pairingState->waitingForDiscoveryResponse) {
    if (DebugService::isEnabled()) {
      debugPrint("Ignoring discovery response - not waiting for one");
    }
    return;  // Not waiting for response
  }
  
  if (DebugService::isEnabled()) {
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
  if (DebugService::isEnabled()) {
    debugPrint("Handling MSG_ALIVE from receiver: %s (channel=%d)", formatMAC(senderMAC), channel);
  }
  
  // Check if we're currently paired
  bool isCurrentlyPaired = pairingState_isPaired(service->pairingState);
  
  if (DebugService::isEnabled()) {
    debugPrint("Currently paired: %s", isCurrentlyPaired ? "true" : "false");
  }
  
  if (isCurrentlyPaired) {
    bool isPairedReceiver = macEqual(senderMAC, service->pairingState->pairedReceiverMAC);
    
    if (isPairedReceiver) {
      // Paired receiver requesting discovery - send MSG_ONLINE so receiver responds with MSG_PAIRING_CONFIRMED
      // Defer to main loop (can't send from callback)
      if (DebugService::isEnabled()) {
        debugPrint("MSG_ALIVE from paired receiver: %s - will send MSG_ONLINE", formatMAC(senderMAC));
      }
      service->hasPendingDiscovery = true;
      macCopy(service->pendingDiscoveryMAC, senderMAC);
      service->pendingDiscoveryChannel = channel;
      return;
    } else {
      // Different receiver - tell it to remove us from its list
      if (DebugService::isEnabled()) {
        debugPrint("MSG_ALIVE from different receiver (%s) - we're paired to %s - sending DELETE_RECORD", 
                   formatMAC(senderMAC), formatMAC(service->pairingState->pairedReceiverMAC));
      }
      espNowTransport_addPeer(service->transport, senderMAC, channel);
      struct_message deleteMsg = {DEVICE_TYPE_TRANSMITTER, MSG_DELETE_RECORD, 0, false, 0};
      bool sent = espNowTransport_send(service->transport, senderMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
      if (DebugService::isEnabled()) {
        debugPrint("DELETE_RECORD %s to different receiver", sent ? "sent successfully" : "send FAILED");
      }
      return;
    }
  }
  
  // Not paired - receiver is requesting discovery, defer to main loop
  if (DebugService::isEnabled()) {
    debugPrint("MSG_ALIVE from receiver (not paired): %s - deferring discovery request", formatMAC(senderMAC));
  }
  pairingState_setDiscoveredReceiver(service->pairingState, senderMAC, 2, channel);
  service->hasPendingDiscovery = true;
  macCopy(service->pendingDiscoveryMAC, senderMAC);
  service->pendingDiscoveryChannel = channel;
  service->pairingState->waitingForDiscoveryResponse = true;
  service->pairingState->discoveryRequestTime = millis();
}

void pairingService_initiatePairing(PairingService* service, const uint8_t* receiverMAC, uint8_t channel) {
  // Validate prerequisites
  if (!isValidMAC(receiverMAC)) {
    if (DebugService::isEnabled()) {
      DebugService::print("Cannot initiate pairing - invalid receiver MAC");
    }
    return;
  }
  if (pairingState_isPaired(service->pairingState)) {
    if (DebugService::isEnabled()) {
      DebugService::print("Cannot initiate pairing - already paired");
    }
    return;
  }
  
  // Allow pairing initiation even without beacon if we have a previously paired receiver MAC
  // This allows pedal press to trigger pairing with a previously known receiver
  bool hasPreviouslyPairedReceiver = !macIsZero(service->pairingState->pairedReceiverMAC) &&
                                      macEqual(receiverMAC, service->pairingState->pairedReceiverMAC);
  
  if (!service->pairingState->receiverBeaconReceived && !hasPreviouslyPairedReceiver) {
    if (DebugService::isEnabled()) {
      DebugService::print("Cannot initiate pairing - no receiver beacon received and not a previously paired receiver");
    }
    return;
  }
  
  // Check if receiver has enough slots available (only if we received a beacon)
  // If it's a previously paired receiver without beacon, assume slots are available and let receiver decide
  int slotsNeeded = getSlotsNeeded(service->pedalMode);
  if (service->pairingState->receiverBeaconReceived && 
      service->pairingState->discoveredAvailableSlots < slotsNeeded) {
    if (DebugService::isEnabled()) {
      DebugService::print("Cannot initiate pairing - insufficient slots (available: %d, needed: %d)", 
                         service->pairingState->discoveredAvailableSlots, slotsNeeded);
    }
    return;
  }
  
  // Send discovery request
  if (DebugService::isEnabled()) {
    DebugService::print("Sending MSG_DISCOVERY_REQ to receiver %s (slots available: %d, needed: %d)", 
                       formatMAC(receiverMAC), service->pairingState->discoveredAvailableSlots, slotsNeeded);
  }
  espNowTransport_addPeer(service->transport, receiverMAC, channel);
  delay(10);  // Small delay to ensure peer is ready
  struct_message discovery = {DEVICE_TYPE_TRANSMITTER, MSG_DISCOVERY_REQ, 0, false, service->pedalMode};
  bool sent = espNowTransport_send(service->transport, receiverMAC, (uint8_t*)&discovery, sizeof(discovery));
  
  if (DebugService::isEnabled()) {
    if (sent) {
      DebugService::print("MSG_DISCOVERY_REQ sent successfully");
    } else {
      DebugService::print("MSG_DISCOVERY_REQ send FAILED");
    }
  }
  
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
  
  online_message onlineMsg;
  onlineMsg.deviceType = DEVICE_TYPE_TRANSMITTER;
  onlineMsg.msgType = MSG_ONLINE;
  macCopy(onlineMsg.deviceMAC, transmitterMAC);
  
      if (DebugService::isEnabled()) {
        debugPrint("Broadcasting MSG_ONLINE message");
  }
  
  espNowTransport_broadcast(service->transport, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
}

void pairingService_broadcastPaired(PairingService* service, const uint8_t* receiverMAC) {
  uint8_t transmitterMAC[6];
  getCachedTransmitterMAC(transmitterMAC);
  
  transmitter_paired_message pairedMsg;
  pairedMsg.deviceType = DEVICE_TYPE_TRANSMITTER;
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

void pairingService_processPendingDiscovery(PairingService* service) {
  if (!service->hasPendingDiscovery) {
    return;
  }
  
  // Extract pending discovery info and clear the flag to prevent re-processing
  service->hasPendingDiscovery = false;
  uint8_t receiverMAC[6];
  uint8_t channel = service->pendingDiscoveryChannel;
  memcpy(receiverMAC, service->pendingDiscoveryMAC, 6);
  memset(service->pendingDiscoveryMAC, 0, 6);
  service->pendingDiscoveryChannel = 0;
  
  // Validate transport is ready
  if (!service->transport || !service->transport->initialized) {
    if (DebugService::isEnabled()) {
      debugPrint("ESP-NOW transport not initialized - cannot send message");
    }
    return;
  }
  
  // Check if we're paired to this receiver
  bool isPaired = pairingState_isPaired(service->pairingState) && 
                  macEqual(receiverMAC, service->pairingState->pairedReceiverMAC);
  
  if (isPaired) {
    // Already paired - send MSG_ONLINE so receiver responds with MSG_PAIRING_CONFIRMED
    if (DebugService::isEnabled()) {
      debugPrint("Sending MSG_ONLINE to paired receiver: %s", formatMAC(receiverMAC));
    }
    
    espNowTransport_addPeer(service->transport, receiverMAC, channel);
    yield();
    delay(ESPNOW_PEER_READY_DELAY_MS);
    yield();
    
    uint8_t transmitterMAC[6];
    getCachedTransmitterMAC(transmitterMAC);
    online_message onlineMsg;
    onlineMsg.deviceType = DEVICE_TYPE_TRANSMITTER;
    onlineMsg.msgType = MSG_ONLINE;
    macCopy(onlineMsg.deviceMAC, transmitterMAC);
    
    bool sent = espNowTransport_send(service->transport, receiverMAC, (uint8_t*)&onlineMsg, sizeof(onlineMsg));
    if (DebugService::isEnabled()) {
      debugPrint("MSG_ONLINE %s to paired receiver", sent ? "sent successfully" : "send FAILED");
    }
  } else {
    // Not paired - send discovery request
    if (DebugService::isEnabled()) {
      debugPrint("Processing deferred discovery request to receiver: %s (channel=%d)", formatMAC(receiverMAC), channel);
    }
    
    // Add peer with the stored channel (from the original MSG_ALIVE message)
    if (!espNowTransport_addPeer(service->transport, receiverMAC, channel)) {
      if (DebugService::isEnabled()) {
        debugPrint("Failed to add peer for discovery request");
      }
      return;
    }
    
    // Brief delay to ensure peer is ready (ESP32-S3 requires this)
    yield();
    delay(ESPNOW_PEER_READY_DELAY_MS);
    yield();
    
    // Verify peer was added successfully before attempting to send
    esp_now_peer_info_t peerInfo;
    if (esp_now_get_peer(receiverMAC, &peerInfo) != ESP_OK) {
      if (DebugService::isEnabled()) {
        debugPrint("Peer verification failed - peer not found after add");
      }
      return;
    }
    
    // Send discovery request from main loop context (safe - not from ESP-NOW callback)
    struct_message discovery = {DEVICE_TYPE_TRANSMITTER, MSG_DISCOVERY_REQ, 0, false, service->pedalMode};
    bool sent = espNowTransport_send(service->transport, receiverMAC, (uint8_t*)&discovery, sizeof(discovery));
    
    if (DebugService::isEnabled()) {
      if (sent) {
        debugPrint("Discovery request sent successfully (from main loop)");
      } else {
        debugPrint("Discovery request send FAILED (from main loop)");
      }
    }
    
    // Only mark as waiting for response if send was successful
    if (sent) {
      service->pairingState->waitingForDiscoveryResponse = true;
      service->pairingState->discoveryRequestTime = millis();
    }
  }
}
