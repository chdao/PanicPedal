#include <WiFi.h>
#include <esp_now.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

USBHIDKeyboard Keyboard;

// Structure to receive pedal events (must match transmitter)
typedef struct __attribute__((packed)) struct_message {
  char key;       // Key character to press ('l' or 'r')
  bool pressed;   // true = press, false = release
} struct_message;

// Track which keys are currently pressed (using the key character as identifier)
bool keysPressed[256];  // Track all possible keys

void initializeKeysPressed() {
  for (int i = 0; i < 256; i++) {
    keysPressed[i] = false;
  }
}

// Callback function for ESP-NOW receiving
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  Serial.println("*** ESP-NOW data received! ***");
  
  if (len < sizeof(struct_message)) {
    Serial.println("Error: Received data too short");
    return;
  }
  
  // Parse the message
  struct_message msg;
  memcpy(&msg, incomingData, sizeof(msg));
  
  Serial.print("Received - Key: '");
  Serial.print(msg.key);
  Serial.print("' (ASCII: ");
  Serial.print((uint8_t)msg.key);
  Serial.print("), Action: ");
  Serial.println(msg.pressed ? "PRESS" : "RELEASE");
  
  // Press or release whatever key was sent by the transmitter
  uint8_t keyIndex = (uint8_t)msg.key;
  if (msg.pressed) {
    // Key PRESS
    if (!keysPressed[keyIndex]) {
      Keyboard.press(msg.key);
      keysPressed[keyIndex] = true;
      Serial.print("Key '");
      Serial.print(msg.key);
      Serial.println("' PRESSED and held");
    } else {
      Serial.print("Key '");
      Serial.print(msg.key);
      Serial.println("' already pressed (ignoring duplicate)");
    }
  } else {
    // Key RELEASE
    if (keysPressed[keyIndex]) {
      Keyboard.release(msg.key);
      keysPressed[keyIndex] = false;
      Serial.print("Key '");
      Serial.print(msg.key);
      Serial.println("' RELEASED");
    } else {
      Serial.print("Key '");
      Serial.print(msg.key);
      Serial.println("' not pressed (ignoring release)");
    }
  }
}

void setup() {
  // Initialize Serial first (on ESP32-S2/S3 this initializes USB CDC)
  Serial.begin(115200);
  delay(2000); // Give USB time to enumerate and stabilize
  Serial.println("Starting Receiver");
  
  // Initialize keys pressed tracking array
  initializeKeysPressed();

  // Initialize WiFi first
  WiFi.mode(WIFI_STA);
  delay(100); // Give WiFi time to initialize
  
  // Get MAC address BEFORE disconnecting
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  
  // Disconnect WiFi (ESP-NOW doesn't need WiFi connection, just WiFi mode)
  WiFi.disconnect();
  delay(100); // Give WiFi time to disconnect

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  Serial.println("ESP-NOW initialized successfully");

  // Register callback BEFORE initializing Keyboard
  esp_now_register_recv_cb(OnDataRecv);
  Serial.println("ESP-NOW receive callback registered");

  // Initialize Keyboard - this may cause Serial to stop working on some boards
  // If Serial stops, you'll need to use hardware UART or accept that Serial won't work
  Keyboard.begin();
  delay(1000); // Give Keyboard time to initialize and be recognized by OS
  Serial.println("Keyboard initialized");
  Serial.println("Receiver ready!");
  Serial.println("If you can see this, Serial is still working!");
}

void loop() {
  // ESP-NOW receive is handled in the callback
}

