#include "PairingState.h"
#include <string.h>
#include <Arduino.h>

void pairingState_init(PairingState* state) {
  memset(state->pairedReceiverMAC, 0, 6);
  memset(state->discoveredReceiverMAC, 0, 6);
  state->discoveredAvailableSlots = 0;
  state->isPaired = false;
  state->waitingForDiscoveryResponse = false;
  state->receiverBeaconReceived = false;
  state->discoveryRequestTime = 0;
}

bool pairingState_isPaired(const PairingState* state) {
  return state->isPaired;
}

void pairingState_setPaired(PairingState* state, const uint8_t* receiverMAC) {
  memcpy(state->pairedReceiverMAC, receiverMAC, 6);
  state->isPaired = true;
  state->waitingForDiscoveryResponse = false;
  state->receiverBeaconReceived = false;
  state->discoveryRequestTime = 0;
}

void pairingState_clearPairing(PairingState* state) {
  memset(state->pairedReceiverMAC, 0, 6);
  state->isPaired = false;
}

void pairingState_setDiscoveredReceiver(PairingState* state, const uint8_t* receiverMAC, uint8_t availableSlots) {
  memcpy(state->discoveredReceiverMAC, receiverMAC, 6);
  state->discoveredAvailableSlots = availableSlots;
  state->receiverBeaconReceived = true;
}

void pairingState_clearDiscoveredReceiver(PairingState* state) {
  memset(state->discoveredReceiverMAC, 0, 6);
  state->discoveredAvailableSlots = 0;
  state->receiverBeaconReceived = false;
}

