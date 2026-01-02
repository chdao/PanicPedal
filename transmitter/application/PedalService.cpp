#include "PedalService.h"
#include "../application/PairingService.h"
#include <string.h>

static PedalService* g_pedalService = nullptr;
static PairingService* g_pairingService = nullptr;

void pedalService_setPairingService(PairingService* pairingService) {
  g_pairingService = pairingService;
}

void onPedalPress(char key) {
  if (!g_pedalService) return;
  
  // If not paired and we have a discovered receiver, initiate pairing
  if (!pairingState_isPaired(g_pedalService->pairingState) && 
      g_pedalService->pairingState->receiverBeaconReceived && g_pairingService) {
    // Determine slots needed based on pedal mode (0=DUAL needs 2, 1=SINGLE needs 1)
    int slotsNeeded = (g_pedalService->reader->pedalMode == 0) ? 2 : 1;
    if (g_pedalService->pairingState->discoveredAvailableSlots >= slotsNeeded) {
      pairingService_initiatePairing(g_pairingService, 
                                     g_pedalService->pairingState->discoveredReceiverMAC, 0);
    }
  }
  
  // Send pedal event if paired
  if (pairingState_isPaired(g_pedalService->pairingState)) {
    pedalService_sendPedalEvent(g_pedalService, key, true);
  }
  
  if (g_pedalService->onActivity) {
    g_pedalService->onActivity();
  }
}

void onPedalRelease(char key) {
  if (!g_pedalService) return;
  
  // Send pedal event if paired
  if (pairingState_isPaired(g_pedalService->pairingState)) {
    pedalService_sendPedalEvent(g_pedalService, key, false);
  }
  
  if (g_pedalService->onActivity) {
    g_pedalService->onActivity();
  }
}

void pedalService_init(PedalService* service, PedalReader* reader, PairingState* pairingState, 
                       EspNowTransport* transport, unsigned long* lastActivityTime) {
  service->reader = reader;
  service->pairingState = pairingState;
  service->transport = transport;
  service->lastActivityTime = lastActivityTime;
  service->onActivity = nullptr;
  g_pedalService = service;
}

void pedalService_update(PedalService* service) {
  pedalReader_update(service->reader, onPedalPress, onPedalRelease);
}

void pedalService_sendPedalEvent(PedalService* service, char key, bool pressed) {
  if (!pairingState_isPaired(service->pairingState)) {
    return;  // Not paired
  }
  
  struct_message msg = {MSG_PEDAL_EVENT, key, pressed, 0};
  espNowTransport_send(service->transport, service->pairingState->pairedReceiverMAC, 
                       (uint8_t*)&msg, sizeof(msg));
  
  if (service->lastActivityTime) {
    *service->lastActivityTime = millis();
  }
}

