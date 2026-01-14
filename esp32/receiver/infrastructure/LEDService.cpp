#include "LEDService.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void ledService_init(LEDService* service, unsigned long bootTime) {
  service->bootTime = bootTime;
  pixels.begin();
  pixels.clear();
  pixels.show();
}

void ledService_update(LEDService* service, unsigned long currentTime, bool gracePeriodDone, int slotsUsed) {
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  
  // LED should be ON only when:
  // - Grace period is NOT done
  // - AND time since boot < timeout
  // - AND slots are not full
  bool shouldBeOn = !gracePeriodDone && 
                    (timeSinceBoot < TRANSMITTER_TIMEOUT) && 
                    (slotsUsed < MAX_PEDAL_SLOTS);
  
  if (shouldBeOn) {
    // Grace period active and slots available - set LED to blue
    pixels.setPixelColor(0, pixels.Color(0, 0, 255));
    pixels.show();
  } else {
    // After grace period, timeout, or slots full - turn LED off
    pixels.setPixelColor(0, pixels.Color(0, 0, 0));
    pixels.show();
  }
}

