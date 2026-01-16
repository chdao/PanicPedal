#include "PedalService.h"
#include "PairingService.h"
#include "../debug_format.h"
#include <string.h>
#include <stdarg.h>
#include <Arduino.h>
#include "../messages.h"

// Forward declarations
extern void debugPrint(const char* format, ...);
extern bool debugEnabled;
extern unsigned long bootTime;
extern int getSlotsNeeded(uint8_t pedalMode);

static PedalService* g_pedalService = nullptr;
static PairingService* g_pairingService = nullptr;
#ifdef PEDAL_SERVICE_HAS_LED
static LEDService* g_ledService = nullptr;
#endif

void pedalService_setPairingService(PairingService* pairingService) {
  g_pairingService = pairingService;
}

#ifdef PEDAL_SERVICE_HAS_LED
void pedalService_setLEDService(void* ledService) {
  g_ledService = (LEDService*)ledService;
}
#endif

void onPedalPress(char key) {
  if (!g_pedalService) return;
  
  debugPrint("T0: '%c' ▼", key);
  
  // If not paired, try to initiate pairing when pedal is pressed
  if (!pairingState_isPaired(g_pedalService->pairingState) && 
      g_pedalService->pairingState->receiverBeaconReceived && g_pairingService) {
    int slotsNeeded = getSlotsNeeded(g_pedalService->reader->pedalMode);
    if (g_pedalService->pairingState->discoveredAvailableSlots >= slotsNeeded) {
      debugPrint("Initiating pairing on pedal press...\n");
      pairingService_initiatePairing(g_pairingService, 
                                     g_pedalService->pairingState->discoveredReceiverMAC,
                                     g_pedalService->pairingState->discoveredReceiverChannel);
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
  
  debugPrint("T0: '%c' ▲", key);
  
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

bool pedalService_update(PedalService* service) {
  bool hasWork = pedalReader_needsUpdate(service->reader);
  if (hasWork) {
    pedalReader_update(service->reader, onPedalPress, onPedalRelease);
  }
  return hasWork;
}

void pedalService_sendPedalEvent(PedalService* service, char key, bool pressed) {
  if (!pairingState_isPaired(service->pairingState)) {
    return;
  }
  
  struct_message msg = {
    .msgType = MSG_PEDAL_EVENT,
    .key = key,
    .pressed = pressed,
    .pedalMode = service->reader->pedalMode
  };
  
  bool sent = espNowTransport_send(service->transport, service->pairingState->pairedReceiverMAC, 
                                   (uint8_t*)&msg, sizeof(msg));
  
  if (debugEnabled && !sent) {
    debugPrint("Pedal event send FAILED: key='%c', %s\n", key, pressed ? "PRESSED" : "RELEASED");
  }
  
  if (service->lastActivityTime) {
    *service->lastActivityTime = millis();
  }
}
