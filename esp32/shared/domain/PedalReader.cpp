#include "PedalReader.h"
#include <Arduino.h>

// Global pointer to PedalReader instance (needed for ISR)
PedalReader* g_pedalReader = nullptr;

// Interrupt Service Routines - read GPIO state immediately in ISR
void IRAM_ATTR pedal1ISR() {
  if (g_pedalReader) {
    // Read GPIO state once in ISR - no need to read again later
    g_pedalReader->pedal1State.newState = digitalRead(g_pedalReader->pedal1Pin);
    g_pedalReader->pedal1State.interruptFlag = true;
  }
}

void IRAM_ATTR pedal2ISR() {
  if (g_pedalReader) {
    // Read GPIO state once in ISR - no need to read again later
    g_pedalReader->pedal2State.newState = digitalRead(g_pedalReader->pedal2Pin);
    g_pedalReader->pedal2State.interruptFlag = true;
  }
}

void pedalReader_init(PedalReader* reader, uint8_t pedal1Pin, uint8_t pedal2Pin, uint8_t pedalMode) {
  reader->pedal1Pin = pedal1Pin;
  reader->pedal2Pin = pedal2Pin;
  reader->pedalMode = pedalMode;
  reader->pedal1State.lastState = HIGH;
  reader->pedal1State.newState = HIGH;
  reader->pedal1State.debounceTime = 0;
  reader->pedal1State.debouncing = false;
  reader->pedal1State.interruptFlag = false;
  reader->pedal2State.lastState = HIGH;
  reader->pedal2State.newState = HIGH;
  reader->pedal2State.debounceTime = 0;
  reader->pedal2State.debouncing = false;
  reader->pedal2State.interruptFlag = false;
  
  // Set global pointer for ISR access
  g_pedalReader = reader;
  
  // Configure pins as inputs with pull-ups
  pinMode(pedal1Pin, INPUT_PULLUP);
  if (pedalMode == 0) {  // DUAL mode
    pinMode(pedal2Pin, INPUT_PULLUP);
  }
  
  // Read initial state
  reader->pedal1State.lastState = digitalRead(pedal1Pin);
  if (pedalMode == 0) {
    reader->pedal2State.lastState = digitalRead(pedal2Pin);
  }
}

bool pedalReader_needsUpdate(PedalReader* reader) {
  // Need to update if:
  // 1. Interrupt flag is set (new interrupt occurred)
  // 2. Debouncing is in progress and debounce time may have elapsed
  if (reader->pedal1State.interruptFlag || reader->pedal1State.debouncing) {
    return true;
  }
  if (reader->pedalMode == 0 && (reader->pedal2State.interruptFlag || reader->pedal2State.debouncing)) {
    return true;
  }
  return false;
}

void pedalReader_processPedal(PedalReader* reader, uint8_t pin, PedalState* state, char key, 
                               void (*onPedalPress)(char), void (*onPedalRelease)(char)) {
  unsigned long currentTime = millis();
  bool stateChanged = false;
  
  if (state->interruptFlag) {
    // New interrupt occurred - use state read in ISR
    state->interruptFlag = false;
    bool currentState = state->newState;
    
    if (currentState == LOW && state->lastState == HIGH) {
      // Transition from HIGH to LOW (pedal pressed) - start debouncing
      state->debounceTime = currentTime;
      state->debouncing = true;
    } else if (currentState == HIGH && state->lastState == LOW) {
      // Transition from LOW to HIGH (pedal released) - immediate, no debounce needed
      state->lastState = HIGH;
      state->debouncing = false;
      stateChanged = true;
      if (onPedalRelease) onPedalRelease(key);
    } else if (currentState == HIGH && state->debouncing) {
      // Bounce detected during press - cancel debounce
      state->debouncing = false;
    }
  } else if (state->debouncing) {
    // Check if debounce time has elapsed
    if (currentTime - state->debounceTime >= DEBOUNCE_DELAY) {
      // Debounce time elapsed - verify state is still LOW (one final read)
      if (digitalRead(pin) == LOW) {
        state->lastState = LOW;
        state->debouncing = false;
        stateChanged = true;
        if (onPedalPress) onPedalPress(key);
      } else {
        // State changed back to HIGH - bounce, cancel
        state->debouncing = false;
      }
    }
  }
}

void pedalReader_update(PedalReader* reader, void (*onPedalPress)(char key), void (*onPedalRelease)(char key)) {
  // Only process if there's work to do (interrupt occurred or debouncing needs check)
  if (!pedalReader_needsUpdate(reader)) {
    return;  // No work needed - exit immediately
  }
  
  // Process pedal 1
  pedalReader_processPedal(reader, reader->pedal1Pin, &reader->pedal1State, '1', onPedalPress, onPedalRelease);
  
  // Process pedal 2 (only in DUAL mode)
  if (reader->pedalMode == 0) {
    pedalReader_processPedal(reader, reader->pedal2Pin, &reader->pedal2State, '2', onPedalPress, onPedalRelease);
  }
}
