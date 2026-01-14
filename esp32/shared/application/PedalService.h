#ifndef PEDAL_SERVICE_H
#define PEDAL_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "../domain/PedalReader.h"
#include "../domain/PairingState.h"
#include "../infrastructure/EspNowTransport.h"
#include "../messages.h"
#include "PairingService.h"

typedef struct {
  PedalReader* reader;
  PairingState* pairingState;
  EspNowTransport* transport;
  unsigned long* lastActivityTime;
  void (*onActivity)();
} PedalService;

void pedalService_init(PedalService* service, PedalReader* reader, PairingState* pairingState, 
                       EspNowTransport* transport, unsigned long* lastActivityTime);
void pedalService_setPairingService(PairingService* pairingService);
bool pedalService_update(PedalService* service);  // Returns true if work was done (debouncing, etc.)
void pedalService_sendPedalEvent(PedalService* service, char key, bool pressed);

// Optional LED service support (only available if LEDService.h exists in project)
#ifdef PEDAL_SERVICE_HAS_LED
void pedalService_setLEDService(void* ledService);
#endif

#endif // PEDAL_SERVICE_H
