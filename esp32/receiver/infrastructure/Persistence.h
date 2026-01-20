#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <stdint.h>
#include <stdbool.h>
#include "../domain/TransmitterManager.h"

void persistence_save(TransmitterManager* manager);
void persistence_load(TransmitterManager* manager);
void persistence_saveDebugMonitor(const uint8_t* mac);
void persistence_loadDebugMonitor(uint8_t* mac, bool* isPaired);
void persistence_clear();  // Clear all stored transmitter MAC addresses

#endif // PERSISTENCE_H

