#ifndef LED_SERVICE_H
#define LED_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#define LED_PIN 48
#define NUM_LEDS 1
#define TRANSMITTER_TIMEOUT 30000  // 30 seconds
#define MAX_PEDAL_SLOTS 2

typedef struct {
  unsigned long bootTime;
  uint32_t lastLEDColor;  // Track last LED color to avoid unnecessary updates
} LEDService;

void ledService_init(LEDService* service, unsigned long bootTime);
void ledService_update(LEDService* service, unsigned long currentTime, bool gracePeriodDone, int slotsUsed, bool inInitialWait);

#endif // LED_SERVICE_H

