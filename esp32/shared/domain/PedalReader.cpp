#include "PedalReader.h"
#include <Arduino.h>
#include "../config.h"

// Global pointer to PedalReader instance (needed for ISR)
PedalReader* g_pedalReader = nullptr;

// Interrupt Service Routines - minimal ISRs that only set flags
// GPIO reading and debouncing happen in main loop to avoid watchdog timeouts
void IRAM_ATTR pedal1ISR() {
  if (g_pedalReader != nullptr) {
    volatile bool* flag = &g_pedalReader->pedal1State.interruptFlag;
    if (!*flag) {
      *flag = true;
    }
  }
}

void IRAM_ATTR pedal2ISR() {
  if (g_pedalReader != nullptr) {
    volatile bool* flag = &g_pedalReader->pedal2State.interruptFlag;
    if (!*flag) {
      *flag = true;
    }
  }
}

void pedalReader_init(PedalReader* reader, uint8_t pedal1Pin, uint8_t pedal2Pin, uint8_t pedalMode) {
  reader->pedal1Pin = pedal1Pin;
  reader->pedal2Pin = pedal2Pin;
  reader->pedalMode = pedalMode;
  
  // Initialize pedal 1 state
  reader->pedal1State.lastState = HIGH;
  reader->pedal1State.interruptFlag = false;
  reader->pedal1State.lastDebounceTime = 0;
  reader->interruptAttached1 = false;
  
  // Initialize pedal 2 state
  reader->pedal2State.lastState = HIGH;
  reader->pedal2State.interruptFlag = false;
  reader->pedal2State.lastDebounceTime = 0;
  reader->interruptAttached2 = false;
  
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
  return reader->pedal1State.interruptFlag || 
         (reader->pedalMode == 0 && reader->pedal2State.interruptFlag);
}

void pedalReader_processPedal(PedalReader* reader, uint8_t pin, PedalState* state, char key, 
                               void (*onPedalPress)(char), void (*onPedalRelease)(char),
                               bool* interruptAttached) {
  if (!state->interruptFlag) {
    return;
  }
  
  state->interruptFlag = false;
  
  // Read GPIO state (not done in ISR to avoid watchdog timeout)
  bool currentState = digitalRead(pin);
  unsigned long currentTime = millis();
  
  // Ignore if state didn't actually change (noise filtering)
  if (currentState == state->lastState) {
    return;
  }
  
  // Check debounce time
  if (currentTime - state->lastDebounceTime < DEBOUNCE_TIME_MS) {
    return;
  }
  
  // Valid state change - process transition
  state->lastDebounceTime = currentTime;
  state->lastState = currentState;
  
  if (currentState == LOW) {
    // Pedal pressed (HIGH -> LOW)
    if (onPedalPress) onPedalPress(key);
  } else {
    // Pedal released (LOW -> HIGH)
    if (onPedalRelease) onPedalRelease(key);
  }
}

void pedalReader_update(PedalReader* reader, void (*onPedalPress)(char key), void (*onPedalRelease)(char key)) {
  if (!pedalReader_needsUpdate(reader)) {
    return;
  }
  
  pedalReader_processPedal(reader, reader->pedal1Pin, &reader->pedal1State, '1', 
                          onPedalPress, onPedalRelease, &reader->interruptAttached1);
  
  if (reader->pedalMode == 0) {  // DUAL mode
    pedalReader_processPedal(reader, reader->pedal2Pin, &reader->pedal2State, '2', 
                            onPedalPress, onPedalRelease, &reader->interruptAttached2);
  }
}
