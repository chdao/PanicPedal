#ifndef PEDAL_SERVICE_H
#define PEDAL_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "../domain/PedalReader.h"
#include "../domain/PairingState.h"
#include "../infrastructure/EspNowTransport.h"
#include "../shared/messages.h"
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
void pedalService_setPairingService(PedalService* pairingService);
void pedalService_setLEDService(void* ledService);
void pedalService_update(PedalService* service);
void pedalService_sendPedalEvent(PedalService* service, char key, bool pressed);

#endif // PEDAL_SERVICE_H
