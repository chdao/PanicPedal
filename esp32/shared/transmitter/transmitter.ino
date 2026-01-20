#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include <Preferences.h>

// ============================================================================
// DEVICE CONFIGURATION
// ============================================================================
// Each device includes its own config.h that defines hardware-specific settings
// PanicPedal Pro: includes "config.h" from panicpedal-pro/
// FireBeetle: includes "config.h" from firebeetle2/
#include "config.h"
#include "TransmitterConfig.h"

// Clean Architecture: Include shared modules
#include "../messages.h"
#include "../domain/PairingState.h"
#include "../domain/PedalReader.h"
#include "../domain/MacUtils.h"
#include "../infrastructure/EspNowTransport.h"
#include "../application/PairingService.h"
#include "../application/PedalService.h"
#include "../infrastructure/DebugService.h"
#include "../infrastructure/PowerManagement.h"
#include "../application/MessageHandler.h"
#include "../application/WakeupHandler.h"
#include "../domain/PedalModeDetector.h"

// ============================================================================
// SYSTEM STATE
// ============================================================================

// Domain layer instances
PairingState pairingState;
PedalReader pedalReader;
EspNowTransport transport;

// Application layer instances
PairingService pairingService;
PedalService pedalService;

// System state
unsigned long lastActivityTime = 0;
unsigned long bootTime = 0;

// Debug button interrupt-based tracking (power optimized)
volatile bool debugButtonInterruptFlag = false;
unsigned long debugButtonLastDebounceTime = 0;
bool debugButtonLastState = HIGH;
bool debugButtonPressed = false;

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void onPaired(const uint8_t* receiverMAC);
void onActivity();
void IRAM_ATTR debugButtonISR();
void IRAM_ATTR pedal1ISR();
void IRAM_ATTR pedal2ISR();

// ============================================================================
// CALLBACKS
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
// INTERRUPT SERVICE ROUTINES
// ============================================================================

