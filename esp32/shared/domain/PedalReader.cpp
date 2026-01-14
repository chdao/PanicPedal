#include "PedalReader.h"
#include <Arduino.h>

// Global pointer to PedalReader instance (needed for ISR)
PedalReader* g_pedalReader = nullptr;

// Debounce time in milliseconds (ignore interrupts that occur within this time)
#define DEBOUNCE_TIME_MS 50

// Interrupt Service Routines - set flag only, read GPIO in main loop to avoid watchdog timeout
// digitalRead() can be slow in ISRs and cause watchdog issues
// Note: We don't debounce in ISR to keep it fast - debouncing happens in main loop
// CRITICAL: These ISRs must be extremely fast - any delay can cause watchdog timeout
// CRITICAL: micros() and other time functions are too slow for ISRs - use simple flag check only
// CRITICAL: CHANGE mode fires on both edges - we only process actual state changes in main loop
void IRAM_ATTR pedal1ISR() {
  // Ultra-minimal ISR - just set flag if not already set
  // No time calculations, no function calls, no GPIO reads - just a simple flag check and set
  // The main loop will read GPIO and only process if state actually changed
  // This prevents noise while pedal is held from causing watchdog timeouts
  if (g_pedalReader != nullptr) {
    // Use volatile pointer to ensure compiler doesn't optimize away the check
    volatile bool* flag = &g_pedalReader->pedal1State.interruptFlag;
    if (!*flag) {
      *flag = true;
    }
  }
  // If flag already set, ignore this interrupt - main loop will process it eventually
  // This prevents ISR from being called repeatedly while main loop is processing
}

void IRAM_ATTR pedal2ISR() {
  // Ultra-minimal ISR - just set flag if not already set
  // No time calculations, no function calls, no GPIO reads - just a simple flag check and set
  // The main loop will read GPIO and only process if state actually changed
  // This prevents noise while pedal is held from causing watchdog timeouts
  if (g_pedalReader != nullptr) {
    // Use volatile pointer to ensure compiler doesn't optimize away the check
    volatile bool* flag = &g_pedalReader->pedal2State.interruptFlag;
    if (!*flag) {
      *flag = true;
    }
  }
  // If flag already set, ignore this interrupt - main loop will process it eventually
  // This prevents ISR from being called repeatedly while main loop is processing
}

void pedalReader_init(PedalReader* reader, uint8_t pedal1Pin, uint8_t pedal2Pin, uint8_t pedalMode) {
  reader->pedal1Pin = pedal1Pin;
  reader->pedal2Pin = pedal2Pin;
  reader->pedalMode = pedalMode;
  reader->pedal1State.lastState = HIGH;
  reader->pedal1State.interruptFlag = false;
  reader->pedal1State.lastDebounceTime = 0;
  reader->interruptAttached1 = false;  // Will be set when interrupt is attached
  reader->pedal2State.lastState = HIGH;
  reader->pedal2State.interruptFlag = false;
  reader->pedal2State.lastDebounceTime = 0;
  reader->interruptAttached2 = false;  // Will be set when interrupt is attached
  
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
  // Need to update if interrupt flag is set (new interrupt occurred)
  if (reader->pedal1State.interruptFlag) {
    return true;
  }
  if (reader->pedalMode == 0 && reader->pedal2State.interruptFlag) {
    return true;
  }
  return false;
}

void pedalReader_processPedal(PedalReader* reader, uint8_t pin, PedalState* state, char key, 
                               void (*onPedalPress)(char), void (*onPedalRelease)(char),
                               bool* interruptAttached) {
  if (state->interruptFlag) {
    // Clear flag immediately
    state->interruptFlag = false;
    
    // Read GPIO state now (not in ISR to avoid watchdog timeout)
    bool currentState = digitalRead(pin);
    unsigned long currentTime = millis();
    
    // Only process if state actually changed (prevents processing noise as events)
    bool stateChanged = (currentState != state->lastState);
    
    if (!stateChanged) {
      // State didn't change - this is noise, ignore it
      return;
    }
    
    // State changed - check debounce time
    if (currentTime - state->lastDebounceTime < DEBOUNCE_TIME_MS) {
      // Too soon since last state change - likely bounce, ignore it
      return;
    }
    
    // Valid state change after debounce - process the transition
    state->lastDebounceTime = currentTime;
    
    if (currentState == LOW && state->lastState == HIGH) {
      // Transition from HIGH to LOW (pedal pressed)
      state->lastState = LOW;
      if (onPedalPress) onPedalPress(key);
    } else if (currentState == HIGH && state->lastState == LOW) {
      // Transition from LOW to HIGH (pedal released)
      state->lastState = HIGH;
      if (onPedalRelease) onPedalRelease(key);
    }
  }
}

void pedalReader_update(PedalReader* reader, void (*onPedalPress)(char key), void (*onPedalRelease)(char key)) {
  // Only process if there's work to do (interrupt occurred)
  if (!pedalReader_needsUpdate(reader)) {
    return;  // No work needed - exit immediately
  }
  
  // Process pedal 1
  pedalReader_processPedal(reader, reader->pedal1Pin, &reader->pedal1State, '1', onPedalPress, onPedalRelease, &reader->interruptAttached1);
  
  // Process pedal 2 (only in DUAL mode)
  if (reader->pedalMode == 0) {
    pedalReader_processPedal(reader, reader->pedal2Pin, &reader->pedal2State, '2', onPedalPress, onPedalRelease, &reader->interruptAttached2);
  }
}
