#include "LEDService.h"
#include <Arduino.h>

// APA102 protocol constants
#define APA102_START_FRAME 0x00000000
#define APA102_END_FRAME 0xFFFFFFFF

// Send a byte via bit-banging SPI for APA102
void apa102_sendByte(uint8_t dinPin, uint8_t clkPin, uint8_t data) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(dinPin, (data >> i) & 0x01);
    digitalWrite(clkPin, HIGH);
    delayMicroseconds(1);  // Small delay for timing
    digitalWrite(clkPin, LOW);
    delayMicroseconds(1);
  }
}

// Send APA102 start frame (32 zeros)
void apa102_sendStartFrame(uint8_t dinPin, uint8_t clkPin) {
  for (int i = 0; i < 4; i++) {
    apa102_sendByte(dinPin, clkPin, 0x00);
  }
}

// Send APA102 end frame (enough to latch the data)
void apa102_sendEndFrame(uint8_t dinPin, uint8_t clkPin) {
  // Send enough zeros to latch (typically 32+ bits)
  for (int i = 0; i < 4; i++) {
    apa102_sendByte(dinPin, clkPin, 0x00);
  }
}

// Send color data to APA102 LED
// Format: 111 + 5-bit brightness, then B, G, R (8 bits each)
void apa102_sendLED(uint8_t dinPin, uint8_t clkPin, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  // Brightness is 5-bit (0-31), but we'll use 0-255 and scale to 0-31
  uint8_t scaledBrightness = (brightness >> 3) & 0x1F;  // Scale 0-255 to 0-31
  uint8_t brightnessByte = 0xE0 | scaledBrightness;  // 111 + 5-bit brightness
  
  apa102_sendByte(dinPin, clkPin, brightnessByte);
  apa102_sendByte(dinPin, clkPin, b);
  apa102_sendByte(dinPin, clkPin, g);
  apa102_sendByte(dinPin, clkPin, r);
}

void ledService_init(LEDService* service, uint8_t dinPin, uint8_t clkPin) {
  service->dinPin = dinPin;
  service->clkPin = clkPin;
  service->state = LED_STATE_OFF;
  service->lastUpdate = 0;
  service->blinkState = false;
  service->lastBlinkToggle = 0;
  
  // Configure pins as outputs
  pinMode(dinPin, OUTPUT);
  pinMode(clkPin, OUTPUT);
  
  // Initialize pins to LOW
  digitalWrite(dinPin, LOW);
  digitalWrite(clkPin, LOW);
  
  // Turn LED off initially
  ledService_setColor(service, 0, 0, 0, 0);
}

void ledService_setState(LEDService* service, LEDState state) {
  service->state = state;
  service->lastBlinkToggle = millis();
  service->blinkState = false;
}

void ledService_setColor(LEDService* service, uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  apa102_sendStartFrame(service->dinPin, service->clkPin);
  apa102_sendLED(service->dinPin, service->clkPin, r, g, b, brightness);
  apa102_sendEndFrame(service->dinPin, service->clkPin);
}

void ledService_update(LEDService* service, unsigned long currentTime) {
  unsigned long elapsed = currentTime - service->lastUpdate;
  service->lastUpdate = currentTime;
  
  switch (service->state) {
    case LED_STATE_OFF:
      ledService_setColor(service, 0, 0, 0, 0);
      break;
      
    case LED_STATE_PAIRING:
      // Blink blue every 500ms
      if (currentTime - service->lastBlinkToggle > 500) {
        service->blinkState = !service->blinkState;
        service->lastBlinkToggle = currentTime;
        if (service->blinkState) {
          ledService_setColor(service, 0, 0, 255, 128);  // Blue, medium brightness
        } else {
          ledService_setColor(service, 0, 0, 0, 0);  // Off
        }
      }
      break;
      
    case LED_STATE_PAIRED:
      // Solid green
      ledService_setColor(service, 0, 255, 0, 128);  // Green, medium brightness
      break;
      
    case LED_STATE_PEDAL_PRESS:
      // Brief white flash (100ms)
      if (elapsed < 100) {
        ledService_setColor(service, 255, 255, 255, 200);  // White, bright
      } else {
        // Return to previous state (paired or off)
        if (service->state == LED_STATE_PEDAL_PRESS) {
          service->state = LED_STATE_PAIRED;  // Default back to paired
        }
      }
      break;
      
    case LED_STATE_ERROR:
      // Blink red every 250ms
      if (currentTime - service->lastBlinkToggle > 250) {
        service->blinkState = !service->blinkState;
        service->lastBlinkToggle = currentTime;
        if (service->blinkState) {
          ledService_setColor(service, 255, 0, 0, 200);  // Red, bright
        } else {
          ledService_setColor(service, 0, 0, 0, 0);  // Off
        }
      }
      break;
  }
}
