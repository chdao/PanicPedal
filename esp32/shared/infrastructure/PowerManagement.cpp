#include "PowerManagement.h"
#include "../transmitter/TransmitterConfig.h"
#include "../transmitter/TransmitterConstants.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_task_wdt.h"
#include <WiFi.h>
#include <esp_now.h>

// ESP-IDF constant compatibility - cast to enum type if constant doesn't exist
#ifndef ESP_EXT1_WAKEUP_ANY_LOW
#define ESP_EXT1_WAKEUP_ANY_LOW ((esp_sleep_ext1_wakeup_mode_t)0)  // Wake when ANY pin goes LOW
#endif

void PowerManagement::goToDeepSleep(PedalReader* pedalReader, EspNowTransport* transport, EspNowTransport** debugTransport) {
  // CRITICAL: Disable debug transport FIRST to prevent any ESP-NOW operations
  if (debugTransport) {
    *debugTransport = nullptr;
  }
  transport->initialized = false;
  
  // CRITICAL: Detach interrupts to prevent them from firing during deep sleep prep
  detachInterrupt(digitalPinToInterrupt(PEDAL_1_PIN));
  if (pedalReader->pedalMode == 0) {  // 0 = DUAL mode
    detachInterrupt(digitalPinToInterrupt(PEDAL_2_PIN));
  }
  detachInterrupt(digitalPinToInterrupt(DEBUG_BUTTON_PIN));
  
  // Disable watchdog timer to prevent timeouts during deep sleep prep
  esp_task_wdt_deinit();
  
  // Deinitialize ESP-NOW before deep sleep
  esp_now_deinit();
  
  // Turn off WiFi
  WiFi.mode(WIFI_OFF);
  
  // Configure deep sleep wakeup - always wake on ANY pedal press (simpler and more robust)
  // Always use EXT1 wakeup to support both pedals
  
  // Configure NC pins as RTC GPIOs if available
  #if HAS_AUTO_DETECT()
  rtc_gpio_init((gpio_num_t)PEDAL_1_NC_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_1_NC_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_1_NC_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_1_NC_PIN);
  
  rtc_gpio_init((gpio_num_t)PEDAL_2_NC_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_2_NC_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_2_NC_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_2_NC_PIN);
  #endif
  
  // Configure both pedal pins as RTC GPIOs for ext1 wakeup (always configure both)
  rtc_gpio_init((gpio_num_t)PEDAL_1_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_1_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_1_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_1_PIN);
  
  rtc_gpio_init((gpio_num_t)PEDAL_2_PIN);
  rtc_gpio_set_direction((gpio_num_t)PEDAL_2_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en((gpio_num_t)PEDAL_2_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PEDAL_2_PIN);
  
  delay(20);  // Allow RTC GPIO pull-up to stabilize
  
  // Check if pedals are pressed - don't sleep if pressed
  int pedal1HighReads = 0;
  int pedal2HighReads = 0;
  for (int i = 0; i < 10; i++) {
    if (digitalRead(PEDAL_1_PIN) == HIGH) pedal1HighReads++;
    if (digitalRead(PEDAL_2_PIN) == HIGH) pedal2HighReads++;
    delay(5);
  }
  
  if (pedal1HighReads < 8 || pedal2HighReads < 8) {
    delay(20);
    if (digitalRead(PEDAL_1_PIN) == LOW || digitalRead(PEDAL_2_PIN) == LOW) {
      return;  // Skip sleep if a pedal is definitely pressed
    }
  }
  
  // Configure ext1 wakeup: PEDAL_1_PIN OR PEDAL_2_PIN, LOW trigger (wake on any pedal)
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  uint64_t gpioMask = (1ULL << PEDAL_1_PIN) | (1ULL << PEDAL_2_PIN);
  // Use ESP_EXT1_WAKEUP_ANY_LOW (0) = wake when ANY pin goes LOW (what we want)
  // ESP_EXT1_WAKEUP_ALL_LOW (1) = wake when ALL pins are LOW (not what we want)
  esp_err_t wakeupResult = esp_sleep_enable_ext1_wakeup(gpioMask, ESP_EXT1_WAKEUP_ANY_LOW);
  
  if (wakeupResult != ESP_OK) {
    return;
  }
  
  delay(10);
  if (digitalRead(PEDAL_1_PIN) == LOW || digitalRead(PEDAL_2_PIN) == LOW) {
    return;  // Pedal pressed - will wake immediately
  }
  
  // NO Serial operations - they can cause crashes during deep sleep entry
  // Enter deep sleep immediately
  esp_deep_sleep_start();
  // Code never reaches here - device enters deep sleep
}

bool PowerManagement::shouldEnterDeepSleep(unsigned long lastActivityTime, unsigned long currentTime, PedalReader* pedalReader) {
  // Check if any pedal is currently pressed (always check both, regardless of mode)
  bool pedalPressed = (digitalRead(PEDAL_1_PIN) == LOW) || (digitalRead(PEDAL_2_PIN) == LOW);
  
  if (pedalPressed) {
    return false;  // Don't sleep if pedal is pressed
  }
  
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
  
  return (timeSinceActivity > INACTIVITY_TIMEOUT_MS);
}

void PowerManagement::resetActivityTimer(unsigned long* lastActivityTime, PedalReader* pedalReader) {
  // Check if any pedal is currently pressed (always check both, regardless of mode)
  bool pedalPressed = (digitalRead(PEDAL_1_PIN) == LOW) || (digitalRead(PEDAL_2_PIN) == LOW);
  
  if (pedalPressed) {
    // Pedal is currently pressed - reset activity timer to prevent sleep
    *lastActivityTime = millis();
  }
}
