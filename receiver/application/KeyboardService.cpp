#include "KeyboardService.h"
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <string.h>
#include <Arduino.h>

USBHIDKeyboard Keyboard;

void keyboardService_init(KeyboardService* service, TransmitterManager* manager) {
  service->manager = manager;
  memset(service->keysPressed, 0, sizeof(service->keysPressed));
  
  USB.begin();
  delay(500);
  Keyboard.begin();
  delay(2000);
}

void keyboardService_handlePedalEvent(KeyboardService* service, const uint8_t* txMAC, 
                                       const struct_message* msg) {
  int transmitterIndex = transmitterManager_findIndex(service->manager, txMAC);
  if (transmitterIndex < 0) {
    return;  // Unknown transmitter
  }
  
  // Update last seen
  service->manager->transmitters[transmitterIndex].lastSeen = millis();
  
  // Determine key to press
  char keyToPress;
  if (service->manager->transmitters[transmitterIndex].pedalMode == 0) {
    // DUAL pedal: '1' -> 'l', '2' -> 'r'
    keyToPress = (msg->key == '1') ? 'l' : 'r';
  } else {
    // SINGLE pedal: '1' -> assigned key based on pairing order
    if (msg->key != '1') return;
    keyToPress = transmitterManager_getAssignedKey(service->manager, transmitterIndex);
  }
  
  uint8_t keyIndex = (uint8_t)keyToPress;
  
  if (msg->pressed) {
    if (!service->keysPressed[keyIndex]) {
      Keyboard.press(keyToPress);
      service->keysPressed[keyIndex] = true;
    }
  } else {
    if (service->keysPressed[keyIndex]) {
      Keyboard.release(keyToPress);
      service->keysPressed[keyIndex] = false;
    }
  }
}

