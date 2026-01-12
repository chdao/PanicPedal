#include "PedalReader.h"
#include <Arduino.h>

// Global pointer to PedalReader instance (needed for ISR)
PedalReader* g_pedalReader = nullptr;

// Interrupt Service Routines
void IRAM_ATTR pedal1ISR() {
  if (g_pedalReader) {
    g_pedalReader->pedal1State.interruptFlag = true;
  }
}

void IRAM_ATTR pedal2ISR() {
  if (g_pedalReader) {
    g_pedalReader->pedal2State.interruptFlag = true;
  }
}

void pedalReader_init(PedalReader* reader, uint8_t pedal1Pin, uint8_t pedal2Pin, uint8_t pedalMode) {
  reader->pedal1Pin = pedal1Pin;
  reader->pedal2Pin = pedal2Pin;
  reader->pedalMode = pedalMode;
  reader->pedal1State.lastState = HIGH;
  reader->pedal1State.debounceTime = 0;
  reader->pedal1State.debouncing = false;
  reader->pedal1State.interruptFlag = false;
  reader->pedal2State.lastState = HIGH;
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

bool pedalReader_processInterrupt(PedalReader* reader, uint8_t pin, PedalState* state) {
  if (!state->interruptFlag) {
    return false;  // No interrupt occurred
  }
  
  state->interruptFlag = false;  // Clear flag
  bool currentState = digitalRead(pin);
  unsigned long currentTime = millis();
  bool stateChanged = false;
  
  // Handle state change with debouncing
  if (currentState == LOW && state->lastState == HIGH) {
    // Transition from HIGH to LOW (pedal pressed)
    if (!state->debouncing) {
      state->debounceTime = currentTime;
      state->debouncing = true;
    } else if (currentTime - state->debounceTime >= DEBOUNCE_DELAY) {
      // Debounce time elapsed, verify state is still LOW
      if (digitalRead(pin) == LOW) {
        state->lastState = LOW;
        state->debouncing = false;
        stateChanged = true;  // Press event confirmed
      }
    }
  } else if (currentState == HIGH && state->lastState == LOW) {
    // Transition from LOW to HIGH (pedal released)
    state->lastState = HIGH;
    state->debouncing = false;
    stateChanged = true;  // Release event confirmed
  } else if (currentState == HIGH && state->debouncing) {
    // Bounce detected, cancel debounce
    state->debouncing = false;
  }
  
  return stateChanged;
}

void pedalReader_update(PedalReader* reader, void (*onPedalPress)(char key), void (*onPedalRelease)(char key)) {
  // Process pedal 1 interrupt
  if (reader->pedal1State.interruptFlag) {
    bool lastStateBefore = reader->pedal1State.lastState;
    bool stateChanged = pedalReader_processInterrupt(reader, reader->pedal1Pin, &reader->pedal1State);
    
    // Trigger callback if state changed
    if (stateChanged) {
      if (reader->pedal1State.lastState == LOW) {
        if (onPedalPress) onPedalPress('1');
      } else {
        if (onPedalRelease) onPedalRelease('1');
      }
    }
  }
  
  // Process pedal 2 interrupt (only in DUAL mode)
  if (reader->pedalMode == 0 && reader->pedal2State.interruptFlag) {
    bool lastStateBefore = reader->pedal2State.lastState;
    bool stateChanged = pedalReader_processInterrupt(reader, reader->pedal2Pin, &reader->pedal2State);
    
    // Trigger callback if state changed
    if (stateChanged) {
      if (reader->pedal2State.lastState == LOW) {
        if (onPedalPress) onPedalPress('2');
      } else {
        if (onPedalRelease) onPedalRelease('2');
      }
    }
  }
}

