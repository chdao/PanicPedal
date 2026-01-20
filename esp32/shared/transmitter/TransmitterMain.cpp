#include "TransmitterMain.h"
#include "../application/WakeupHandler.h"
#include "../application/MessageHandler.h"
#include "../infrastructure/DebugService.h"
#include "../infrastructure/PowerManagement.h"
#include "../domain/PedalModeDetector.h"
#include "../domain/MacUtils.h"
#include "../messages.h"
#include "../config.h"
#include "TransmitterConfig.h"
#include <Preferences.h>
#include "esp_sleep.h"

// ============================================================================
// CALLBACKS (shared across all transmitters)
// ============================================================================

void onPaired(const uint8_t* receiverMAC) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           receiverMAC[0], receiverMAC[1], receiverMAC[2],
           receiverMAC[3], receiverMAC[4], receiverMAC[5]);
  DebugService::print("Successfully paired with receiver: %s", macStr);
  
  // Save paired receiver MAC to NVS (persists across deep sleep)
  Preferences preferences;
  preferences.begin("pedal", false);  // Read-write mode
  preferences.putBytes("pairedMAC", receiverMAC, 6);
  preferences.end();
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    DebugService::print("Saved paired receiver MAC to NVS");
  }
  #endif
}

void onActivity() {
  lastActivityTime = millis();
}

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  MessageHandler::handleMessage(senderMAC, data, len, channel);
}

// ============================================================================
// INTERRUPT SERVICE ROUTINES (shared across all transmitters)
// ============================================================================

void IRAM_ATTR debugButtonISR() {
  debugButtonInterruptFlag = true;
}

