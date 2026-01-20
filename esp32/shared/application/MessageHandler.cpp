#include "MessageHandler.h"
#include "../domain/MacUtils.h"
#include "../infrastructure/DebugService.h"
#include <Preferences.h>

PairingState* MessageHandler::pairingState = nullptr;
PairingService* MessageHandler::pairingService = nullptr;
EspNowTransport* MessageHandler::transport = nullptr;
void (*MessageHandler::onActivityCallback)() = nullptr;

void MessageHandler::init(PairingState* ps, PairingService* psrv, EspNowTransport* tr, void (*onAct)()) {
  pairingState = ps;
  pairingService = psrv;
  transport = tr;
  onActivityCallback = onAct;
}

void MessageHandler::handleMessage(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 2) return;  // Need at least deviceType + msgType
  
  uint8_t deviceType = data[0];  // First byte is deviceType
  uint8_t msgType = data[1];     // Second byte is msgType
  
  // CRITICAL: Transmitters should NEVER process messages from other transmitters
  // Check deviceType first - this is the most reliable way to filter
  if (deviceType == DEVICE_TYPE_TRANSMITTER) {
    // Message from another transmitter - silently ignore it (no logging to reduce spam)
    return;
  }
  
  // Reset activity timer when receiving messages (keeps device awake during active communication)
  if (onActivityCallback) {
    onActivityCallback();
  }
  
  char senderMacStr[18];
  snprintf(senderMacStr, sizeof(senderMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           senderMAC[0], senderMAC[1], senderMAC[2],
           senderMAC[3], senderMAC[4], senderMAC[5]);
  DebugService::print("Received ESP-NOW message: len=%d, sender=%s, deviceType=0x%02X, msgType=0x%02X", 
                     len, senderMacStr, deviceType, msgType);
  
  // Handle beacon message (only from receivers)
  if (deviceType == DEVICE_TYPE_RECEIVER && msgType == MSG_BEACON && len >= sizeof(beacon_message)) {
    handleBeacon(senderMAC, data, len);
    return;
  }
  
  // Handle pairing confirmed message from receiver (receiver-initiated pairing confirmation)
  if (deviceType == DEVICE_TYPE_RECEIVER && msgType == MSG_PAIRING_CONFIRMED && len >= sizeof(pairing_confirmed_message)) {
    handlePairingConfirmed(senderMAC, data, len, channel);
    return;
  }
  
  // Handle pairing confirmed acknowledgment from receiver
  if (deviceType == DEVICE_TYPE_RECEIVER && msgType == MSG_PAIRING_CONFIRMED_ACK && len >= sizeof(pairing_confirmed_ack_message)) {
    handlePairingConfirmedAck(senderMAC, data, len, channel);
    return;
  }
  
  // Handle debug monitor messages (must be handled regardless of pairing status)
  // Debug monitor discovery is independent of receiver pairing
  // NOTE: Handle both old format (msgType=MSG_DEBUG_MONITOR_REQ as first byte) and new format (deviceType=DEVICE_TYPE_DEBUG_MONITOR)
  if (deviceType == DEVICE_TYPE_DEBUG_MONITOR || 
      (len >= 1 && data[0] == MSG_DEBUG_MONITOR_REQ)) {
    // Check if it's the new format (deviceType first) or old format (msgType first)
    if (deviceType == DEVICE_TYPE_DEBUG_MONITOR && msgType == MSG_DEBUG_MONITOR_REQ && len >= sizeof(debug_monitor_req_message)) {
      // New format: deviceType=DEVICE_TYPE_DEBUG_MONITOR, msgType=MSG_DEBUG_MONITOR_REQ
      DebugService::handleDebugMonitorDiscovery(senderMAC);
    } else if (len >= 1 && data[0] == MSG_DEBUG_MONITOR_REQ && len >= 7) {
      // Old format: first byte is MSG_DEBUG_MONITOR_REQ (0x51), followed by MAC
      // This is a legacy format from before deviceType was added
      DebugService::handleDebugMonitorDiscovery(senderMAC);
    }
    // Silently ignore all debug monitor messages (we only care about discovery)
    // This prevents them from falling through to handleOtherMessages
    return;
  }
  
  // Handle other messages
  handleOtherMessages(senderMAC, data, len, channel);
}

void MessageHandler::handleBeacon(const uint8_t* senderMAC, const uint8_t* data, int len) {
  beacon_message* beacon = (beacon_message*)data;
  pairingService_handleBeacon(pairingService, senderMAC, beacon);
  DebugService::print("Received MSG_BEACON: slots=%d/%d", beacon->availableSlots, beacon->totalSlots);
}

