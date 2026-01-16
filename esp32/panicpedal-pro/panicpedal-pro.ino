#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_task_wdt.h"
#include <Preferences.h>

// Clean Architecture: Include shared and domain modules
#include "shared/messages.h"
#include "shared/config.h"
#include "shared/debug_format.h"
#include "shared/domain/PairingState.h"
#include "shared/domain/PedalReader.h"
#include "shared/domain/MacUtils.h"
#include "shared/infrastructure/EspNowTransport.h"
#include "shared/application/PairingService.h"
#include "shared/application/PedalService.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define PEDAL_MODE_AUTO -1     // Auto-detect based on connected switches (recommended)
#define PEDAL_MODE_DUAL 0      // Force dual pedal mode (GPIO1 & GPIO2)
#define PEDAL_MODE_SINGLE 1    // Force single pedal mode (GPIO1 only)
#define PEDAL_MODE PEDAL_MODE_AUTO  // Change to PEDAL_MODE_DUAL or PEDAL_MODE_SINGLE to override auto-detection
#define DEBUG_ENABLED 1  // Set to 0 to disable Serial output and save battery
// ============================================================================

// Debug system (for debug monitor)
bool debugEnabled = false;  // Runtime debug flag (can be toggled)
EspNowTransport* g_debugTransport = nullptr;  // Set in setup()

// Forward declaration
extern unsigned long bootTime;

// Use shared utilities for debug/serial output
#include "shared/infrastructure/TransmitterUtils.h"

// Cache MAC address to avoid repeated WiFi.macAddress() calls (power optimization)
static uint8_t g_cachedMAC[6] = {0};
static bool g_macCached = false;

static void cacheMAC() {
  if (!g_macCached) {
    WiFi.macAddress(g_cachedMAC);
    g_macCached = true;
  }
}

// Unified debug function: always sends same message to Serial and debug monitor
// This ensures consistency - both outputs receive identical messages
// Note: debugEnabled flag can be checked before calling this function for conditional logic
void debugPrint(const char* format, ...) {
  // Runtime gate: when debug is disabled, do not emit logs to Serial or debug monitor.
  // This keeps normal operation quiet and reduces the chance of WDT issues from heavy I/O.
  if (!debugEnabled) {
    return;
  }

  // Use cached MAC address
  cacheMAC();
  
  // Format message with standardized format
  char buffer[250];
  va_list args;
  va_start(args, format);
  debugFormat_message_va(buffer, sizeof(buffer), g_cachedMAC, false, bootTime, format, args);
  va_end(args);
  
  // Send to Serial (if DEBUG_ENABLED) - non-blocking write in chunks
  if (DEBUG_ENABLED) {
    size_t len = strlen(buffer);
    if (len > 0) {
      size_t written = 0;
      while (written < len) {
        size_t chunkSize = min(len - written, (size_t)64);
        Serial.write((const uint8_t*)(buffer + written), chunkSize);
        written += chunkSize;
        yield();
      }
      
      if (buffer[len-1] != '\n') {
        Serial.println();
        yield();
      }
    }
  }
  
  // Send to debug monitor via ESP-NOW (if transport available)
  // Same message goes to both Serial and debug monitor for consistency
  if (g_debugTransport && g_debugTransport->initialized) {
    debug_message debugMsg;
    debugMsg.msgType = MSG_DEBUG;
    
    int len = strlen(buffer);
    if (len > 0 && buffer[len-1] == '\n') {
      buffer[len-1] = '\0';
      len--;
    }
    
    size_t maxMsgLen = sizeof(debugMsg.message) - 1;
    if (len > (int)maxMsgLen) {
      len = maxMsgLen;
    }
    
    memcpy(debugMsg.message, buffer, len);
    debugMsg.message[len] = '\0';
    
    // Small delay to ensure ESP-NOW stack is ready (especially after sending other messages)
    yield();
    delay(10);  // Additional delay to ensure ESP-NOW stack is ready for broadcast
    
    uint8_t broadcastMAC[] = BROADCAST_MAC;
    espNowTransport_broadcast(g_debugTransport, (uint8_t*)&debugMsg, sizeof(debugMsg));
    
    // Small delay after broadcast to ensure it completes
    yield();
    delay(5);
  }
}

// Legacy wrapper functions for compatibility (now use unified debugPrint)
// These ensure backward compatibility while using the unified debug function
void serialPrint(const char* format, ...) {
  // Always print to Serial (boot/config messages).
  // For debug monitor output, we only forward when debugEnabled is true via debugPrint().
  if (!DEBUG_ENABLED) {
    return;
  }

  va_list args;
  va_start(args, format);
  char buffer[250];
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  Serial.println(buffer);

  // Forward same message to debug monitor if enabled (keeps outputs consistent when debugging).
  if (debugEnabled) {
    debugPrint("%s", buffer);
  }
}

void sendDebugMessage(const char* formattedMessage) {
  debugPrint("%s", formattedMessage);
}
// ============================================================================

