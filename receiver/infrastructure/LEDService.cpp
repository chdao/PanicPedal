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

void ledService_update(LEDService* service, unsigned long currentTime) {
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  if (timeSinceBoot < TRANSMITTER_TIMEOUT) {
    // Grace period - set LED to blue
    pixels.setPixelColor(0, pixels.Color(0, 0, 255));
    pixels.show();
  } else {
    // After grace period - turn LED off
    pixels.setPixelColor(0, pixels.Color(0, 0, 0));
    pixels.show();
  }
}

