#ifndef WAKEUP_HANDLER_H
#define WAKEUP_HANDLER_H

#include <Arduino.h>
#include "esp_sleep.h"
#include "../domain/PairingState.h"
#include "../domain/PedalReader.h"
#include "../application/PedalService.h"
#include "../infrastructure/EspNowTransport.h"

struct WakeupInfo {
  bool wokeFromDeepSleep;
  bool pedalPressedOnWakeup;
  bool pedal1PressedOnWakeup;
  bool pedal2PressedOnWakeup;
};

class WakeupHandler {
public:
  static WakeupInfo detectWakeup();
  static void logWakeupCause(esp_sleep_wakeup_cause_t wakeupCause);
  static void handleWakeupPedalEvents(WakeupInfo* info, PedalReader* pedalReader, 
                                       PairingState* pairingState, PedalService* pedalService,
                                       EspNowTransport* transport, uint8_t detectedMode);
  static void checkPedalReleaseAfterInterrupts(WakeupInfo* info, PedalReader* pedalReader,
                                                PairingState* pairingState, PedalService* pedalService,
                                                uint8_t detectedMode);
};

#endif // WAKEUP_HANDLER_H