// GPIO Pin Definitions (PanicPedal Pro - ESP32-S3-WROOM)
#define PEDAL_RIGHT_NO_PIN 1   // Right pedal switch NO (normally open)
#define PEDAL_LEFT_NO_PIN 2    // Left pedal switch NO (normally open)
#define BATTERY_VOLTAGE_PIN 9  // Battery voltage sensing (before TLV75733PDBV)
#define BATTERY_STAT1_PIN 4    // STAT1/LBO from MCP73871 (charging status)
#define TOGGLE_SWITCH_PIN 5    // Toggle switch
#define PEDAL_LEFT_NC_PIN 6    // Left pedal switch NC (normally closed) - for detection
#define PEDAL_RIGHT_NC_PIN 7   // Right pedal switch NC (normally closed) - for detection
#define LED_WS2812B_PIN 8      // WS2812B LED control
#define DEBUG_BUTTON_PIN 10    // Debug pushbutton toggle

// Debug button interrupt-based tracking (power optimized)
volatile bool debugButtonInterruptFlag = false;
unsigned long debugButtonLastDebounceTime = 0;
bool debugButtonLastState = HIGH;
bool debugButtonPressed = false;
#define DEBUG_BUTTON_DEBOUNCE_TIME_MS 50

// ISR for debug button (GPIO10) - just set flag, process in main loop
// CRITICAL: This ISR must be extremely fast - any delay can cause watchdog timeout
void IRAM_ATTR debugButtonISR() {
  // Just set flag - no other operations
  // Must return immediately - no delays, no Serial, no other operations
  debugButtonInterruptFlag = true;
}

#define INACTIVITY_TIMEOUT 300000  // 5 minutes of inactivity before deep sleep
#define IDLE_DELAY_PAIRED 10       // 10ms delay when paired and idle
#define IDLE_DELAY_UNPAIRED 200    // 200ms delay when not paired (longer to save power)

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

// Forward declarations
void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel);
void onPaired(const uint8_t* receiverMAC);
void onActivity();
uint8_t detectPedalMode();

// Deferred debug message (to avoid calling debugPrint from ESP-NOW callback after sending)
static bool hasPendingDebugMessage = false;
static char pendingDebugMessage[200];

void onPaired(const uint8_t* receiverMAC) {
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           receiverMAC[0], receiverMAC[1], receiverMAC[2],
           receiverMAC[3], receiverMAC[4], receiverMAC[5]);
  debugPrint("Successfully paired with receiver: %s", macStr);
  
  // Save paired receiver MAC to NVS (persists across deep sleep)
  Preferences preferences;
  preferences.begin("pedal", false);  // Read-write mode
  preferences.putBytes("pairedMAC", receiverMAC, 6);
  preferences.end();
  
  if (DEBUG_ENABLED) {
    debugPrint("Saved paired receiver MAC to NVS");
  }
}

void onActivity() {
  lastActivityTime = millis();
}

void sendDeleteRecordMessage(const uint8_t* receiverMAC) {
  struct_message deleteMsg = {MSG_DELETE_RECORD, 0, false, 0};
  espNowTransport_send(&transport, receiverMAC, (uint8_t*)&deleteMsg, sizeof(deleteMsg));
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           receiverMAC[0], receiverMAC[1], receiverMAC[2],
           receiverMAC[3], receiverMAC[4], receiverMAC[5]);
  debugPrint("Sent delete record message to receiver: %s", macStr);
}