void transmitterMain_setup() {
  // Initialize Serial (only if DEBUG_ENABLED)
  Serial.begin(115200);
  delay(500);
  bootTime = millis();
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    Serial.println("\n========================================");
    Serial.println("ESP-NOW Pedal Transmitter");
    Serial.println("========================================");
  }
  #endif
  
  // Detect wakeup cause
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  WakeupInfo wakeupInfo = WakeupHandler::detectWakeup();
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    WakeupHandler::logWakeupCause(wakeupCause);
  }
  #endif
  
  // Battery optimization
  setCpuFrequencyMhz(80);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  esp_bt_controller_disable();
  
  lastActivityTime = millis();
  
  // Determine pedal mode
  uint8_t detectedMode = PEDAL_MODE;
  #if HAS_AUTO_DETECT()
  if (PEDAL_MODE == PEDAL_MODE_AUTO) {
    detectedMode = PedalModeDetector::detect();
  }
  #endif
  
  // Initialize debug button
  pinMode(DEBUG_BUTTON_PIN, INPUT_PULLUP);
  debugButtonLastState = digitalRead(DEBUG_BUTTON_PIN);
  
  // Initialize domain layer FIRST (before attaching interrupts)
  pairingState_init(&pairingState);
  
  // Load persisted paired receiver MAC from NVS ONLY if waking from deep sleep
  if (wakeupInfo.wokeFromDeepSleep) {
    Preferences preferences;
    preferences.begin("pedal", true);
    uint8_t savedMAC[6];
    size_t macLen = preferences.getBytes("pairedMAC", savedMAC, 6);
    preferences.end();
    
    if (macLen == 6 && !macEqual(savedMAC, (uint8_t[6]){0,0,0,0,0,0})) {
      memcpy(pairingState.pairedReceiverMAC, savedMAC, 6);
      pairingState.isPaired = true;
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        DebugService::print("Restored paired receiver from NVS (deep sleep wakeup): %02X:%02X:%02X:%02X:%02X:%02X",
                           savedMAC[0], savedMAC[1], savedMAC[2], savedMAC[3], savedMAC[4], savedMAC[5]);
      }
      #endif
    }
  } else {
    Preferences preferences;
    preferences.begin("pedal", false);
    preferences.remove("pairedMAC");
    preferences.end();
    #ifdef DEBUG_ENABLED
    if (DEBUG_ENABLED) {
      DebugService::print("Full reset - cleared saved pairing");
    }
    #endif
  }
  
  pedalReader_init(&pedalReader, PEDAL_1_PIN, PEDAL_2_PIN, detectedMode);
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  DebugService::init(&transport);
  DebugService::setEnabled(DEBUG_ENABLED != 0);
  
  // Add broadcast peer
  uint8_t broadcastMAC[] = BROADCAST_MAC;
  espNowTransport_addPeer(&transport, broadcastMAC, 0);
  espNowTransport_registerReceiveCallback(&transport, onMessageReceived);
  
  // Initialize application layer
  pairingService_init(&pairingService, &pairingState, &transport, detectedMode, bootTime);
  pairingService.onPaired = onPaired;
  
  pedalService_init(&pedalService, &pedalReader, &pairingState, &transport, &lastActivityTime);
  pedalService.onActivity = onActivity;
  pedalService_setPairingService(&pairingService);
  
  // Initialize message handler
  MessageHandler::init(&pairingState, &pairingService, &transport, onActivity);
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    DebugService::print("ESP-NOW initialized");
    DebugService::print("Debug mode: %s", DebugService::isEnabled() ? "ENABLED" : "DISABLED");
    DebugService::print("ESP-NOW Pedal Transmitter Mode: %s", detectedMode == PEDAL_MODE_DUAL ? "DUAL" : "SINGLE");
  }
  #endif
  
  delay(100);
  
  // CRITICAL: If we restored pairing from NVS, add the peer now
  if (pairingState_isPaired(&pairingState)) {
    bool peerAdded = espNowTransport_addPeer(&transport, pairingState.pairedReceiverMAC, 0);
    #ifdef DEBUG_ENABLED
    if (DEBUG_ENABLED) {
      if (peerAdded) {
        DebugService::print("Added restored receiver peer to ESP-NOW");
      } else {
        DebugService::print("Failed to add restored receiver peer to ESP-NOW");
      }
    }
    #endif
    delay(20);
    
    if (wakeupInfo.wokeFromDeepSleep) {
      pairing_confirmed_message confirmMsg;
      confirmMsg.deviceType = DEVICE_TYPE_TRANSMITTER;
      confirmMsg.msgType = MSG_PAIRING_CONFIRMED;
      memcpy(confirmMsg.receiverMAC, pairingState.pairedReceiverMAC, 6);
      
      bool sent = espNowTransport_send(&transport, pairingState.pairedReceiverMAC, (uint8_t*)&confirmMsg, sizeof(confirmMsg));
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        if (sent) {
          DebugService::print("Sent MSG_PAIRING_CONFIRMED to saved receiver (waking from deep sleep)");
        } else {
          DebugService::print("Failed to send MSG_PAIRING_CONFIRMED to saved receiver");
        }
      }
      #endif
      
      if (sent) {
        pairingService.pairingConfirmedSentTime = millis();
        pairingService.waitingForPairingConfirmedAck = true;
      }
    }
  } else {
    pairingService_broadcastOnline(&pairingService);
  }
  
  delay(50);
  
  // CRITICAL: If woke from deep sleep and pedal is pressed, send pedal event immediately
  WakeupHandler::handleWakeupPedalEvents(&wakeupInfo, &pedalReader, &pairingState, 
                                          &pedalService, &transport, detectedMode);
  if (wakeupInfo.pedalPressedOnWakeup) {
    onActivity();
  }
  
  // Attach interrupts LAST
  delay(50);
  
  attachInterrupt(digitalPinToInterrupt(DEBUG_BUTTON_PIN), debugButtonISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PEDAL_1_PIN), pedal1ISR, CHANGE);
  pedalReader.interruptAttached1 = true;
  if (detectedMode == PEDAL_MODE_DUAL) {
    attachInterrupt(digitalPinToInterrupt(PEDAL_2_PIN), pedal2ISR, CHANGE);
    pedalReader.interruptAttached2 = true;
  }
  
  // CRITICAL: After attaching interrupts, check if pedal state changed during initialization
  WakeupHandler::checkPedalReleaseAfterInterrupts(&wakeupInfo, &pedalReader, 
                                                   &pairingState, &pedalService, detectedMode);
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    DebugService::print("Interrupts attached - ready for pedal input");
  }
  #endif
}

