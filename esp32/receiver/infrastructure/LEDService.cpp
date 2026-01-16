#include "LEDService.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <math.h>

Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void ledService_init(LEDService* service, unsigned long bootTime) {
  service->bootTime = bootTime;
  service->lastLEDColor = 0;  // Off initially
  pixels.begin();
  pixels.clear();
  pixels.show();
}

void ledService_update(LEDService* service, unsigned long currentTime, bool gracePeriodDone, int slotsUsed, bool inInitialWait) {
  unsigned long timeSinceBoot = currentTime - service->bootTime;
  
  // LED states:
  // - Green: During initial wait (waiting for known transmitters to respond)
  // - Blue (breathing): Grace period active, not timed out, and slots available
  // - Off: Grace period done or slots full
  
  uint32_t ledColor = 0;  // Off by default
  
  if (inInitialWait) {
    // Initial wait period - show solid green
    ledColor = pixels.Color(0, 255, 0);  // Green
  } else if (!gracePeriodDone && 
             (timeSinceBoot < TRANSMITTER_TIMEOUT) && 
             (slotsUsed < MAX_PEDAL_SLOTS)) {
    // Grace period active - show breathing blue
    // Use sine wave for smooth breathing effect (2 second cycle)
    float cycleTime = (currentTime % 2000) / 2000.0f;  // 0.0 to 1.0 over 2 seconds
    float brightness = (sin(cycleTime * 2.0f * PI) + 1.0f) / 2.0f;  // 0.0 to 1.0
    // Map to 30-255 brightness range (30 = minimum visible, 255 = full brightness)
    uint8_t blueValue = 30 + (uint8_t)(brightness * 225);
    ledColor = pixels.Color(0, 0, blueValue);
  } else {
    // Grace period done or slots full - off
    ledColor = pixels.Color(0, 0, 0);  // Off
  }
  
  // Always update during grace period (breathing animation) or when color changes
  bool isBreathing = (!gracePeriodDone && 
                      (timeSinceBoot < TRANSMITTER_TIMEOUT) && 
                      (slotsUsed < MAX_PEDAL_SLOTS) && 
                      !inInitialWait);
  
  if (isBreathing || ledColor != service->lastLEDColor) {
    service->lastLEDColor = ledColor;
    pixels.setPixelColor(0, ledColor);
    pixels.show();
  }
}