void onMessageReceived(const uint8_t* senderMAC, const uint8_t* data, int len, uint8_t channel) {
  if (len < 1) return;
  
  // Reset activity timer when receiving messages (keeps device awake during active communication)
  onActivity();
  
  char senderMacStr[18];
  snprintf(senderMacStr, sizeof(senderMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           senderMAC[0], senderMAC[1], senderMAC[2],
           senderMAC[3], senderMAC[4], senderMAC[5]);
  debugPrint("Received ESP-NOW message: len=%d, sender=%s", len, senderMacStr);
  
  uint8_t msgType = data[0];
  
  // Handle beacon message
  if (msgType == MSG_BEACON && len >= sizeof(beacon_message)) {
    beacon_message* beacon = (beacon_message*)data;
    pairingService_handleBeacon(&pairingService, senderMAC, beacon);
    
    debugPrint("Received MSG_BEACON: slots=%d/%d", beacon->availableSlots, beacon->totalSlots);
    return;
  }
  
  // Handle pairing confirmed message from receiver (receiver-initiated pairing confirmation)
  if (msgType == MSG_PAIRING_CONFIRMED && len >= sizeof(pairing_confirmed_message)) {
    pairing_confirmed_message* confirm = (pairing_confirmed_message*)data;
    
    // Check if we're already paired to a different receiver
    if (pairingState_isPaired(&pairingState)) {
      bool isPairedReceiver = macEqual(senderMAC, pairingState.pairedReceiverMAC);
      
      if (!isPairedReceiver) {
        // Already paired to a different receiver - tell this receiver to delete our record
        char senderMacStr[18];
        char pairedMacStr[18];
        snprintf(senderMacStr, sizeof(senderMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 senderMAC[0], senderMAC[1], senderMAC[2],
                 senderMAC[3], senderMAC[4], senderMAC[5]);
        snprintf(pairedMacStr, sizeof(pairedMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 pairingState.pairedReceiverMAC[0], pairingState.pairedReceiverMAC[1], pairingState.pairedReceiverMAC[2],
                 pairingState.pairedReceiverMAC[3], pairingState.pairedReceiverMAC[4], pairingState.pairedReceiverMAC[5]);
        debugPrint("Received MSG_PAIRING_CONFIRMED from different receiver (%s) - we're paired to %s - sending DELETE_RECORD",
                   senderMacStr, pairedMacStr);
        
        espNowTransport_addPeer(&transport, senderMAC, channel);
        delay(10);
        sendDeleteRecordMessage(senderMAC);
        return;  // Don't send ACK - we're rejecting this pairing
      }
      
      // Same receiver - just ensure peer is added
      espNowTransport_addPeer(&transport, senderMAC, channel);
      delay(10);
      debugPrint("Received MSG_PAIRING_CONFIRMED from paired receiver - peer confirmed");
    } else {
      // Not paired yet - restore pairing state with this receiver
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               senderMAC[0], senderMAC[1], senderMAC[2],
               senderMAC[3], senderMAC[4], senderMAC[5]);
      debugPrint("Received MSG_PAIRING_CONFIRMED - restoring pairing state with receiver %s", macStr);
      memcpy(pairingState.pairedReceiverMAC, senderMAC, 6);
      pairingState.isPaired = true;
      
      // Ensure peer is added
      espNowTransport_addPeer(&transport, senderMAC, channel);
      delay(10);
      
      // Save to NVS
      Preferences preferences;
      preferences.begin("pedal", false);
      preferences.putBytes("pairedMAC", senderMAC, 6);
      preferences.end();
      
      debugPrint("Pairing restored - ready to send pedal events");
    }
    
    // Reply with MSG_PAIRING_CONFIRMED_ACK to acknowledge we received and accepted the pairing confirmation
    // This lets the receiver know we're online and responsive
    pairing_confirmed_ack_message ackMsg;
    ackMsg.msgType = MSG_PAIRING_CONFIRMED_ACK;
    memcpy(ackMsg.receiverMAC, senderMAC, 6);  // Echo receiver's MAC to confirm
    
    espNowTransport_addPeer(&transport, senderMAC, channel);
    delay(10);  // Small delay to ensure peer is ready
    bool sent = espNowTransport_send(&transport, senderMAC, (uint8_t*)&ackMsg, sizeof(ackMsg));
    
    // Defer debug message to main loop (can't reliably send ESP-NOW broadcast from callback after sending)
    if (sent) {
      snprintf(pendingDebugMessage, sizeof(pendingDebugMessage), "Sent MSG_PAIRING_CONFIRMED_ACK to acknowledge receiver's pairing confirmation");
    } else {
      snprintf(pendingDebugMessage, sizeof(pendingDebugMessage), "Failed to send MSG_PAIRING_CONFIRMED_ACK acknowledgment");
    }
    hasPendingDebugMessage = true;
    
    return;
  }
  
  // Handle pairing confirmed acknowledgment from receiver (receiver acknowledging our MSG_PAIRING_CONFIRMED request)
  if (msgType == MSG_PAIRING_CONFIRMED_ACK && len >= sizeof(pairing_confirmed_ack_message)) {
    pairing_confirmed_ack_message* ack = (pairing_confirmed_ack_message*)data;
    
    // Check if this is from our paired receiver
    if (pairingState_isPaired(&pairingState)) {
      bool isPairedReceiver = macEqual(senderMAC, pairingState.pairedReceiverMAC);
      
      if (!isPairedReceiver) {
        // Different receiver - ignore (shouldn't happen, but handle gracefully)
        debugPrint("Received MSG_PAIRING_CONFIRMED_ACK from different receiver - ignoring");
        return;
      }
    }
    
    // Clear waiting flag - we received the ACK
    pairingService.waitingForPairingConfirmedAck = false;
    pairingService.pairingConfirmedSentTime = 0;
    
    // Receiver acknowledged our reconnection request - restore pairing state if not already paired
    if (!pairingState_isPaired(&pairingState)) {
      char macStr[18];
      snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
               senderMAC[0], senderMAC[1], senderMAC[2],
               senderMAC[3], senderMAC[4], senderMAC[5]);
      debugPrint("Received MSG_PAIRING_CONFIRMED_ACK - restoring pairing state with receiver %s", macStr);
      memcpy(pairingState.pairedReceiverMAC, senderMAC, 6);
      pairingState.isPaired = true;
      
      // Ensure peer is added
      espNowTransport_addPeer(&transport, senderMAC, channel);
      delay(10);
      
      // Save to NVS
      Preferences preferences;
      preferences.begin("pedal", false);
      preferences.putBytes("pairedMAC", senderMAC, 6);
      preferences.end();
      
      debugPrint("Pairing restored - ready to send pedal events");
    } else {
      // Already paired - just ensure peer is added
      espNowTransport_addPeer(&transport, senderMAC, channel);
      delay(10);
      debugPrint("Received MSG_PAIRING_CONFIRMED_ACK from paired receiver - reconnection confirmed");
    }
    
    // No need to send another message - receiver already acknowledged
    return;
  }
  
  // Handle other messages
  if (len < sizeof(struct_message)) {
    debugPrint("Message too short");
    return;
  }
  
  struct_message* msg = (struct_message*)data;
  
  debugPrint("Message type=%d, isPaired=%s", msg->msgType, 
             pairingState_isPaired(&pairingState) ? "true" : "false");
  
  if (pairingState_isPaired(&pairingState)) {
    // Already paired - check if message is from our paired receiver
    if (memcmp(senderMAC, pairingState.pairedReceiverMAC, 6) == 0) {
      // Message from our paired receiver
      if (msg->msgType == MSG_ALIVE) {
        debugPrint("Received MSG_ALIVE from paired receiver - calling handleAlive");
        pairingService_handleAlive(&pairingService, senderMAC, channel);
      } else if (msg->msgType == MSG_PAIRING_CONFIRMED_ACK && len >= sizeof(pairing_confirmed_ack_message)) {
        // Receiver acknowledged our reconnection request - already handled in dedicated handler above
        // Just reset activity timer
        onActivity();
      } else {
        debugPrint("Received message from paired receiver (type=%d)", msg->msgType);
      }
    } else {
      // Message from different receiver - send DELETE_RECORD
      if (msg->msgType == MSG_ALIVE || msg->msgType == MSG_DISCOVERY_RESP) {
        debugPrint("Received message from different receiver - sending DELETE_RECORD");
        
        espNowTransport_addPeer(&transport, senderMAC, channel);
        sendDeleteRecordMessage(senderMAC);
      }
    }
  } else {
    // Not paired - handle pairing messages
    if (msg->msgType == MSG_DISCOVERY_RESP) {
      debugPrint("Handling MSG_DISCOVERY_RESP from receiver");
      pairingService_handleDiscoveryResponse(&pairingService, senderMAC, channel);
    } else if (msg->msgType == MSG_ALIVE) {
      debugPrint("Received MSG_ALIVE when not paired - calling handleAlive");
      pairingService_handleAlive(&pairingService, senderMAC, channel);
    } else {
      debugPrint("Unhandled message type=%d when not paired", msg->msgType);
  }
}
}



uint8_t detectPedalMode() {
  // Configure NC pins as inputs with pull-ups to detect switch connections
  pinMode(PEDAL_LEFT_NC_PIN, INPUT_PULLUP);
  pinMode(PEDAL_RIGHT_NC_PIN, INPUT_PULLUP);
  
  // Small delay to allow pins to stabilize
  delay(10);
  
  // Read NC pins multiple times to ensure stable reading (debouncing)
  int leftReads = 0, rightReads = 0;
  for (int i = 0; i < 5; i++) {
    if (digitalRead(PEDAL_LEFT_NC_PIN) == LOW) leftReads++;
    if (digitalRead(PEDAL_RIGHT_NC_PIN) == LOW) rightReads++;
    delay(2);
  }
  
  // Consider connected if pin reads LOW in majority of reads
  bool pedal1Connected = (leftReads >= 3);   // GPIO6 - Left NC
  bool pedal2Connected = (rightReads >= 3);  // GPIO7 - Right NC
  
  // Read raw pin states for logging
  int leftRaw = digitalRead(PEDAL_LEFT_NC_PIN);
  int rightRaw = digitalRead(PEDAL_RIGHT_NC_PIN);
  
  serialPrint("Pedal detection: GPIO6(Left)=%s (%d/5 LOW), GPIO7(Right)=%s (%d/5 LOW)",
               pedal1Connected ? "CONNECTED" : "NOT CONNECTED", leftReads,
               pedal2Connected ? "CONNECTED" : "NOT CONNECTED", rightReads);
  
  // Determine mode based on detected switches
  uint8_t detectedMode;
  if (pedal1Connected && pedal2Connected) {
    detectedMode = PEDAL_MODE_DUAL;
    serialPrint("Detected mode: DUAL (both pedals)");
  } else if (pedal1Connected) {
    detectedMode = PEDAL_MODE_SINGLE;
    serialPrint("Detected mode: SINGLE (left pedal only)");
  } else if (pedal2Connected) {
    detectedMode = PEDAL_MODE_SINGLE;
    serialPrint("Detected mode: SINGLE (right detected, using GPIO2)");
  } else {
    detectedMode = PEDAL_MODE_SINGLE;
    serialPrint("WARNING: No pedals detected via NC pins, defaulting to SINGLE mode");
  }
  
  return detectedMode;
}

void goToDeepSleep() {
  // CRITICAL: Disable debug transport FIRST to prevent any ESP-NOW operations
  g_debugTransport = nullptr;
  transport.initialized = false;
  
  // CRITICAL: Detach interrupts to prevent them from firing during deep sleep prep
  detachInterrupt(digitalPinToInterrupt(PEDAL_LEFT_NO_PIN));
  if (pedalReader.pedalMode == 0) {  // 0 = DUAL mode
    detachInterrupt(digitalPinToInterrupt(PEDAL_RIGHT_NO_PIN));
  }
  detachInterrupt(digitalPinToInterrupt(DEBUG_BUTTON_PIN));
  
  // Disable watchdog timer to prevent timeouts during deep sleep prep
  esp_task_wdt_deinit();
  
  // Deinitialize ESP-NOW before deep sleep
  esp_now_deinit();
  
  // Turn off WiFi
  WiFi.mode(WIFI_OFF);
  
  // Configure NC pins (GPIO6/7) as RTC GPIOs with pull-up
  rtc_gpio_init((gpio_num_t)PEDAL_LEFT_NC_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_LEFT_NC_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_LEFT_NC_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_LEFT_NC_PIN);
  
  rtc_gpio_init((gpio_num_t)PEDAL_RIGHT_NC_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_RIGHT_NC_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_RIGHT_NC_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_RIGHT_NC_PIN);
  
  // Configure GPIO2 and GPIO1 as RTC GPIOs for ext1 wakeup (wake on either pedal)
  // IMPORTANT: Always configure with pull-up so pins read HIGH when no pedal is attached
  rtc_gpio_init((gpio_num_t)PEDAL_LEFT_NO_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_LEFT_NO_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_LEFT_NO_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_LEFT_NO_PIN);

  rtc_gpio_init((gpio_num_t)PEDAL_RIGHT_NO_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_RIGHT_NO_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_RIGHT_NO_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_RIGHT_NO_PIN);
  
  // Small delay to allow RTC GPIO pull-up to stabilize
  delay(20);  // Longer delay to ensure pull-up is active
  
  // Check GPIO1/GPIO2 multiple times - if either is LOW, don't sleep
  // Both pins must be HIGH when we configure wakeup, otherwise it will wake immediately.
  // With pull-ups enabled, they should read HIGH when no pedal is attached.
  int leftHighReads = 0;
  int rightHighReads = 0;
  for (int i = 0; i < 10; i++) {  // More reads to ensure stability
    if (digitalRead(PEDAL_LEFT_NO_PIN) == HIGH) leftHighReads++;
    if (digitalRead(PEDAL_RIGHT_NO_PIN) == HIGH) rightHighReads++;
    delay(5);
  }
  
  // If either pin is LOW (pedal pressed), don't sleep.
  // If it's just unstable, do a final check after a longer delay.
  if (leftHighReads < 8 || rightHighReads < 8) {
    delay(20);
    if (digitalRead(PEDAL_LEFT_NO_PIN) == LOW || digitalRead(PEDAL_RIGHT_NO_PIN) == LOW) {
      return;  // Skip sleep if a pedal is definitely pressed
    }
    // If both are HIGH now, it was just unstable - proceed with sleep
  }
  
  // Disable all wakeup sources, then enable GPIO1 + GPIO2
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  
  // Configure ext1 wakeup: GPIO1 OR GPIO2, LOW trigger (either pedal press)
  // GPIO2 is bit 2, GPIO1 is bit 1
  uint64_t gpioMask = (1ULL << PEDAL_LEFT_NO_PIN) | (1ULL << PEDAL_RIGHT_NO_PIN);
  esp_err_t wakeupResult = esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ANY_LOW);
  
  // If wakeup configuration failed, don't sleep
  if (wakeupResult != ESP_OK) {
    return;
  }
  
  // CRITICAL: Check GPIO1/GPIO2 one more time AFTER configuring wakeup
  // If either goes LOW now, it will wake immediately
  delay(10);
  if (digitalRead(PEDAL_LEFT_NO_PIN) == LOW || digitalRead(PEDAL_RIGHT_NO_PIN) == LOW) {
    // A pedal is pressed - will wake immediately, don't sleep
    return;
  }
  
  // NO Serial operations - they can cause crashes during deep sleep entry
  // Enter deep sleep immediately
  esp_deep_sleep_start();
  // Code never reaches here - device enters deep sleep
}