void transmitterMain_loop() {
  unsigned long currentTime = millis();
  
  // Process any pending discovery requests
  pairingService_processPendingDiscovery(&pairingService);
  
  // Process any pending debug messages
  DebugService::processPending();
  
  // Check discovery timeout
  if (pairingService_checkDiscoveryTimeout(&pairingService, currentTime)) {
    DebugService::print("Discovery response timeout");
  }
  
  // Check if MSG_PAIRING_CONFIRMED was sent and no ACK received within timeout
  if (pairingService.waitingForPairingConfirmedAck && pairingService.pairingConfirmedSentTime > 0) {
    unsigned long timeSinceSent = currentTime - pairingService.pairingConfirmedSentTime;
    if (timeSinceSent >= PAIRING_CONFIRMED_TIMEOUT_MS) {
      pairingService.waitingForPairingConfirmedAck = false;
      pairingService.pairingConfirmedSentTime = 0;
      
      // Receiver refused pairing (slots full) - delete receiver from NVS
      // This happens when receiver restarted and accepted new transmitters, or pedal was asleep during grace period
      Preferences preferences;
      preferences.begin("pedal", false);
      preferences.remove("pairedMAC");
      preferences.end();
      
      // Clear pairing state
      pairingState_init(&pairingState);
      
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        DebugService::print("MSG_PAIRING_CONFIRMED timeout - receiver refused pairing (slots full), deleted receiver from NVS");
        DebugService::print("Broadcasting MSG_ONLINE to discover available receivers");
      }
      #endif
      pairingService_broadcastOnline(&pairingService);
    }
  }
  
  // CRITICAL: Check if pedal is currently pressed and reset activity timer
  PowerManagement::resetActivityTimer(&lastActivityTime, &pedalReader);
  
  // Check inactivity timeout and enter deep sleep if inactive for 5 minutes
  if (PowerManagement::shouldEnterDeepSleep(lastActivityTime, currentTime, &pedalReader)) {
    DebugService::print("Inactivity timeout reached - entering deep sleep");
    EspNowTransport* debugTransport = DebugService::getDebugTransport();
    PowerManagement::goToDeepSleep(&pedalReader, &transport, &debugTransport);
  }
  
  // Update pedal service
  bool hasWork = pedalService_update(&pedalService);
  
  // Process debug button interrupt
  if (debugButtonInterruptFlag) {
    debugButtonInterruptFlag = false;
    
    if (currentTime - debugButtonLastDebounceTime >= DEBUG_BUTTON_DEBOUNCE_TIME_MS) {
      bool currentButtonState = digitalRead(DEBUG_BUTTON_PIN);
      debugButtonLastDebounceTime = currentTime;
      
      if (currentButtonState != debugButtonLastState) {
        debugButtonLastState = currentButtonState;
        
        if (currentButtonState == LOW && !debugButtonPressed) {
          debugButtonPressed = true;
          DebugService::setEnabled(!DebugService::isEnabled());
          DebugService::print("Debug mode toggled: %s", DebugService::isEnabled() ? "ENABLED" : "DISABLED");
        } else if (currentButtonState == HIGH && debugButtonPressed) {
          debugButtonPressed = false;
        }
      }
    }
  }
  
  // Power-optimized idle loop
  if (!hasWork) {
    bool isPaired = pairingState_isPaired(&pairingState);
    delay(isPaired ? IDLE_DELAY_PAIRED_MS : IDLE_DELAY_UNPAIRED_MS);
  } else {
    yield();
  }
}
