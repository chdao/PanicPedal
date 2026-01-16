#ifndef PAIRING_SERVICE_H
#define PAIRING_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "../messages.h"
#include "../domain/PairingState.h"
#include "../infrastructure/EspNowTransport.h"

typedef struct {
  PairingState* pairingState;
  EspNowTransport* transport;
  uint8_t pedalMode;  // 0=DUAL, 1=SINGLE
  unsigned long bootTime;
  void (*onPaired)(const uint8_t* receiverMAC);
  // Deferred discovery request (to avoid sending from ESP-NOW callback)
  uint8_t pendingDiscoveryMAC[6];
  uint8_t pendingDiscoveryChannel;
  bool hasPendingDiscovery;
  // Track MSG_PAIRING_CONFIRMED timeout (send MSG_TRANSMITTER_ONLINE if no response)
  unsigned long pairingConfirmedSentTime;
  bool waitingForPairingConfirmedAck;
} PairingService;

void pairingService_init(PairingService* service, PairingState* state, EspNowTransport* transport, uint8_t pedalMode, unsigned long bootTime);
void pairingService_handleBeacon(PairingService* service, const uint8_t* senderMAC, const beacon_message* beacon);
void pairingService_handleDiscoveryResponse(PairingService* service, const uint8_t* senderMAC, uint8_t channel);
void pairingService_handleAlive(PairingService* service, const uint8_t* senderMAC, uint8_t channel);
void pairingService_initiatePairing(PairingService* service, const uint8_t* receiverMAC, uint8_t channel);
void pairingService_broadcastOnline(PairingService* service);
void pairingService_broadcastPaired(PairingService* service, const uint8_t* receiverMAC);
bool pairingService_checkDiscoveryTimeout(PairingService* service, unsigned long currentTime);
void pairingService_processPendingDiscovery(PairingService* service);  // Process deferred discovery request from main loop

#endif // PAIRING_SERVICE_H
