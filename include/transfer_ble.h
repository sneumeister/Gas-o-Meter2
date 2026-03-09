#ifndef TRANSFER_BLE_H
#define TRANSFER_BLE_H

#include "transfer.h"
#include <stddef.h>

#ifndef ARDUINO

bool transfer_ble_init(void);
transfer_status_t transfer_ble_send_data(const transfer_data_t* data);
void transfer_ble_deinit(void);
bool transfer_ble_get_status_json(char* buffer, size_t buffer_size);

// Pairing: Startet BLE-Advertising für BLE_ADVERTISING_DURATION_MS
// Wird von config.html "BLE Pairing starten" aufgerufen
bool transfer_ble_start_pairing(void);
bool transfer_ble_is_advertising(void);

#endif // !ARDUINO
#endif // TRANSFER_BLE_H
