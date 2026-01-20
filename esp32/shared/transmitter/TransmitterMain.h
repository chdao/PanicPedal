#ifndef TRANSMITTER_MAIN_H
#define TRANSMITTER_MAIN_H

#include <Arduino.h>
#include "../domain/PairingState.h"
#include "../domain/PedalReader.h"
#include "../infrastructure/EspNowTransport.h"
#include "../application/PairingService.h"
#include "../application/PedalService.h"

// Forward declarations for ISRs (implemented in PedalReader.cpp)
extern void IRAM_ATTR pedal1ISR();
extern void IRAM_ATTR pedal2ISR();

// Global state (defined in .ino file)
extern PairingState pairingState;
extern PedalReader pedalReader;
extern EspNowTransport transport;
extern PairingService pairingService;
extern PedalService pedalService;
extern unsigned long lastActivityTime;
extern unsigned long bootTime;
extern volatile bool debugButtonInterruptFlag;
extern unsigned long debugButtonLastDebounceTime;
extern bool debugButtonLastState;
extern bool debugButtonPressed;

// Main functions (implemented in TransmitterMain.cpp)
void transmitterMain_setup();
void transmitterMain_loop();

#endif // TRANSMITTER_MAIN_H
