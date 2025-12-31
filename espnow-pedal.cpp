#include <WiFi.h>
#include <esp_now.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

USBHIDKeyboard Keyboard;

// Structure to receive data (must match transmitter)
typedef struct struct_message {
  bool pinState;
} struct_message;

struct_message receivedData;

// Callback function for ESP-NOW receiving
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  memcpy(&receivedData, incomingData, sizeof(receivedData));
  char receivedChar = incomingData[0];
   // receivedData.pinState == LOW) { // Check if received state is LOW (0)
    Serial.println("Sending received letter...");
    Keyboard.press(receivedChar);
    delay(50); // Debounce
    Keyboard.release(receivedChar);
  
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Receiver");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);

  //USB.begin(); // Initialize USB OTG

  Serial.println("USB Connected");
 //Keyboard.begin();
}

void loop() {
  // ESP-NOW receive is handled in the callback
}