void MessageHandler::handlePairingConfirmed(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  pairing_confirmed_message* confirm = (pairing_confirmed_message*)data;
  
  // Check if we're already paired to a different receiver
  if (pairingState_isPaired(pairingState)) {
    bool isPairedReceiver = macEqual(senderMAC, pairingState->pairedReceiverMAC);
    
    if (!isPairedReceiver) {
      // Already paired to a different receiver - tell this receiver to delete our record
      char senderMacStr[18];
      char pairedMacStr[18];
      snprintf(senderMacStr, sizeof(senderMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               senderMAC[0], senderMAC[1], senderMAC[2],
               senderMAC[3], senderMAC[4], senderMAC[5]);
      snprintf(pairedMacStr, sizeof(pairedMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               pairingState->pairedReceiverMAC[0], pairingState->pairedReceiverMAC[1], pairingState->pairedReceiverMAC[2],
               pairingState->pairedReceiverMAC[3], pairingState->pairedReceiverMAC[4], pairingState->pairedReceiverMAC[5]);
      DebugService::print("Received MSG_PAIRING_CONFIRMED from different receiver (%s) - we're paired to %s - sending DELETE_RECORD",
                         senderMacStr, pairedMacStr);
      
      espNowTransport_addPeer(transport, senderMAC, channel);
      delay(10);
      sendDeleteRecordMessage(senderMAC);
      return;  // Don't send ACK - we're rejecting this pairing
    }
    
    // Same receiver - just ensure peer is added
    espNowTransport_addPeer(transport, senderMAC, channel);
    delay(10);
    DebugService::print("Received MSG_PAIRING_CONFIRMED from paired receiver - peer confirmed");
  } else {
    // Not paired yet - restore pairing state with this receiver
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             senderMAC[0], senderMAC[1], senderMAC[2],
             senderMAC[3], senderMAC[4], senderMAC[5]);
    DebugService::print("Received MSG_PAIRING_CONFIRMED - restoring pairing state with receiver %s", macStr);
    memcpy(pairingState->pairedReceiverMAC, senderMAC, 6);
    pairingState->isPaired = true;
    
    // Ensure peer is added
    espNowTransport_addPeer(transport, senderMAC, channel);
    delay(10);
    
    // Save to NVS
    Preferences preferences;
    preferences.begin("pedal", false);
    preferences.putBytes("pairedMAC", senderMAC, 6);
    preferences.end();
    
    DebugService::print("Pairing restored - ready to send pedal events");
  }
  
  // Reply with MSG_PAIRING_CONFIRMED_ACK to acknowledge we received and accepted the pairing confirmation
  // This lets the receiver know we're online and responsive
  pairing_confirmed_ack_message ackMsg;
  ackMsg.deviceType = DEVICE_TYPE_TRANSMITTER;
  ackMsg.msgType = MSG_PAIRING_CONFIRMED_ACK;
  memcpy(ackMsg.receiverMAC, senderMAC, 6);  // Echo receiver's MAC to confirm
  
  espNowTransport_addPeer(transport, senderMAC, channel);
  delay(10);  // Small delay to ensure peer is ready
  bool sent = espNowTransport_send(transport, senderMAC, (uint8_t*)&ackMsg, sizeof(ackMsg));
  
  // Defer debug message to main loop (can't reliably send ESP-NOW broadcast from callback after sending)
  // Use DebugService's pending message mechanism
  extern void debugPrint(const char* format, ...);
  if (sent) {
    // Message will be processed in main loop via DebugService::processPending()
    // For now, just log directly since we're in callback context
    DebugService::print("Sent MSG_PAIRING_CONFIRMED_ACK to acknowledge receiver's pairing confirmation");
  } else {
    DebugService::print("Failed to send MSG_PAIRING_CONFIRMED_ACK acknowledgment");
  }
}

void MessageHandler::handlePairingConfirmedAck(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  pairing_confirmed_ack_message* ack = (pairing_confirmed_ack_message*)data;
  
  // Check if this is from our paired receiver
  if (pairingState_isPaired(pairingState)) {
    bool isPairedReceiver = macEqual(senderMAC, pairingState->pairedReceiverMAC);
    
    if (!isPairedReceiver) {
      // Different receiver - ignore (shouldn't happen, but handle gracefully)
      DebugService::print("Received MSG_PAIRING_CONFIRMED_ACK from different receiver - ignoring");
      return;
    }
  }
  
  // Clear waiting flag - we received the ACK
  pairingService->waitingForPairingConfirmedAck = false;
  pairingService->pairingConfirmedSentTime = 0;
  
  // Receiver acknowledged our reconnection request - restore pairing state if not already paired
  if (!pairingState_isPaired(pairingState)) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             senderMAC[0], senderMAC[1], senderMAC[2],
             senderMAC[3], senderMAC[4], senderMAC[5]);
    DebugService::print("Received MSG_PAIRING_CONFIRMED_ACK - restoring pairing state with receiver %s", macStr);
    memcpy(pairingState->pairedReceiverMAC, senderMAC, 6);
    pairingState->isPaired = true;
    
    // Ensure peer is added
    espNowTransport_addPeer(transport, senderMAC, channel);
    delay(10);
    
    // Save to NVS
    Preferences preferences;
    preferences.begin("pedal", false);
    preferences.putBytes("pairedMAC", senderMAC, 6);
    preferences.end();
    
    DebugService::print("Pairing restored - ready to send pedal events");
  } else {
    // Already paired - just ensure peer is added
    espNowTransport_addPeer(transport, senderMAC, channel);
    delay(10);
    DebugService::print("Received MSG_PAIRING_CONFIRMED_ACK from paired receiver - reconnection confirmed");
  }
}