void setup() {
  // CRITICAL: For ESP32-S3, you MUST enable USB CDC On Boot in Arduino IDE:
  // Tools > USB CDC On Boot > Enabled
  // Tools > USB Mode > Hardware CDC and JTAG (or USB-OTG (TinyUSB))
  
  // Check wakeup cause if waking from deep sleep
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  bool wokeFromDeepSleep = (wakeupCause == ESP_SLEEP_WAKEUP_EXT1 || wakeupCause == ESP_SLEEP_WAKEUP_EXT0);
  bool pedalPressedOnWakeup = false;
  
  // Initialize Serial (only if DEBUG_ENABLED)
  Serial.begin(115200);
  delay(500);  // Allow Serial to initialize
  bootTime = millis();
  
  if (DEBUG_ENABLED) {
    Serial.println("\n========================================");
    Serial.println("ESP-NOW Pedal Transmitter - PanicPedal Pro");
    Serial.println("========================================");
    
    // Log wakeup cause
    switch (wakeupCause) {
      case ESP_SLEEP_WAKEUP_EXT0:
        // EXT0 wakeup (single GPIO) - shouldn't happen with ext1, but handle it
        Serial.println("Wakeup cause: EXT0 (unexpected - using ext1)");
        break;
      case ESP_SLEEP_WAKEUP_EXT1: {
        // EXT1 wakeup - check which GPIO triggered it
        uint64_t gpioMask = esp_sleep_get_ext1_wakeup_status();
        pinMode(PEDAL_LEFT_NO_PIN, INPUT_PULLUP);
        pinMode(PEDAL_RIGHT_NO_PIN, INPUT_PULLUP);
        delay(10);  // Allow pin to stabilize
        
        bool gpio2State = digitalRead(PEDAL_LEFT_NO_PIN);
        bool gpio1State = digitalRead(PEDAL_RIGHT_NO_PIN);
        pedalPressedOnWakeup = (gpio2State == LOW) || (gpio1State == LOW);
        pedal1PressedOnWakeup = (gpio2State == LOW);  // Left pedal (GPIO2)
        pedal2PressedOnWakeup = (gpio1State == LOW);  // Right pedal (GPIO1)
        
        Serial.printf("Wakeup cause: EXT1 - GPIO mask: 0x%llx, GPIO2=%s, GPIO1=%s\n",
                     gpioMask,
                     gpio2State == HIGH ? "HIGH" : "LOW",
                     gpio1State == HIGH ? "HIGH" : "LOW");
        
        // Check which GPIO triggered the wakeup
        if (gpioMask & (1ULL << PEDAL_LEFT_NO_PIN)) {
          Serial.println("  -> GPIO2 triggered wakeup");
        }
        if (gpioMask & (1ULL << PEDAL_RIGHT_NO_PIN)) {
          Serial.println("  -> GPIO1 triggered wakeup");
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
  }

  // Battery optimization: Reduce CPU frequency and enable WiFi power save
  setCpuFrequencyMhz(80);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
  esp_bt_controller_disable();  // Disable Bluetooth to save power
  
  lastActivityTime = millis();
  
  // Determine pedal mode
  uint8_t detectedMode = PEDAL_MODE;
  if (PEDAL_MODE == PEDAL_MODE_AUTO) {
    detectedMode = detectPedalMode();
  }
  
  // Initialize debug button (GPIO10) - INPUT_PULLUP (interrupt attached later)
  pinMode(DEBUG_BUTTON_PIN, INPUT_PULLUP);
  debugButtonLastState = digitalRead(DEBUG_BUTTON_PIN);
  
  // Initialize domain layer FIRST (before attaching interrupts)
  pairingState_init(&pairingState);
  
  // Load persisted paired receiver MAC from NVS ONLY if waking from deep sleep
  // On full reset (UNDEFINED wakeup), forget the receiver
  if (wokeFromDeepSleep) {
    Preferences preferences;
    preferences.begin("pedal", true);  // Read-only mode
    uint8_t savedMAC[6];
    size_t macLen = preferences.getBytes("pairedMAC", savedMAC, 6);
    preferences.end();
    
    if (macLen == 6 && !macEqual(savedMAC, (uint8_t[6]){0,0,0,0,0,0})) {
      // Restore paired state from NVS (only after deep sleep wakeup)
      memcpy(pairingState.pairedReceiverMAC, savedMAC, 6);
      pairingState.isPaired = true;
      if (DEBUG_ENABLED) {
        debugPrint("Restored paired receiver from NVS (deep sleep wakeup): %02X:%02X:%02X:%02X:%02X:%02X",
                   savedMAC[0], savedMAC[1], savedMAC[2], savedMAC[3], savedMAC[4], savedMAC[5]);
      }
    }
  } else {
    // Full reset - clear any saved pairing from NVS
    Preferences preferences;
    preferences.begin("pedal", false);  // Read-write mode
    preferences.remove("pairedMAC");
    preferences.end();
    if (DEBUG_ENABLED) {
      debugPrint("Full reset - cleared saved pairing");
    }
  }
  
  pedalReader_init(&pedalReader, PEDAL_LEFT_NO_PIN, PEDAL_RIGHT_NO_PIN, detectedMode);
  
  // Initialize infrastructure layer
  espNowTransport_init(&transport);
  g_debugTransport = &transport;
  debugEnabled = (DEBUG_ENABLED != 0);
  
  // Cache MAC address early (power optimization)
  cacheMAC();
  
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
  
  if (DEBUG_ENABLED) {
    debugPrint("ESP-NOW initialized");
    debugPrint("Debug mode: %s", debugEnabled ? "ENABLED" : "DISABLED");
    debugPrint("ESP-NOW Pedal Transmitter Mode: %s", detectedMode == PEDAL_MODE_DUAL ? "DUAL" : "SINGLE");
  }
  
  // Small delay to ensure ESP-NOW is fully ready
  delay(100);
  
  // CRITICAL: If we restored pairing from NVS, add the peer now
  if (pairingState_isPaired(&pairingState)) {
    bool peerAdded = espNowTransport_addPeer(&transport, pairingState.pairedReceiverMAC, 0);
    if (DEBUG_ENABLED) {
      if (peerAdded) {
        debugPrint("Added restored receiver peer to ESP-NOW");
      } else {
        debugPrint("Failed to add restored receiver peer to ESP-NOW");
      }
    }
    delay(20);  // Delay to ensure peer is ready
    
    // If waking from deep sleep, send MSG_PAIRING_CONFIRMED to saved receiver
    // This tells the receiver "I'm back online, do you still have a slot for me?"
    if (wokeFromDeepSleep) {
      pairing_confirmed_message confirmMsg;
      confirmMsg.msgType = MSG_PAIRING_CONFIRMED;
      memcpy(confirmMsg.receiverMAC, pairingState.pairedReceiverMAC, 6);
      
      bool sent = espNowTransport_send(&transport, pairingState.pairedReceiverMAC, (uint8_t*)&confirmMsg, sizeof(confirmMsg));
      if (DEBUG_ENABLED) {
        if (sent) {
          debugPrint("Sent MSG_PAIRING_CONFIRMED to saved receiver (waking from deep sleep)");
        } else {
          debugPrint("Failed to send MSG_PAIRING_CONFIRMED to saved receiver");
        }
      }
      
      // Track when we sent MSG_PAIRING_CONFIRMED - if no ACK within 1s, send MSG_TRANSMITTER_ONLINE
      if (sent) {
        pairingService.pairingConfirmedSentTime = millis();
        pairingService.waitingForPairingConfirmedAck = true;
      }
    }
  } else {
    // Not paired - no MAC saved, broadcast MSG_TRANSMITTER_ONLINE for discovery
    pairingService_broadcastOnline(&pairingService);
  }
  
  // Additional delay to ensure messages complete
  delay(50);
  
  // CRITICAL: If woke from deep sleep and pedal is pressed, send pedal event immediately
  // This must happen AFTER ESP-NOW is fully initialized and ready
  if (wokeFromDeepSleep && pedalPressedOnWakeup && pairingState_isPaired(&pairingState)) {
    if (DEBUG_ENABLED) {
      debugPrint("Woke from deep sleep with pedal pressed - sending pedal event");
    }
    
    // Verify ESP-NOW is initialized before sending
    if (!transport.initialized) {
      if (DEBUG_ENABLED) {
        debugPrint("ESP-NOW not initialized - cannot send pedal event");
      }
    } else {
      // Ensure peer is added before sending (critical for reliable delivery)
      uint8_t* receiverMAC = pairingState.pairedReceiverMAC;
      bool peerAdded = espNowTransport_addPeer(&transport, receiverMAC, 0);
      delay(20);  // Delay to ensure peer is ready
      
      if (peerAdded) {
        // Send pedal press events for whichever pedals were pressed on wakeup
        if (pedal1PressedOnWakeup) {
          pedalService_sendPedalEvent(&pedalService, '1', true);
          delay(50);  // Give time for message to be sent
          // CRITICAL: Update PedalReader's lastState to LOW since we just sent a press event
          // This ensures that if the pedal is released before interrupts are attached,
          // we can detect the state change and send a release event
          pedalReader.pedal1State.lastState = LOW;
          
          // Also check current pedal state - if still pressed, send again to be safe
          pinMode(PEDAL_LEFT_NO_PIN, INPUT_PULLUP);
          delay(5);
          if (digitalRead(PEDAL_LEFT_NO_PIN) == LOW) {
            if (DEBUG_ENABLED) {
              debugPrint("Pedal 1 still pressed - sending pedal event again");
            }
            pedalService_sendPedalEvent(&pedalService, '1', true);
            delay(50);
          }
        }
        
        // Handle pedal 2 (right) if in dual mode and it was pressed
        if (detectedMode == PEDAL_MODE_DUAL && pedal2PressedOnWakeup) {
          pedalService_sendPedalEvent(&pedalService, '2', true);
          delay(50);  // Give time for message to be sent
          // CRITICAL: Update PedalReader's lastState to LOW since we just sent a press event
          pedalReader.pedal2State.lastState = LOW;
          
          // Also check current pedal state - if still pressed, send again to be safe
          pinMode(PEDAL_RIGHT_NO_PIN, INPUT_PULLUP);
          delay(5);
          if (digitalRead(PEDAL_RIGHT_NO_PIN) == LOW) {
            if (DEBUG_ENABLED) {
              debugPrint("Pedal 2 still pressed - sending pedal event again");
            }
            pedalService_sendPedalEvent(&pedalService, '2', true);
            delay(50);
          }
        }
      } else {
        if (DEBUG_ENABLED) {
          debugPrint("Failed to add peer - cannot send pedal event");
        }
      }
    }
    
    onActivity();  // Reset activity timer
  }
  
  // Attach interrupts LAST - after everything is initialized
  // This prevents interrupts from firing during initialization and causing watchdog timeouts
  delay(50);  // Small delay to ensure everything is stable before enabling interrupts
  
  attachInterrupt(digitalPinToInterrupt(DEBUG_BUTTON_PIN), debugButtonISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PEDAL_LEFT_NO_PIN), pedal1ISR, CHANGE);
  pedalReader.interruptAttached1 = true;  // Mark interrupt as attached
  if (detectedMode == PEDAL_MODE_DUAL) {
    attachInterrupt(digitalPinToInterrupt(PEDAL_RIGHT_NO_PIN), pedal2ISR, CHANGE);
    pedalReader.interruptAttached2 = true;  // Mark interrupt as attached
  }
  
  // CRITICAL: After attaching interrupts, check if pedal state changed during initialization
  // If we sent a press event on wake but the pedal is now released, send a release event
  if (wokeFromDeepSleep && pedalPressedOnWakeup && pairingState_isPaired(&pairingState)) {
    delay(10);  // Small delay to let interrupts stabilize
    
    // Check pedal 1 (left) if it was pressed on wakeup
    if (pedal1PressedOnWakeup) {
      bool currentPedal1State = digitalRead(PEDAL_LEFT_NO_PIN);
      if (currentPedal1State == HIGH && pedalReader.pedal1State.lastState == LOW) {
        // Pedal 1 is now HIGH (released) but we sent a press event (lastState is LOW)
        if (DEBUG_ENABLED) {
          debugPrint("Pedal 1 released during initialization - sending release event");
        }
        pedalService_sendPedalEvent(&pedalService, '1', false);
        pedalReader.pedal1State.lastState = HIGH;  // Update state to match current state
      }
    }
    
    // Check pedal 2 (right) if in dual mode and it was pressed on wakeup
    if (detectedMode == PEDAL_MODE_DUAL && pedal2PressedOnWakeup) {
      bool currentPedal2State = digitalRead(PEDAL_RIGHT_NO_PIN);
      if (currentPedal2State == HIGH && pedalReader.pedal2State.lastState == LOW) {
        // Pedal 2 is now HIGH (released) but we sent a press event (lastState is LOW)
        if (DEBUG_ENABLED) {
          debugPrint("Pedal 2 released during initialization - sending release event");
        }
        pedalService_sendPedalEvent(&pedalService, '2', false);
        pedalReader.pedal2State.lastState = HIGH;  // Update state to match current state
      }
    }
  }
  
  if (DEBUG_ENABLED) {
    debugPrint("Interrupts attached - ready for pedal input");
  }
}

void loop() {
  unsigned long currentTime = millis();
  
  // Process any pending discovery requests (deferred from ESP-NOW callback)
  // This must be done in main loop, not in callback context
  pairingService_processPendingDiscovery(&pairingService);
  
  // Process any pending debug messages (deferred from ESP-NOW callback after sending)
  if (hasPendingDebugMessage) {
    hasPendingDebugMessage = false;
    debugPrint("%s", pendingDebugMessage);
  }
  
  // Check discovery timeout
  if (pairingService_checkDiscoveryTimeout(&pairingService, currentTime)) {
    debugPrint("Discovery response timeout");
  }
  
  // Check if MSG_PAIRING_CONFIRMED was sent and no ACK received within timeout
  // If timeout occurred, send MSG_TRANSMITTER_ONLINE for discovery
  if (pairingService.waitingForPairingConfirmedAck && pairingService.pairingConfirmedSentTime > 0) {
    unsigned long timeSinceSent = currentTime - pairingService.pairingConfirmedSentTime;
    if (timeSinceSent >= PAIRING_CONFIRMED_TIMEOUT_MS) {
      // Timeout - no ACK received, send MSG_TRANSMITTER_ONLINE for discovery
      pairingService.waitingForPairingConfirmedAck = false;
      pairingService.pairingConfirmedSentTime = 0;
      
      if (DEBUG_ENABLED) {
        debugPrint("MSG_PAIRING_CONFIRMED timeout - no ACK received, sending MSG_TRANSMITTER_ONLINE");
      }
      pairingService_broadcastOnline(&pairingService);
    }
  }
  
  // CRITICAL: Check if pedal is currently pressed and reset activity timer
  // This prevents the timer from counting up while pedal is held down
  bool pedalPressed = (digitalRead(PEDAL_LEFT_NO_PIN) == LOW);
  if (pedalReader.pedalMode == 0) {  // DUAL mode
    bool rightPedalPressed = (digitalRead(PEDAL_RIGHT_NO_PIN) == LOW);
    pedalPressed = pedalPressed || rightPedalPressed;
  }
  
  if (pedalPressed) {
    // Pedal is currently pressed - reset activity timer to prevent sleep
    onActivity();
  }
  
  // Check inactivity timeout and enter deep sleep if inactive for 5 minutes
  // Handle millis() wrap-around (happens after ~49 days) and ensure lastActivityTime is valid
  unsigned long timeSinceActivity;
  if (lastActivityTime == 0) {
    // Not initialized yet - treat as just started
    timeSinceActivity = 0;
  } else if (currentTime >= lastActivityTime) {
    timeSinceActivity = currentTime - lastActivityTime;
  } else {
    // Wrap-around case: calculate time since wrap (shouldn't happen in practice)
    timeSinceActivity = ((unsigned long)-1 - lastActivityTime) + currentTime + 1;
  }
  if (timeSinceActivity > INACTIVITY_TIMEOUT) {
    // Don't go to sleep if pedal is currently pressed
    if (pedalPressed) {
      onActivity();
    } else {
      debugPrint("Inactivity timeout reached: %lu ms - entering deep sleep", timeSinceActivity);
    goToDeepSleep();
  }
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
          debugEnabled = !debugEnabled;
          debugPrint("Debug mode toggled: %s", debugEnabled ? "ENABLED" : "DISABLED");
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
    delay(isPaired ? IDLE_DELAY_PAIRED : IDLE_DELAY_UNPAIRED);
  } else {
    yield();  // Work to do - process immediately
  }
}

// Include implementation files (Arduino IDE doesn't auto-compile .cpp files in subdirectories)
#include "shared/domain/PairingState.cpp"
#include "shared/domain/PedalReader.cpp"
#include "shared/debug_format.cpp"
#include "shared/infrastructure/EspNowTransport.cpp"
#include "shared/infrastructure/TransmitterUtils.cpp"
#include "shared/application/PairingService.cpp"
#include "shared/application/PedalService.cpp"
