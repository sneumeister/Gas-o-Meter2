#ifndef TRANSFER_MQTT_H
#define TRANSFER_MQTT_H

#include "transfer.h"

#ifndef ARDUINO

bool transfer_mqtt_init(void);
transfer_status_t transfer_mqtt_send_data(const transfer_data_t* data);
void transfer_mqtt_deinit(void);

#endif

#endif
