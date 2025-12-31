/*
 * Run This First - Get Receiver MAC Address
 * 
 * Upload this sketch to your ESP32-S2/S3 receiver first to get its MAC address.
 * The MAC address will be displayed in the Serial Monitor.
 * Copy this MAC address and update it in transmitter/transmitter.ino
 */

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(2000); // Give Serial time to initialize
  
  Serial.println("========================================");
  Serial.println("ESPNow Pedal Receiver - MAC Address");
  Serial.println("========================================");
  Serial.println();
  
  // Initialize WiFi to get MAC address
  WiFi.mode(WIFI_STA);
  delay(100); // Give WiFi time to initialize before reading MAC address
  
  // Get and display MAC address
  String macAddress = WiFi.macAddress();
  Serial.print("Receiver MAC Address: ");
  Serial.println(macAddress);
  Serial.println();
  
  // Also display in hex format for easy copy-paste
  Serial.println("For transmitter configuration, use this format:");
  Serial.print("uint8_t broadcastAddress[] = {");
  
  uint8_t mac[6];
  WiFi.macAddress(mac);
  
  for (int i = 0; i < 6; i++) {
    Serial.print("0x");
    if (mac[i] < 0x10) Serial.print("0");
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(", ");
  }
  Serial.println("};");
  Serial.println();
  Serial.println("========================================");
  Serial.println("Copy the MAC address above and update");
  Serial.println("transmitter/transmitter.ino line ~45");
  Serial.println("========================================");
}

void loop() {
  // Nothing to do here
  delay(1000);
}

