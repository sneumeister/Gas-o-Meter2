#ifndef TRANSFER_MQTT_H
#define TRANSFER_MQTT_H

#include "transfer.h"
#include <stddef.h>

bool transfer_mqtt_init(void);
transfer_status_t transfer_mqtt_send_data(const transfer_data_t* data);
void transfer_mqtt_deinit(void);

/** Verbindungstest mit expliziten Parametern (ohne config_rtc). json_out: {"status":"ok"|"error",...} */
bool transfer_mqtt_test_connection(const char* host, uint16_t port, const char* username,
                                   const char* password, char* json_out, size_t json_out_len);

#endif
