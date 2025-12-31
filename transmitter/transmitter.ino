#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// ============================================================================
// CONFIGURATION - Set your pedal mode here
// ============================================================================
// Pedal mode options:
//   0 = DUAL_PEDAL (both LEFT and RIGHT pedals active)
//   1 = SINGLE_PEDAL_1 (LEFT pedal only)
//   2 = SINGLE_PEDAL_2 (RIGHT pedal only)
#define PEDAL_MODE 1  // Change this to 0, 1, or 2
// ============================================================================

#define SINGLE_PEDAL_PIN 13  // GPIO pin for single pedal mode (used in SINGLE_PEDAL_1 and SINGLE_PEDAL_2)
#define DUAL_LEFT_PIN 13     // GPIO pin for LEFT pedal in dual pedal mode
#define DUAL_RIGHT_PIN 14   // GPIO pin for RIGHT pedal in dual pedal mode
#define LED_PIN 2 // Pin that controls our board's built-in LED.  Your board may not have this feature.  I recommend the FireBeetle ESP32 for maximum battery life.
#define INACTIVITY_TIMEOUT 600000  // 10 Minute inactivity timeout in milliseconds.  Change as you like.
#define DEBOUNCE_DELAY 50  // Debounce delay in milliseconds to prevent contact bounce
#define LEFT_PEDAL_KEY 'l'   // Key to send for left pedal
#define RIGHT_PEDAL_KEY 'r'  // Key to send for right pedal

// Pedal mode constants
#define DUAL_PEDAL 0
#define SINGLE_PEDAL_1 1
#define SINGLE_PEDAL_2 2

// Structure to send pedal events
typedef struct __attribute__((packed)) struct_message {
  char key;       // Key character to press ('l' or 'r')
  bool pressed;   // true = press, false = release
} struct_message;

unsigned long lastActivityTime = 0;  // Last activity timestamp
bool lastleftPEDALState = HIGH;  // Last state of LEFT PEDAL
bool lastrightPEDALState = HIGH;  // Last state of RIGHT PEDAL
unsigned long leftDebounceTime = 0;  // When LEFT pedal debounce started
unsigned long rightDebounceTime = 0;  // When RIGHT pedal debounce started
bool leftDebouncing = false;  // Is LEFT pedal currently debouncing
bool rightDebouncing = false;  // Is RIGHT pedal currently debouncing

// ESPNOW peer address - MUST match the receiver's MAC address
// Receiver MAC: a0:85:e3:e0:8e:a8
uint8_t broadcastAddress[] = {0xa0, 0x85, 0xe3, 0xe0, 0x8e, 0xa8};

uint64_t ext1_wakeup_mask = 1ULL << DUAL_LEFT_PIN | (1ULL << DUAL_RIGHT_PIN); // IF your board is an ESP32-S2, ESP32-S3, ESP32-C6 or ESP32-H2, this will allow a wakeup from BOTH pedals.

void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP-NOW Pedal Transmitter");
  
  // Print pedal mode configuration
  Serial.print("Pedal Mode: ");
  if (PEDAL_MODE == DUAL_PEDAL) {
    Serial.println("DUAL PEDAL (both LEFT and RIGHT)");
  } else if (PEDAL_MODE == SINGLE_PEDAL_1) {
    Serial.println("SINGLE PEDAL 1 (LEFT only)");
  } else if (PEDAL_MODE == SINGLE_PEDAL_2) {
    Serial.println("SINGLE PEDAL 2 (RIGHT only)");
  }

  // Configure pins based on pedal mode
  if (PEDAL_MODE == DUAL_PEDAL) {
    pinMode(DUAL_LEFT_PIN, INPUT_PULLUP);   // Pin 13 - LEFT pedal in dual mode
    pinMode(DUAL_RIGHT_PIN, INPUT_PULLUP);  // Pin 14 - RIGHT pedal in dual mode
  } else {
    pinMode(SINGLE_PEDAL_PIN, INPUT_PULLUP);  // Pin 13 - single pedal mode
  }
  pinMode(LED_PIN, OUTPUT);  // Controls built-in LED.  Your board may not have this feature.
 
  // Initialize ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW.");
    return;
  }
  
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  // Default channel
  peerInfo.encrypt = false;  // No encryption
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
  
  Serial.println("ESP-NOW initialized successfully");
  Serial.print("Receiver MAC: ");
  for (int i = 0; i < 6; i++) {
    Serial.print(broadcastAddress[i], HEX);
    if (i < 5) Serial.print(":");
  }
  Serial.println();

  // Set initial activity time
  lastActivityTime = millis();
}

