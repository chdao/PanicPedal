#ifndef PEDAL_READER_H
#define PEDAL_READER_H

#include <stdint.h>
#include <stdbool.h>

#define DEBOUNCE_DELAY 20  // milliseconds

typedef struct {
  bool lastState;
  unsigned long debounceTime;
  bool debouncing;
} PedalState;

typedef struct {
  PedalState pedal1State;
  PedalState pedal2State;
  uint8_t pedal1Pin;
  uint8_t pedal2Pin;
  uint8_t pedalMode;  // 0=DUAL, 1=SINGLE
} PedalReader;

void pedalReader_init(PedalReader* reader, uint8_t pedal1Pin, uint8_t pedal2Pin, uint8_t pedalMode);
bool pedalReader_checkPedal(PedalReader* reader, uint8_t pin, PedalState* state);
void pedalReader_update(PedalReader* reader, void (*onPedalPress)(char key), void (*onPedalRelease)(char key));

#endif // PEDAL_READER_H

