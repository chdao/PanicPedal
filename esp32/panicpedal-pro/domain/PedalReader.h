#ifndef PEDAL_READER_H
#define PEDAL_READER_H

#include <stdint.h>
#include <stdbool.h>

#define DEBOUNCE_DELAY 20  // milliseconds

typedef struct {
  bool lastState;
  volatile bool newState;  // State read in ISR when interrupt occurs
  unsigned long debounceTime;
  bool debouncing;
  volatile bool interruptFlag;  // Set by ISR when interrupt occurs
} PedalState;

typedef struct {
  PedalState pedal1State;
  PedalState pedal2State;
  uint8_t pedal1Pin;
  uint8_t pedal2Pin;
  uint8_t pedalMode;  // 0=DUAL, 1=SINGLE
} PedalReader;

// ISR function pointers (must be accessible from interrupt context)
extern PedalReader* g_pedalReader;

// Interrupt Service Routines (IRAM_ATTR for ESP32)
void IRAM_ATTR pedal1ISR();
void IRAM_ATTR pedal2ISR();

void pedalReader_init(PedalReader* reader, uint8_t pedal1Pin, uint8_t pedal2Pin, uint8_t pedalMode);
bool pedalReader_needsUpdate(PedalReader* reader);  // Returns true if interrupt occurred or debouncing needs check
void pedalReader_update(PedalReader* reader, void (*onPedalPress)(char key), void (*onPedalRelease)(char key));

#endif // PEDAL_READER_H

