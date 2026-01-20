// ============================================================================
// PanicPedal Pro - ESP32-S3-WROOM Transmitter
// ============================================================================
// This file includes device-specific configuration and then uses the unified
// transmitter code from shared/transmitter/
//
// CRITICAL: For ESP32-S3, you MUST enable USB CDC On Boot in Arduino IDE:
// Tools > USB CDC On Boot > Enabled
// Tools > USB Mode > Hardware CDC and JTAG (or USB-OTG (TinyUSB))

// Include constants FIRST (defines PEDAL_MODE constants, etc.)
#include "shared/transmitter/TransmitterConstants.h"
// Then include device-specific configuration (uses constants above)
#include "config.h"
// Finally include TransmitterConfig.h (validates config.h and provides helper macros)
#include "shared/transmitter/TransmitterConfig.h"

// Now include the unified transmitter code
#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include <Preferences.h>

// Clean Architecture: Include shared modules
#include "shared/messages.h"
#include "shared/config.h"
#include "shared/domain/PairingState.h"
#include "shared/domain/PedalReader.h"
#include "shared/domain/MacUtils.h"
#include "shared/infrastructure/EspNowTransport.h"
#include "shared/application/PairingService.h"
#include "shared/application/PedalService.h"
#include "shared/infrastructure/DebugService.h"
#include "shared/infrastructure/PowerManagement.h"
#include "shared/application/MessageHandler.h"
#include "shared/application/WakeupHandler.h"
#include "shared/domain/PedalModeDetector.h"
#include "shared/transmitter/TransmitterMain.h"

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

// Callbacks and ISRs are now in shared/transmitter/TransmitterMain.cpp

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
  transmitterMain_setup();
}

void loop() {
  transmitterMain_loop();
}

// Include implementation files
#include "shared/domain/PairingState.cpp"
#include "shared/domain/PedalReader.cpp"
#include "shared/debug_format.cpp"
#include "shared/infrastructure/EspNowTransport.cpp"
#include "shared/infrastructure/TransmitterUtils.cpp"
#include "shared/application/PairingService.cpp"
#include "shared/application/PedalService.cpp"
#include "shared/infrastructure/DebugService.cpp"
#include "shared/infrastructure/PowerManagement.cpp"
#include "shared/application/MessageHandler.cpp"
#include "shared/application/WakeupHandler.cpp"
#include "shared/domain/PedalModeDetector.cpp"
#include "shared/transmitter/TransmitterMain.cpp"
