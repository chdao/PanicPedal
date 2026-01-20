#include "PedalModeDetector.h"
#include "../transmitter/TransmitterConfig.h"
#include <Arduino.h>

uint8_t PedalModeDetector::detect() {
  #if HAS_AUTO_DETECT()
  // Configure NC pins as inputs with pull-ups to detect switch connections
  pinMode(PEDAL_1_NC_PIN, INPUT_PULLUP);
  pinMode(PEDAL_2_NC_PIN, INPUT_PULLUP);
  
  // Small delay to allow pins to stabilize
  delay(10);
  
  // Read NC pins multiple times to ensure stable reading (debouncing)
  int pedal1Reads = 0, pedal2Reads = 0;
  for (int i = 0; i < 5; i++) {
    if (digitalRead(PEDAL_1_NC_PIN) == LOW) pedal1Reads++;
    if (digitalRead(PEDAL_2_NC_PIN) == LOW) pedal2Reads++;
    delay(2);
  }
  
  // Consider connected if pin reads LOW in majority of reads
  bool pedal1Connected = (pedal1Reads >= 3);
  bool pedal2Connected = (pedal2Reads >= 3);
  
  extern void serialPrint(const char* format, ...);
  serialPrint("Pedal detection: PEDAL_1_NC=%s (%d/5 LOW), PEDAL_2_NC=%s (%d/5 LOW)",
               pedal1Connected ? "CONNECTED" : "NOT CONNECTED", pedal1Reads,
               pedal2Connected ? "CONNECTED" : "NOT CONNECTED", pedal2Reads);
  
  // Determine mode based on detected switches
  uint8_t detectedMode;
  if (pedal1Connected && pedal2Connected) {
    detectedMode = PEDAL_MODE_DUAL;
    serialPrint("Detected mode: DUAL (both pedals)");
  } else if (pedal1Connected) {
    detectedMode = PEDAL_MODE_SINGLE;
    serialPrint("Detected mode: SINGLE (pedal 1 only)");
  } else if (pedal2Connected) {
    detectedMode = PEDAL_MODE_SINGLE;
    serialPrint("Detected mode: SINGLE (pedal 2 detected, using PEDAL_1_PIN)");
  } else {
    detectedMode = PEDAL_MODE_SINGLE;
    serialPrint("WARNING: No pedals detected via NC pins, defaulting to SINGLE mode");
  }
  
  return detectedMode;
  #else
  // Auto-detect not supported - return manual mode
  return PEDAL_MODE;
  #endif
}
