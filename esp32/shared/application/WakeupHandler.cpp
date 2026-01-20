#include "WakeupHandler.h"
#include "../transmitter/TransmitterConfig.h"
#include "../infrastructure/DebugService.h"
#include "../domain/MacUtils.h"
#include "esp_sleep.h"

WakeupInfo WakeupHandler::detectWakeup() {
  WakeupInfo info = {false, false, false, false};
  
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  info.wokeFromDeepSleep = (wakeupCause == ESP_SLEEP_WAKEUP_EXT1 || wakeupCause == ESP_SLEEP_WAKEUP_EXT0);
  
  if (info.wokeFromDeepSleep) {
    // Always check both pedals - wake on any pedal press
    pinMode(PEDAL_1_PIN, INPUT_PULLUP);
    pinMode(PEDAL_2_PIN, INPUT_PULLUP);
    delay(10);  // Allow pin to stabilize
    
    bool pedal1State = digitalRead(PEDAL_1_PIN);
    bool pedal2State = digitalRead(PEDAL_2_PIN);
    info.pedalPressedOnWakeup = (pedal1State == LOW) || (pedal2State == LOW);
    info.pedal1PressedOnWakeup = (pedal1State == LOW);
    info.pedal2PressedOnWakeup = (pedal2State == LOW);
    
    // Log which GPIO triggered wakeup if EXT1
    if (wakeupCause == ESP_SLEEP_WAKEUP_EXT1) {
      uint64_t gpioMask = esp_sleep_get_ext1_wakeup_status();
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        Serial.printf("EXT1 wakeup mask: 0x%llx\n", gpioMask);
      }
      #endif
    }
  }
  
  return info;
}

void WakeupHandler::logWakeupCause(esp_sleep_wakeup_cause_t wakeupCause) {
  #ifdef DEBUG_ENABLED
  if (!DEBUG_ENABLED) return;
  
  switch (wakeupCause) {
    case ESP_SLEEP_WAKEUP_EXT0: {
      Serial.println("Wakeup cause: EXT0 (legacy - should use EXT1)");
      pinMode(PEDAL_1_PIN, INPUT_PULLUP);
      pinMode(PEDAL_2_PIN, INPUT_PULLUP);
      delay(10);
      bool pedal1State = digitalRead(PEDAL_1_PIN);
      bool pedal2State = digitalRead(PEDAL_2_PIN);
      Serial.printf("  -> PEDAL_1_PIN=%s, PEDAL_2_PIN=%s\n", 
                   pedal1State == HIGH ? "HIGH" : "LOW",
                   pedal2State == HIGH ? "HIGH" : "LOW");
      break;
    }
    case ESP_SLEEP_WAKEUP_EXT1: {
      // EXT1 wakeup - wake on any pedal press
      uint64_t gpioMask = esp_sleep_get_ext1_wakeup_status();
      pinMode(PEDAL_1_PIN, INPUT_PULLUP);
      pinMode(PEDAL_2_PIN, INPUT_PULLUP);
      delay(10);
      
      bool pedal1State = digitalRead(PEDAL_1_PIN);
      bool pedal2State = digitalRead(PEDAL_2_PIN);
      
      Serial.printf("Wakeup cause: EXT1 - GPIO mask: 0x%llx, PEDAL_1_PIN=%s, PEDAL_2_PIN=%s\n",
                   gpioMask,
                   pedal1State == HIGH ? "HIGH" : "LOW",
                   pedal2State == HIGH ? "HIGH" : "LOW");
      
      if (gpioMask & (1ULL << PEDAL_1_PIN)) {
        Serial.println("  -> PEDAL_1_PIN triggered wakeup");
      }
      if (gpioMask & (1ULL << PEDAL_2_PIN)) {
        Serial.println("  -> PEDAL_2_PIN triggered wakeup");
      }
      break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup cause: TIMER");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Wakeup cause: TOUCHPAD");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("Wakeup cause: ULP");
      break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
      Serial.println("Wakeup cause: UNDEFINED (power-on reset)");
      break;
  }
  #endif
}

