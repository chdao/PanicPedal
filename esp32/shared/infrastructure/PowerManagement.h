#ifndef POWER_MANAGEMENT_H
#define POWER_MANAGEMENT_H

#include "../domain/PedalReader.h"
#include "EspNowTransport.h"

class PowerManagement {
public:
  static void goToDeepSleep(PedalReader* pedalReader, EspNowTransport* transport, EspNowTransport** debugTransport);
  static bool shouldEnterDeepSleep(unsigned long lastActivityTime, unsigned long currentTime, PedalReader* pedalReader);
  static void resetActivityTimer(unsigned long* lastActivityTime, PedalReader* pedalReader);
};

#endif // POWER_MANAGEMENT_H
