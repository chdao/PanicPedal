#ifndef LED_SERVICE_H
#define LED_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

// LED status states
typedef enum {
  LED_STATE_OFF = 0,
  LED_STATE_PAIRING,      // Blinking blue - searching for receiver
  LED_STATE_PAIRED,       // Solid green - paired with receiver
  LED_STATE_PEDAL_PRESS,  // Brief flash - pedal pressed
  LED_STATE_ERROR         // Red - error state
} LEDState;

typedef struct {
  uint8_t dinPin;
  uint8_t clkPin;
  LEDState state;
  unsigned long lastUpdate;
  bool blinkState;
  unsigned long lastBlinkToggle;
} LEDService;

void ledService_init(LEDService* service, uint8_t dinPin, uint8_t clkPin);
void ledService_setState(LEDService* service, LEDState state);
void ledService_update(LEDService* service, unsigned long currentTime);
void ledService_setColor(LEDService* service, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

#endif // LED_SERVICE_H
