#include "Persistence.h"
#include <Preferences.h>
#include <string.h>
#include <Arduino.h>

Preferences preferences;

void persistence_save(TransmitterManager* manager) {
  preferences.begin("pedal", false);
  preferences.putInt("pairedCount", manager->count);
  preferences.putInt("pedalSlotsUsed", manager->slotsUsed);
  
  for (int i = 0; i < manager->count; i++) {
    char macKey[12];
    char modeKey[12];
    snprintf(macKey, sizeof(macKey), "mac%d", i);
    snprintf(modeKey, sizeof(modeKey), "mode%d", i);
    
    for (int j = 0; j < 6; j++) {
      char key[15];
      snprintf(key, sizeof(key), "%s_%d", macKey, j);
      preferences.putUChar(key, manager->transmitters[i].mac[j]);
    }
    preferences.putUChar(modeKey, manager->transmitters[i].pedalMode);
  }
  
  preferences.end();
}

void persistence_load(TransmitterManager* manager) {
  preferences.begin("pedal", true);
  manager->count = preferences.getInt("pairedCount", 0);
  // Don't restore slotsUsed - it will be calculated based on responsive transmitters
  manager->slotsUsed = 0;
  
  for (int i = 0; i < manager->count && i < MAX_PEDAL_SLOTS; i++) {
    char macKey[12];
    char modeKey[12];
    snprintf(macKey, sizeof(macKey), "mac%d", i);
    snprintf(modeKey, sizeof(modeKey), "mode%d", i);
    
    for (int j = 0; j < 6; j++) {
      char key[15];
      snprintf(key, sizeof(key), "%s_%d", macKey, j);
      manager->transmitters[i].mac[j] = preferences.getUChar(key, 0);
    }
    manager->transmitters[i].pedalMode = preferences.getUChar(modeKey, 0);
    manager->transmitters[i].seenOnBoot = false;  // Will be set to true when transmitter responds
    manager->transmitters[i].lastSeen = 0;
  }
  
  preferences.end();
}

void persistence_saveDebugMonitor(const uint8_t* mac) {
  preferences.begin("pedal", false);
  for (int j = 0; j < 6; j++) {
    char key[15];
    snprintf(key, sizeof(key), "dbgmon_%d", j);
    preferences.putUChar(key, mac[j]);
  }
  preferences.putBool("dbgmon_paired", true);
  preferences.end();
}

void persistence_loadDebugMonitor(uint8_t* mac, bool* isPaired) {
  preferences.begin("pedal", true);
  bool dbgMonSaved = preferences.getBool("dbgmon_paired", false);
  if (dbgMonSaved) {
    bool allZero = true;
    for (int j = 0; j < 6; j++) {
      char key[15];
      snprintf(key, sizeof(key), "dbgmon_%d", j);
      mac[j] = preferences.getUChar(key, 0);
      if (mac[j] != 0) allZero = false;
    }
    *isPaired = !allZero;
  } else {
    *isPaired = false;
  }
  preferences.end();
}

