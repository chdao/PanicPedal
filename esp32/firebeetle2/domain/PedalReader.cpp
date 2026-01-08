#include "PedalReader.h"
#include <Arduino.h>

void pedalReader_init(PedalReader* reader, uint8_t pedal1Pin, uint8_t pedal2Pin, uint8_t pedalMode) {
  reader->pedal1Pin = pedal1Pin;
  reader->pedal2Pin = pedal2Pin;
  reader->pedalMode = pedalMode;
  reader->pedal1State.lastState = HIGH;
  reader->pedal1State.debounceTime = 0;
  reader->pedal1State.debouncing = false;
  reader->pedal2State.lastState = HIGH;
  reader->pedal2State.debounceTime = 0;
  reader->pedal2State.debouncing = false;
  
  pinMode(pedal1Pin, INPUT_PULLUP);
  if (pedalMode == 0) {  // DUAL mode
    pinMode(pedal2Pin, INPUT_PULLUP);
  }
}

bool pedalReader_checkPedal(PedalReader* reader, uint8_t pin, PedalState* state) {
  bool currentState = digitalRead(pin);
  
  if (currentState == state->lastState && !state->debouncing) {
    return false;  // No change
  }
  
  unsigned long currentTime = millis();
  
  if (currentState == LOW && state->lastState == HIGH) {
    if (!state->debouncing) {
      state->debounceTime = currentTime;
      state->debouncing = true;
      return false;
    } else if (currentTime - state->debounceTime >= DEBOUNCE_DELAY) {
      if (digitalRead(pin) == LOW) {
        state->lastState = LOW;
        state->debouncing = false;
        return true;  // Pressed
      }
    }
  } else if (currentState == HIGH && state->lastState == LOW) {
    state->lastState = HIGH;
    state->debouncing = false;
    return true;  // Released
  } else if (currentState == HIGH && state->debouncing) {
    state->debouncing = false;
  }
  
  return false;
}

void pedalReader_update(PedalReader* reader, void (*onPedalPress)(char key), void (*onPedalRelease)(char key)) {
  if (pedalReader_checkPedal(reader, reader->pedal1Pin, &reader->pedal1State)) {
    if (reader->pedal1State.lastState == LOW) {
      if (onPedalPress) onPedalPress('1');
    } else {
      if (onPedalRelease) onPedalRelease('1');
    }
  }
  
  if (reader->pedalMode == 0) {  // DUAL mode
    if (pedalReader_checkPedal(reader, reader->pedal2Pin, &reader->pedal2State)) {
      if (reader->pedal2State.lastState == LOW) {
        if (onPedalPress) onPedalPress('2');
      } else {
        if (onPedalRelease) onPedalRelease('2');
      }
    }
  }
}

