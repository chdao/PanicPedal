#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <Arduino.h>
#include "../messages.h"
#include "../domain/PairingState.h"
#include "../infrastructure/EspNowTransport.h"
#include "PairingService.h"

class MessageHandler {
public:
  static void init(PairingState* pairingState, PairingService* pairingService, EspNowTransport* transport, void (*onActivity)());
  static void handleMessage(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
  
private:
  static PairingState* pairingState;
  static PairingService* pairingService;
  static EspNowTransport* transport;
  static void (*onActivityCallback)();
  
  static void handleBeacon(const uint8_t* senderMAC, const uint8_t* data, int len);
  static void handlePairingConfirmed(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
  static void handlePairingConfirmedAck(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
  static void handleOtherMessages(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
  static void sendDeleteRecordMessage(const uint8_t* receiverMAC);
};

#endif // MESSAGE_HANDLER_H
