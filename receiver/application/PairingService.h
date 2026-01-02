#ifndef RECEIVER_PAIRING_SERVICE_H
#define RECEIVER_PAIRING_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "../domain/TransmitterManager.h"
#include "../infrastructure/EspNowTransport.h"
#include "../shared/messages.h"

#define BEACON_INTERVAL 2000
#define TRANSMITTER_TIMEOUT 30000  // 30 seconds
#define ALIVE_RESPONSE_TIMEOUT 2000  // 2 seconds

typedef struct {
  TransmitterManager* manager;
  ReceiverEspNowTransport* transport;
  unsigned long bootTime;
  unsigned long lastBeaconTime;
  bool gracePeriodCheckDone;
  
  // Transmitter replacement mechanism
  uint8_t pendingNewTransmitterMAC[6];
  bool waitingForAliveResponses;
  unsigned long aliveResponseTimeout;
  bool transmitterResponded[MAX_PEDAL_SLOTS];
} ReceiverPairingService;

void receiverPairingService_init(ReceiverPairingService* service, TransmitterManager* manager, 
                                  ReceiverEspNowTransport* transport, unsigned long bootTime);
void receiverPairingService_handleDiscoveryRequest(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                    uint8_t pedalMode, uint8_t channel, unsigned long currentTime);
void receiverPairingService_handleTransmitterOnline(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                     uint8_t channel);
void receiverPairingService_handleTransmitterPaired(ReceiverPairingService* service, 
                                                     const transmitter_paired_message* msg);
void receiverPairingService_handleAlive(ReceiverPairingService* service, const uint8_t* txMAC);
void receiverPairingService_sendBeacon(ReceiverPairingService* service);
void receiverPairingService_pingKnownTransmitters(ReceiverPairingService* service);
void receiverPairingService_update(ReceiverPairingService* service, unsigned long currentTime);

#endif // RECEIVER_PAIRING_SERVICE_H