void IRAM_ATTR debugButtonISR() {
  // Just set flag - no other operations
  // Must return immediately - no delays, no Serial, no other operations
  debugButtonInterruptFlag = true;
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  // CRITICAL: For ESP32-S3, you MUST enable USB CDC On Boot in Arduino IDE:
  // Tools > USB CDC On Boot > Enabled
  // Tools > USB Mode > Hardware CDC and JTAG (or USB-OTG (TinyUSB))
  
  // Initialize Serial (only if DEBUG_ENABLED)
  Serial.begin(115200);
  delay(500);  // Allow Serial to initialize
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
  
  // Battery optimization: Reduce CPU frequency and enable WiFi power save
  setCpuFrequencyMhz(80);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  esp_bt_controller_disable();  // Disable Bluetooth to save power
  
  lastActivityTime = millis();
  
  // Determine pedal mode
  uint8_t detectedMode = PEDAL_MODE;
  #if HAS_AUTO_DETECT()
  if (PEDAL_MODE == PEDAL_MODE_AUTO) {
    detectedMode = PedalModeDetector::detect();
  }
  #endif
  
  // Initialize debug button - INPUT_PULLUP (interrupt attached later)
  pinMode(DEBUG_BUTTON_PIN, INPUT_PULLUP);
  debugButtonLastState = digitalRead(DEBUG_BUTTON_PIN);
  
  // Initialize domain layer FIRST (before attaching interrupts)
  pairingState_init(&pairingState);
  
  // Load persisted paired receiver MAC from NVS ONLY if waking from deep sleep
  // On full reset (UNDEFINED wakeup), forget the receiver
  if (wakeupInfo.wokeFromDeepSleep) {
    Preferences preferences;
    preferences.begin("pedal", true);  // Read-only mode
    uint8_t savedMAC[6];
    size_t macLen = preferences.getBytes("pairedMAC", savedMAC, 6);
    preferences.end();
    
    if (macLen == 6 && !macEqual(savedMAC, (uint8_t[6]){0,0,0,0,0,0})) {
      // Restore paired state from NVS (only after deep sleep wakeup)
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
    // Full reset - clear any saved pairing from NVS
    Preferences preferences;
    preferences.begin("pedal", false);  // Read-write mode
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
  
  // Small delay to ensure ESP-NOW is fully ready
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
    delay(20);  // Delay to ensure peer is ready
    
    // If waking from deep sleep, send MSG_PAIRING_CONFIRMED to saved receiver
    // This tells the receiver "I'm back online, do you still have a slot for me?"
    if (wakeupInfo.wokeFromDeepSleep) {
      pairing_confirmed_message confirmMsg;
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
      
      // Track when we sent MSG_PAIRING_CONFIRMED - if no ACK within 1s, send MSG_ONLINE
      if (sent) {
        pairingService.pairingConfirmedSentTime = millis();
        pairingService.waitingForPairingConfirmedAck = true;
      }
    }
  } else {
    // Not paired - no MAC saved, broadcast MSG_ONLINE for discovery
    pairingService_broadcastOnline(&pairingService);
  }
  
  // Additional delay to ensure messages complete
  delay(50);
  
  // CRITICAL: If woke from deep sleep and pedal is pressed, send pedal event immediately
  // This must happen AFTER ESP-NOW is fully initialized and ready
  WakeupHandler::handleWakeupPedalEvents(&wakeupInfo, &pedalReader, &pairingState, 
                                          &pedalService, &transport, detectedMode);
  if (wakeupInfo.pedalPressedOnWakeup) {
    onActivity();  // Reset activity timer
  }
  
  // Attach interrupts LAST - after everything is initialized
  // This prevents interrupts from firing during initialization and causing watchdog timeouts
  delay(50);  // Small delay to ensure everything is stable before enabling interrupts
  
  attachInterrupt(digitalPinToInterrupt(DEBUG_BUTTON_PIN), debugButtonISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PEDAL_1_PIN), pedal1ISR, CHANGE);
  pedalReader.interruptAttached1 = true;  // Mark interrupt as attached
  if (detectedMode == PEDAL_MODE_DUAL) {
    attachInterrupt(digitalPinToInterrupt(PEDAL_2_PIN), pedal2ISR, CHANGE);
    pedalReader.interruptAttached2 = true;  // Mark interrupt as attached
  }
  
  // CRITICAL: After attaching interrupts, check if pedal state changed during initialization
  // If we sent a press event on wake but the pedal is now released, send a release event
  WakeupHandler::checkPedalReleaseAfterInterrupts(&wakeupInfo, &pedalReader, 
                                                   &pairingState, &pedalService, detectedMode);
  
  #ifdef DEBUG_ENABLED
  if (DEBUG_ENABLED) {
    DebugService::print("Interrupts attached - ready for pedal input");
  }
  #endif
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
  unsigned long currentTime = millis();
  
  // Process any pending discovery requests (deferred from ESP-NOW callback)
  // This must be done in main loop, not in callback context
  pairingService_processPendingDiscovery(&pairingService);
  
  // Process any pending debug messages (deferred from ESP-NOW callback after sending)
  DebugService::processPending();
  
  // Check discovery timeout
  if (pairingService_checkDiscoveryTimeout(&pairingService, currentTime)) {
    DebugService::print("Discovery response timeout");
  }
  
  // Check if MSG_PAIRING_CONFIRMED was sent and no ACK received within timeout
  // If timeout occurred, send MSG_ONLINE for discovery
  if (pairingService.waitingForPairingConfirmedAck && pairingService.pairingConfirmedSentTime > 0) {
    unsigned long timeSinceSent = currentTime - pairingService.pairingConfirmedSentTime;
    if (timeSinceSent >= PAIRING_CONFIRMED_TIMEOUT_MS) {
      // Timeout - no ACK received, send MSG_ONLINE for discovery
      pairingService.waitingForPairingConfirmedAck = false;
      pairingService.pairingConfirmedSentTime = 0;
      
      #ifdef DEBUG_ENABLED
      if (DEBUG_ENABLED) {
        DebugService::print("MSG_PAIRING_CONFIRMED timeout - no ACK received, sending MSG_ONLINE");
      }
      #endif
      pairingService_broadcastOnline(&pairingService);
    }
  }
  
  // CRITICAL: Check if pedal is currently pressed and reset activity timer
  // This prevents the timer from counting up while pedal is held down
  PowerManagement::resetActivityTimer(&lastActivityTime, &pedalReader);
  
  // Check inactivity timeout and enter deep sleep if inactive for 5 minutes
  if (PowerManagement::shouldEnterDeepSleep(lastActivityTime, currentTime, &pedalReader)) {
    DebugService::print("Inactivity timeout reached - entering deep sleep");
    EspNowTransport* debugTransport = DebugService::getDebugTransport();
    PowerManagement::goToDeepSleep(&pedalReader, &transport, &debugTransport);
  }
  
  // Update pedal service (only processes when interrupts occur)
  bool hasWork = pedalService_update(&pedalService);
  
  // Process debug button interrupt (interrupt-driven, power optimized)
  if (debugButtonInterruptFlag) {
    debugButtonInterruptFlag = false;  // Clear flag immediately
    
    // Debounce: ignore if too soon since last interrupt
    if (currentTime - debugButtonLastDebounceTime >= DEBUG_BUTTON_DEBOUNCE_TIME_MS) {
      // Read GPIO state in main loop (not ISR) to avoid watchdog timeout
      bool currentButtonState = digitalRead(DEBUG_BUTTON_PIN);
      debugButtonLastDebounceTime = currentTime;
      
      // Only process if state actually changed
      if (currentButtonState != debugButtonLastState) {
        debugButtonLastState = currentButtonState;
        
        if (currentButtonState == LOW && !debugButtonPressed) {
          // Button pressed (LOW with pull-up)
          debugButtonPressed = true;
          DebugService::setEnabled(!DebugService::isEnabled());
          DebugService::print("Debug mode toggled: %s", DebugService::isEnabled() ? "ENABLED" : "DISABLED");
        } else if (currentButtonState == HIGH && debugButtonPressed) {
          // Button released
          debugButtonPressed = false;
        }
      }
    }
  }
  
  // Power-optimized idle loop: use longer delays when not paired (saves power)
  if (!hasWork) {
    bool isPaired = pairingState_isPaired(&pairingState);
    delay(isPaired ? IDLE_DELAY_PAIRED_MS : IDLE_DELAY_UNPAIRED_MS);
  } else {
    yield();  // Work to do - process immediately
  }
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "../domain/PairingState.cpp"
#include "../domain/PedalReader.cpp"
#include "../debug_format.cpp"
#include "../infrastructure/EspNowTransport.cpp"
#include "../infrastructure/TransmitterUtils.cpp"
#include "../application/PairingService.cpp"
#include "../application/PedalService.cpp"
#include "../infrastructure/DebugService.cpp"
#include "../infrastructure/PowerManagement.cpp"
#include "../application/MessageHandler.cpp"
#include "../application/WakeupHandler.cpp"
#include "../domain/PedalModeDetector.cpp"
