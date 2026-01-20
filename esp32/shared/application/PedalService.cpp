#include "PedalService.h"
#include "PairingService.h"
#include "../debug_format.h"
#include "../domain/MacUtils.h"
#include <string.h>
#include <stdarg.h>
#include <Arduino.h>
#include "../messages.h"

// Forward declarations
extern void debugPrint(const char* format, ...);
#include "../infrastructure/DebugService.h"
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
  if (!pairingState_isPaired(g_pedalService->pairingState) && g_pairingService) {
    #ifdef DEBUG_ENABLED
    if (DEBUG_ENABLED && DebugService::isEnabled()) {
      DebugService::print("Pedal %c pressed but device NOT PAIRED - attempting to initiate pairing", key);
    }
    #endif
    
    // Try to initiate pairing with discovered receiver (from beacon)
    if (g_pedalService->pairingState->receiverBeaconReceived) {
      int slotsNeeded = getSlotsNeeded(g_pedalService->reader->pedalMode);
      if (g_pedalService->pairingState->discoveredAvailableSlots >= slotsNeeded) {
        #ifdef DEBUG_ENABLED
        if (DEBUG_ENABLED && DebugService::isEnabled()) {
          DebugService::print("Initiating pairing on pedal press (slots available: %d, needed: %d)", 
                             g_pedalService->pairingState->discoveredAvailableSlots, slotsNeeded);
        }
        #endif
        pairingService_initiatePairing(g_pairingService, 
                                       g_pedalService->pairingState->discoveredReceiverMAC,
                                       g_pedalService->pairingState->discoveredReceiverChannel);
      } else {
        #ifdef DEBUG_ENABLED
        if (DEBUG_ENABLED && DebugService::isEnabled()) {
          DebugService::print("Cannot initiate pairing - insufficient slots (available: %d, needed: %d)", 
                             g_pedalService->pairingState->discoveredAvailableSlots, slotsNeeded);
        }
        #endif
      }
    }
    // If no beacon received but we have a previously paired receiver MAC, try pairing with that
    else if (!macIsZero(g_pedalService->pairingState->pairedReceiverMAC)) {
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED && DebugService::isEnabled()) {
        DebugService::print("Initiating pairing on pedal press with previously paired receiver (no beacon received yet)");
      }
      #endif
      // Use channel 0 (ESP-NOW will use current WiFi channel)
      pairingService_initiatePairing(g_pairingService, 
                                     g_pedalService->pairingState->pairedReceiverMAC,
                                     0);
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
    // Process pedal interrupts (no debug logging - only log actual pedal events)
    pedalReader_update(service->reader, onPedalPress, onPedalRelease);
  }
  return hasWork;
}

void pedalService_sendPedalEvent(PedalService* service, char key, bool pressed) {
  if (!pairingState_isPaired(service->pairingState)) {
    return;
  }
  
  struct_message msg = {
    .deviceType = DEVICE_TYPE_TRANSMITTER,
    .msgType = MSG_PEDAL_EVENT,
    .key = key,
    .pressed = pressed,
    .pedalMode = service->reader->pedalMode
  };
  
  bool sent = espNowTransport_send(service->transport, service->pairingState->pairedReceiverMAC, 
                                   (uint8_t*)&msg, sizeof(msg));
  
  if (DebugService::isEnabled() && !sent) {
    debugPrint("Pedal event send FAILED: key='%c', %s\n", key, pressed ? "PRESSED" : "RELEASED");
  }
  
  if (service->lastActivityTime) {
    *service->lastActivityTime = millis();
  }
}
