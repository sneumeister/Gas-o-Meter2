#ifndef TRANSFER_ZIGBEE_H
#define TRANSFER_ZIGBEE_H

#include "transfer.h"

// ============================================
// ZigBee-spezifische Funktionen
// ============================================

/**
 * @brief Initialisiert den ZigBee-Stack
 * 
 * @return true bei Erfolg, false bei Fehler
 */
bool transfer_zigbee_init(void);

/**
 * @brief Führt die ZigBee-Datenübertragung durch
 * 
 * @param data Zu übertragende Daten
 * @return transfer_status_t Status-Code
 */
transfer_status_t transfer_zigbee_send_data(const transfer_data_t* data);

/**
 * @brief Synchronisiert die Zeit vom ZigBee-Coordinator
 * 
 * @return true bei Erfolg, false bei Fehler
 */
bool transfer_zigbee_sync_time(void);

/**
 * @brief Deinitialisiert den ZigBee-Stack
 */
void transfer_zigbee_deinit(void);

#endif // TRANSFER_ZIGBEE_H