void loop() {
  // Read the current state of Pedal(s)
  bool currentleftPEDALState, currentrightPEDALState;
  if (PEDAL_MODE == DUAL_PEDAL) {
    currentleftPEDALState = digitalRead(DUAL_LEFT_PIN);
    currentrightPEDALState = digitalRead(DUAL_RIGHT_PIN);
  } else {
    // In single pedal mode, read from SINGLE_PEDAL_PIN
    bool singlePedalState = digitalRead(SINGLE_PEDAL_PIN);
    if (PEDAL_MODE == SINGLE_PEDAL_1) {
      currentleftPEDALState = singlePedalState;
      currentrightPEDALState = HIGH;  // Not used
    } else {  // SINGLE_PEDAL_2
      currentleftPEDALState = HIGH;  // Not used
      currentrightPEDALState = singlePedalState;
    }
  }

  // Turn ON Power LED  LOW = OFF, HIGH = ON  Note:  Turning on LED will reduce battery life
  digitalWrite(LED_PIN, LOW);

  // Handle inactivity timeout
  if (millis() - lastActivityTime > INACTIVITY_TIMEOUT) {
    Serial.println("Inactivity timeout. Going to deep sleep.");
    goToDeepSleep();
  }

  unsigned long currentTime = millis();
  
  // LEFT PEDAL handling - pin 13 sends as LEFT pedal (pedal ID 0)
  // Active in DUAL_PEDAL mode or SINGLE_PEDAL_1 mode
  if (PEDAL_MODE == DUAL_PEDAL || PEDAL_MODE == SINGLE_PEDAL_1) {
    if (currentleftPEDALState == LOW && lastleftPEDALState == HIGH) {
      // State changed from HIGH to LOW (pressed) - start debounce
      if (!leftDebouncing) {
        leftDebounceTime = currentTime;
        leftDebouncing = true;
      } else if (currentTime - leftDebounceTime >= DEBOUNCE_DELAY) {
        // Debounce time passed, check if still LOW
        uint8_t pinToRead = (PEDAL_MODE == DUAL_PEDAL) ? DUAL_LEFT_PIN : SINGLE_PEDAL_PIN;
        if (digitalRead(pinToRead) == LOW) {
          // Send PRESS event with key character
          struct_message msg;
          msg.key = LEFT_PEDAL_KEY;  // 'l'
          msg.pressed = true;
          esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
          if (result == ESP_OK) {
            Serial.println("Sent LEFT pedal PRESS");
          } else {
            Serial.print("Error sending LEFT pedal PRESS: ");
            Serial.println(result);
          }
          lastleftPEDALState = LOW;
          resetInactivityTimer();
        }
        leftDebouncing = false;
      }
    } else if (currentleftPEDALState == HIGH && lastleftPEDALState == LOW) {
      // State changed from LOW to HIGH (released) - send release event
      struct_message msg;
      msg.key = LEFT_PEDAL_KEY;  // 'l'
      msg.pressed = false;
      esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
      if (result == ESP_OK) {
        Serial.println("Sent LEFT pedal RELEASE");
      } else {
        Serial.print("Error sending LEFT pedal RELEASE: ");
        Serial.println(result);
      }
      lastleftPEDALState = HIGH;
      leftDebouncing = false;
      resetInactivityTimer();
    } else if (currentleftPEDALState == HIGH && leftDebouncing) {
      // Bounced back to HIGH during debounce - cancel
      leftDebouncing = false;
    }
  }
  
  // RIGHT PEDAL handling
  // In DUAL_PEDAL mode: pin 14 sends as RIGHT pedal (pedal ID 1)
  // In SINGLE_PEDAL_2 mode: pin 13 sends as RIGHT pedal (pedal ID 1)
  if (PEDAL_MODE == DUAL_PEDAL || PEDAL_MODE == SINGLE_PEDAL_2) {
    // In SINGLE_PEDAL_2 mode, currentrightPEDALState contains SINGLE_PEDAL_PIN state
    // In DUAL_PEDAL mode, currentrightPEDALState contains DUAL_RIGHT_PIN state
    bool pedalState = currentrightPEDALState;
    bool lastPedalState = (PEDAL_MODE == DUAL_PEDAL) ? lastrightPEDALState : lastleftPEDALState;
    
    if (pedalState == LOW && lastPedalState == HIGH) {
      // State changed from HIGH to LOW (pressed) - start debounce
      if (!rightDebouncing) {
        rightDebounceTime = currentTime;
        rightDebouncing = true;
      } else if (currentTime - rightDebounceTime >= DEBOUNCE_DELAY) {
        // Debounce time passed, check if still LOW
        uint8_t pinToRead = (PEDAL_MODE == DUAL_PEDAL) ? DUAL_RIGHT_PIN : SINGLE_PEDAL_PIN;
        if (digitalRead(pinToRead) == LOW) {
          // Send PRESS event with key character
          struct_message msg;
          msg.key = RIGHT_PEDAL_KEY;  // 'r'
          msg.pressed = true;
          esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
          if (result == ESP_OK) {
            Serial.println("Sent RIGHT pedal PRESS");
          } else {
            Serial.print("Error sending RIGHT pedal PRESS: ");
            Serial.println(result);
          }
          // Update the appropriate state variable
          if (PEDAL_MODE == DUAL_PEDAL) {
            lastrightPEDALState = LOW;
          } else {
            lastleftPEDALState = LOW;
          }
          resetInactivityTimer();
        }
        rightDebouncing = false;
      }
    } else if (pedalState == HIGH && lastPedalState == LOW) {
      // State changed from LOW to HIGH (released) - send release event
      struct_message msg;
      msg.key = RIGHT_PEDAL_KEY;  // 'r'
      msg.pressed = false;
      esp_err_t result = esp_now_send(broadcastAddress, (uint8_t*)&msg, sizeof(msg));
      if (result == ESP_OK) {
        Serial.println("Sent RIGHT pedal RELEASE");
      } else {
        Serial.print("Error sending RIGHT pedal RELEASE: ");
        Serial.println(result);
      }
      // Update the appropriate state variable
      if (PEDAL_MODE == DUAL_PEDAL) {
        lastrightPEDALState = HIGH;
      } else {
        lastleftPEDALState = HIGH;
      }
      rightDebouncing = false;
      resetInactivityTimer();
    } else if (pedalState == HIGH && rightDebouncing) {
      // Bounced back to HIGH during debounce - cancel
      rightDebouncing = false;
    }
  }
}

// Function to reset the inactivity timer
void resetInactivityTimer() {
  lastActivityTime = millis();
}

// Function to put the ESP32 into deep sleep
void goToDeepSleep() {
  // Turn OFF LED
  digitalWrite(LED_PIN, LOW);
  // Wakeup from deep sleep when PEDAL is pushed/LOW.
  // Always use pin 13 for wakeup since it's used in all modes
  // For DUAL_PEDAL mode with ESP32-S2/S3/C6/H2, you could use ext1_wakeup to wake from both pins:
  //esp_sleep_enable_ext1_wakeup(ext1_wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  uint8_t wakeupPin = (PEDAL_MODE == DUAL_PEDAL) ? DUAL_LEFT_PIN : SINGLE_PEDAL_PIN;
  esp_sleep_enable_ext0_wakeup((gpio_num_t)wakeupPin, LOW);
  Serial.println("Going to deep sleep...");
  esp_deep_sleep_start();
}