void WakeupHandler::handleWakeupPedalEvents(WakeupInfo* info, PedalReader* pedalReader, 
                                             PairingState* pairingState, PedalService* pedalService,
                                             EspNowTransport* transport, uint8_t detectedMode) {
  if (!info->wokeFromDeepSleep || !info->pedalPressedOnWakeup || !pairingState_isPaired(pairingState)) {
    return;
  }
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    DebugService::print("Woke from deep sleep with pedal pressed - sending pedal event");
  }
  #endif
  
  // Verify ESP-NOW is initialized before sending
  if (!transport->initialized) {
    #ifdef DEBUG_ENABLED
    if (DEBUG_ENABLED) {
      DebugService::print("ESP-NOW not initialized - cannot send pedal event");
    }
    #endif
    return;
  }
  
  // Ensure peer is added before sending (critical for reliable delivery)
  uint8_t* receiverMAC = pairingState->pairedReceiverMAC;
  bool peerAdded = espNowTransport_addPeer(transport, receiverMAC, 0);
  delay(20);  // Delay to ensure peer is ready
  
  if (!peerAdded) {
    #ifdef DEBUG_ENABLED
    if (DEBUG_ENABLED) {
      DebugService::print("Failed to add peer - cannot send pedal event");
    }
    #endif
    return;
  }
  
  // Send pedal press events for whichever pedals were pressed on wakeup
  if (info->pedal1PressedOnWakeup) {
    pedalService_sendPedalEvent(pedalService, '1', true);
    delay(50);  // Give time for message to be sent
    // CRITICAL: Update PedalReader's lastState to LOW since we just sent a press event
    pedalReader->pedal1State.lastState = LOW;
    
    // Also check current pedal state - if still pressed, send again to be safe
    pinMode(PEDAL_1_PIN, INPUT_PULLUP);
    delay(5);
    if (digitalRead(PEDAL_1_PIN) == LOW) {
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        DebugService::print("Pedal 1 still pressed - sending pedal event again");
      }
      #endif
      pedalService_sendPedalEvent(pedalService, '1', true);
      delay(50);
    }
  }
  
  // Handle pedal 2 (right) if in dual mode and it was pressed
  if (detectedMode == PEDAL_MODE_DUAL && info->pedal2PressedOnWakeup) {
    pedalService_sendPedalEvent(pedalService, '2', true);
    delay(50);  // Give time for message to be sent
    // CRITICAL: Update PedalReader's lastState to LOW since we just sent a press event
    pedalReader->pedal2State.lastState = LOW;
    
    // Also check current pedal state - if still pressed, send again to be safe
    pinMode(PEDAL_2_PIN, INPUT_PULLUP);
    delay(5);
    if (digitalRead(PEDAL_2_PIN) == LOW) {
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        DebugService::print("Pedal 2 still pressed - sending pedal event again");
      }
      #endif
      pedalService_sendPedalEvent(pedalService, '2', true);
      delay(50);
    }
  }
}

void WakeupHandler::checkPedalReleaseAfterInterrupts(WakeupInfo* info, PedalReader* pedalReader,
                                                      PairingState* pairingState, PedalService* pedalService,
                                                      uint8_t detectedMode) {
  if (!info->wokeFromDeepSleep || !info->pedalPressedOnWakeup || !pairingState_isPaired(pairingState)) {
    return;
  }
  
  delay(10);  // Small delay to let interrupts stabilize
  
  // Check pedal 1 if it was pressed on wakeup
  if (info->pedal1PressedOnWakeup) {
    bool currentPedal1State = digitalRead(PEDAL_1_PIN);
    if (currentPedal1State == HIGH && pedalReader->pedal1State.lastState == LOW) {
      // Pedal 1 is now HIGH (released) but we sent a press event (lastState is LOW)
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        DebugService::print("Pedal 1 released during initialization - sending release event");
      }
      #endif
      pedalService_sendPedalEvent(pedalService, '1', false);
      pedalReader->pedal1State.lastState = HIGH;  // Update state to match current state
    }
  }
  
  // Check pedal 2 (right) if in dual mode and it was pressed on wakeup
  if (detectedMode == PEDAL_MODE_DUAL && info->pedal2PressedOnWakeup) {
    bool currentPedal2State = digitalRead(PEDAL_2_PIN);
    if (currentPedal2State == HIGH && pedalReader->pedal2State.lastState == LOW) {
      // Pedal 2 is now HIGH (released) but we sent a press event (lastState is LOW)
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        DebugService::print("Pedal 2 released during initialization - sending release event");
      }
      #endif
      pedalService_sendPedalEvent(pedalService, '2', false);
      pedalReader->pedal2State.lastState = HIGH;  // Update state to match current state
    }
  }
}
