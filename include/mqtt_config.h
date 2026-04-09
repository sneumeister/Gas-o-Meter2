#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#define MQTT_TOPIC_SUFFIX_GAS "gas"
#define MQTT_TOPIC_SUFFIX_BATTERY "battery"
#define MQTT_TOPIC_SUFFIX_BATTERY_VOLTAGE "battery_voltage"
#define MQTT_TOPIC_SUFFIX_BATTERY_LOW "battery_low"
#define MQTT_TOPIC_SUFFIX_FIRMWARE_VERSION "firmware_version"
#define MQTT_TOPIC_SUFFIX_TIMESTAMP "timestamp"
#define MQTT_TOPIC_SUFFIX_DATA "data"
#define MQTT_TOPIC_SUFFIX_RSSI "rssi"
#define MQTT_TOPIC_SUFFIX_NTP_STATUS "ntp_status"

#define MQTT_HOST_MAX_LEN 63
#define MQTT_USERNAME_MAX_LEN 63
#define MQTT_PASSWORD_MAX_LEN 63
#define MQTT_MAIN_TOPIC_MAX_LEN 63

#define MQTT_DEFAULT_PORT 1883
/**
 * Fallback für mqtt_main_topic, wenn noch kein Hostname aus der Config bekannt ist.
 * Laufzeit-Default nach Laden der Config: bevorzugt hostname (≤ HOSTNAME_MAX_LEN), siehe load_config().
 */
#define MQTT_DEFAULT_MAIN_TOPIC "gas-o-meter2"
#define MQTT_DUMMY_HOST "dummy_mqtt_host"

#define MQTT_CONNECT_TIMEOUT_MS 5000
#define MQTT_PUBLISH_TIMEOUT_MS 3000
#define MQTT_MAX_PUBLISH_ATTEMPTS 3
#define MQTT_PUBLISH_RETRY_DELAY_MS 200
#define MQTT_DISCONNECT_TIMEOUT_MS 1000

#define MQTT_QOS 1
#define MQTT_RETAIN true

/** Home Assistant MQTT Discovery: fester Geräte-Präfix im Config-Topic (siehe Doku). */
#define MQTT_HA_DEVICE_TOPIC_PREFIX "gas_o_meter2"
#define MQTT_HA_MANUFACTURER "Custom"
#define MQTT_HA_MODEL "Gas-O-Meter2"

#endif
