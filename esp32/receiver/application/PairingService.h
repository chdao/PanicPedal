#ifndef RECEIVER_PAIRING_SERVICE_H
#define RECEIVER_PAIRING_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "../domain/TransmitterManager.h"
#include "../domain/SlotManager.h"
#include "../infrastructure/EspNowTransport.h"
#include "../shared/messages.h"
#include "../shared/config.h"

// Use centralized config values
#define BEACON_INTERVAL BEACON_INTERVAL_MS
#define TRANSMITTER_TIMEOUT TRANSMITTER_TIMEOUT_MS
#define ALIVE_RESPONSE_TIMEOUT ALIVE_RESPONSE_TIMEOUT_MS
#define INITIAL_PING_WAIT INITIAL_PING_WAIT_MS

// Forward declaration for debug callback
typedef void (*DebugCallback)(const char* format, ...);

typedef struct {
  TransmitterManager* manager;
  ReceiverEspNowTransport* transport;
  unsigned long bootTime;
  unsigned long lastBeaconTime;
  unsigned long initialPingTime;  // Track when initial ping was sent (for 1-second wait)
  bool gracePeriodCheckDone;
  bool initialPingSent;  // Track if initial ping to known transmitters has been sent
  bool gracePeriodSkipped;  // Track if grace period was skipped because slots are full
  bool slotReassignmentDone;  // Track if slot reassignment check has been performed
  DebugCallback debugCallback;  // Callback for debug messages
  
  // Transmitter replacement mechanism
  uint8_t pendingNewTransmitterMAC[6];
  bool waitingForAliveResponses;
  unsigned long aliveResponseTimeout;
  bool transmitterResponded[MAX_PEDAL_SLOTS];
} ReceiverPairingService;

void receiverPairingService_init(ReceiverPairingService* service, TransmitterManager* manager, 
                                  ReceiverEspNowTransport* transport, unsigned long bootTime);
void receiverPairingService_setDebugCallback(ReceiverPairingService* service, DebugCallback callback);
void receiverPairingService_handleDiscoveryRequest(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                    uint8_t pedalMode, uint8_t channel, unsigned long currentTime);
void receiverPairingService_handleTransmitterOnline(ReceiverPairingService* service, const uint8_t* txMAC, 
                                                     uint8_t channel);
void receiverPairingService_handleTransmitterPaired(ReceiverPairingService* service, 
                                                     const transmitter_paired_message* msg);
void receiverPairingService_handleAlive(ReceiverPairingService* service, const uint8_t* txMAC);
void receiverPairingService_sendBeacon(ReceiverPairingService* service);
void receiverPairingService_pingKnownTransmittersOnBoot(ReceiverPairingService* service);  // Immediate ping on boot
void receiverPairingService_pingKnownTransmitters(ReceiverPairingService* service);  // Periodic ping during grace period
void receiverPairingService_update(ReceiverPairingService* service, unsigned long currentTime);

#endif // RECEIVER_PAIRING_SERVICE_H

