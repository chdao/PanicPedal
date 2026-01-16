#include "LEDService.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void ledService_init(LEDService* service, unsigned long bootTime) {
  service->bootTime = bootTime;
  service->lastLEDState = false;
  pixels.begin();
  pixels.clear();
  pixels.show();
}

void ledService_update(LEDService* service, unsigned long currentTime, bool gracePeriodDone, int slotsUsed) {
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  
  // LED ON: grace period active, not timed out, and slots available
  bool shouldBeOn = !gracePeriodDone && 
                    (timeSinceBoot < TRANSMITTER_TIMEOUT) && 
                    (slotsUsed < MAX_PEDAL_SLOTS);
  
  // Only update if state changed (power optimization)
  if (shouldBeOn != service->lastLEDState) {
    service->lastLEDState = shouldBeOn;
    
    if (shouldBeOn) {
      pixels.setPixelColor(0, pixels.Color(0, 0, 255));  // Blue
    } else {
      pixels.setPixelColor(0, pixels.Color(0, 0, 0));  // Off
    }
    pixels.show();
  }
}

