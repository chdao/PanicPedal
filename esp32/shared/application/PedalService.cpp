#include "PedalService.h"
#include "PairingService.h"
#include "../domain/PedalSlots.h"
#include "../debug_format.h"
#include <string.h>
#include <stdarg.h>
#include <Arduino.h>
#include "../messages.h"

// Optional LED service support
// LEDService.h is included by the project's .ino file before this file
// When PEDAL_SERVICE_HAS_LED is defined, LEDService is already typedef'd, so we don't need to forward declare
// The type is already available from the included header

// Forward declaration - debugPrint is defined in transmitter.ino
extern void debugPrint(const char* format, ...);
extern bool debugEnabled;
extern unsigned long bootTime;

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
  
  // Log pedal press with standardized format (T0: 'key' ▼)
  debugPrint("T0: '%c' ▼", key);
  
  // If not paired and we have a discovered receiver (from beacon or MSG_ALIVE), initiate pairing
  if (!pairingState_isPaired(g_pedalService->pairingState) && 
      g_pedalService->pairingState->receiverBeaconReceived && g_pairingService) {
    // Determine slots needed based on pedal mode (0=DUAL needs 2, 1=SINGLE needs 1)
    int slotsNeeded = getSlotsNeeded(g_pedalService->reader->pedalMode);
    if (g_pedalService->pairingState->discoveredAvailableSlots >= slotsNeeded) {
      debugPrint("Initiating pairing on pedal press...\n");
      // Use the stored channel from beacon or MSG_ALIVE
      pairingService_initiatePairing(g_pairingService, 
                                     g_pedalService->pairingState->discoveredReceiverMAC,
                                     g_pedalService->pairingState->discoveredReceiverChannel);
    }
  }
  
  // Send pedal event if paired
  if (pairingState_isPaired(g_pedalService->pairingState)) {
    pedalService_sendPedalEvent(g_pedalService, key, true);
  }
  
#ifdef PEDAL_SERVICE_HAS_LED
  // LED stays off during pedal press (battery saving)
#endif
  
  if (g_pedalService->onActivity) {
    g_pedalService->onActivity();
  }
}

void onPedalRelease(char key) {
  if (!g_pedalService) return;
  
  // Log pedal release with standardized format (T0: 'key' ▲)
  debugPrint("T0: '%c' ▲", key);
  
  // Send pedal event if paired
  if (pairingState_isPaired(g_pedalService->pairingState)) {
    pedalService_sendPedalEvent(g_pedalService, key, false);
  }
  
#ifdef PEDAL_SERVICE_HAS_LED
  // LED stays off after pedal release (battery saving)
#endif
  
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
  // Only process if there's work to do (interrupt occurred)
  bool hasWork = pedalReader_needsUpdate(service->reader);
  if (hasWork) {
    pedalReader_update(service->reader, onPedalPress, onPedalRelease);
  }
  return hasWork;  // Return true if work was done (for dynamic delay)
}

void pedalService_sendPedalEvent(PedalService* service, char key, bool pressed) {
  if (!pairingState_isPaired(service->pairingState)) {
    return;  // Not paired
  }
  
  // Note: pedalMode field is not used by receiver (it uses transmitterManager data)
  // but we set it for consistency
  // Use designated initializer for better code generation
  struct_message msg = {
    .msgType = MSG_PEDAL_EVENT,
    .key = key,
    .pressed = pressed,
    .pedalMode = service->reader->pedalMode
  };
  
  bool sent = espNowTransport_send(service->transport, service->pairingState->pairedReceiverMAC, 
                                   (uint8_t*)&msg, sizeof(msg));
  
  // Only log failures (successful sends are routine)
  if (debugEnabled && !sent) {
    debugPrint("Pedal event send FAILED: key='%c', %s\n", key, pressed ? "PRESSED" : "RELEASED");
  }
  
  if (service->lastActivityTime) {
    *service->lastActivityTime = millis();
  }
}
