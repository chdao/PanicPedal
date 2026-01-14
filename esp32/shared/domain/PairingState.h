#ifndef PAIRING_STATE_H
#define PAIRING_STATE_H

#include <stdint.h>
#include <stdbool.h>

// Pairing state management
typedef struct {
  uint8_t pairedReceiverMAC[6];
  uint8_t discoveredReceiverMAC[6];
  uint8_t discoveredAvailableSlots;
  uint8_t discoveredReceiverChannel;  // Channel of discovered receiver (from beacon or MSG_ALIVE)
  bool isPaired;
  bool waitingForDiscoveryResponse;
  bool receiverBeaconReceived;
  unsigned long discoveryRequestTime;
} PairingState;

void pairingState_init(PairingState* state);
bool pairingState_isPaired(const PairingState* state);
void pairingState_setPaired(PairingState* state, const uint8_t* receiverMAC);
void pairingState_setDiscoveredReceiver(PairingState* state, const uint8_t* receiverMAC, uint8_t availableSlots, uint8_t channel);
void pairingState_clearDiscoveredReceiver(PairingState* state);

#endif // PAIRING_STATE_H