void MessageHandler::handleOtherMessages(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  // Handle other messages (should only be from receivers at this point)
  uint8_t deviceType = data[0];
  uint8_t msgType = data[1];
  
  // Only process messages from receivers
  if (deviceType != DEVICE_TYPE_RECEIVER) {
    // Not from a receiver - silently ignore (shouldn't happen, but safety check)
    return;
  }
  
  if (len < sizeof(struct_message)) {
    DebugService::print("Message too short: len=%d, expected at least %d", len, sizeof(struct_message));
    return;
  }
  
  struct_message* msg = (struct_message*)data;
  
  // Verify deviceType matches (sanity check)
  if (msg->deviceType != DEVICE_TYPE_RECEIVER) {
    DebugService::print("Message deviceType mismatch: expected RECEIVER (0x%02X), got 0x%02X", 
                       DEVICE_TYPE_RECEIVER, msg->deviceType);
    return;
  }
  
  DebugService::print("Message type=%d (0x%02X), isPaired=%s", msg->msgType, msg->msgType,
             pairingState_isPaired(pairingState) ? "true" : "false");
  
  if (pairingState_isPaired(pairingState)) {
    // Already paired - check if message is from our paired receiver
    if (memcmp(senderMAC, pairingState->pairedReceiverMAC, 6) == 0) {
      // Message from our paired receiver
      if (msg->msgType == MSG_ALIVE) {
        DebugService::print("Received MSG_ALIVE from paired receiver - calling handleAlive");
        pairingService_handleAlive(pairingService, senderMAC, channel);
      } else if (msg->msgType == MSG_PAIRING_CONFIRMED_ACK && len >= sizeof(pairing_confirmed_ack_message)) {
        // Receiver acknowledged our reconnection request - already handled in dedicated handler above
        // Just reset activity timer
        if (onActivityCallback) {
          onActivityCallback();
        }
      } else {
        DebugService::print("Received message from paired receiver (type=%d)", msg->msgType);
      }
    } else {
      // Message from different receiver - send DELETE_RECORD
      if (msg->msgType == MSG_ALIVE || msg->msgType == MSG_DISCOVERY_RESP) {
        DebugService::print("Received message from different receiver - sending DELETE_RECORD");
        
        espNowTransport_addPeer(transport, senderMAC, channel);
        sendDeleteRecordMessage(senderMAC);
      }
    }
  } else {
    // Not paired - handle pairing messages
    if (msg->msgType == MSG_DISCOVERY_RESP) {
      DebugService::print("Handling MSG_DISCOVERY_RESP from receiver");
      pairingService_handleDiscoveryResponse(pairingService, senderMAC, channel);
    } else if (msg->msgType == MSG_ALIVE) {
      DebugService::print("Received MSG_ALIVE when not paired - calling handleAlive");
      pairingService_handleAlive(pairingService, senderMAC, channel);
    } else {
      DebugService::print("Unhandled message type=%d when not paired", msg->msgType);
    }
  }
}

void MessageHandler::sendDeleteRecordMessage(const uint8_t* receiverMAC) {
  struct_message deleteMsg = {DEVICE_TYPE_TRANSMITTER, MSG_DELETE_RECORD, 0, false, 0};
  espNowTransport_send(transport, receiverMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           receiverMAC[0], receiverMAC[1], receiverMAC[2],
           receiverMAC[3], receiverMAC[4], receiverMAC[5]);
  DebugService::print("Sent delete record message to receiver: %s", macStr);
}
