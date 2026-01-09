#include "PedalService.h"
#include "../application/PairingService.h"
#include "../infrastructure/LEDService.h"
#include <string.h>
#include <stdarg.h>
#include <Arduino.h>
#include "../shared/messages.h"

// Forward declaration - debugPrint is defined in transmitter.ino
extern void debugPrint(const char* format, ...);
extern bool debugEnabled;

static PedalService* g_pedalService = nullptr;
static PairingService* g_pairingService = nullptr;
static LEDService* g_ledService = nullptr;

void pedalService_setPairingService(PairingService* pairingService) {
  g_pairingService = pairingService;
}

void onPedalPress(char key) {
  if (!g_pedalService) return;
  
  // Log pedal press
  if (debugEnabled) {
    if (pairingState_isPaired(g_pedalService->pairingState)) {
      debugPrint("Pedal %c PRESSED\n", key);
    } else {
      debugPrint("Pedal %c PRESSED (not paired)\n", key);
    }
  }
  
  // If not paired and we have a discovered receiver, initiate pairing
  if (!pairingState_isPaired(g_pedalService->pairingState) && 
      g_pedalService->pairingState->receiverBeaconReceived && g_pairingService) {
    // Determine slots needed based on pedal mode (0=DUAL needs 2, 1=SINGLE needs 1)
    int slotsNeeded = getSlotsNeeded(g_pedalService->reader->pedalMode);
    if (g_pedalService->pairingState->discoveredAvailableSlots >= slotsNeeded) {
      if (debugEnabled) {
        debugPrint("Initiating pairing...\n");
      }
      pairingService_initiatePairing(g_pairingService, 
                                     g_pedalService->pairingState->discoveredReceiverMAC, 0);
    }
  }
  
  // Send pedal event if paired
  if (pairingState_isPaired(g_pedalService->pairingState)) {
    pedalService_sendPedalEvent(g_pedalService, key, true);
  }
  
  // Flash LED when pedal is pressed
  if (g_ledService) {
    ledService_setState(g_ledService, LED_STATE_PEDAL_PRESS);
    ledService_update(g_ledService, millis());
  }
  
  if (g_pedalService->onActivity) {
    g_pedalService->onActivity();
  }
}

void onPedalRelease(char key) {
  if (!g_pedalService) return;
  
  // Log pedal release
  if (debugEnabled) {
    if (pairingState_isPaired(g_pedalService->pairingState)) {
      debugPrint("Pedal %c RELEASED\n", key);
    } else {
      debugPrint("Pedal %c RELEASED (not paired)\n", key);
    }
  }
  
  // Send pedal event if paired
  if (pairingState_isPaired(g_pedalService->pairingState)) {
    pedalService_sendPedalEvent(g_pedalService, key, false);
  }
  
  // Return LED to paired state when pedal is released
  if (g_ledService && pairingState_isPaired(g_pedalService->pairingState)) {
    ledService_setState(g_ledService, LED_STATE_PAIRED);
  }
  
  if (g_pedalService->onActivity) {
    g_pedalService->onActivity();
  }
}

void pedalService_init(PedalService* service, PedalReader* reader, PairingState* pairingState, 
                       EspNowTransport* transport, unsigned long* lastActivityTime, unsigned long bootTime) {
  service->reader = reader;
  service->pairingState = pairingState;
  service->transport = transport;
  service->lastActivityTime = lastActivityTime;
  service->bootTime = bootTime;
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

