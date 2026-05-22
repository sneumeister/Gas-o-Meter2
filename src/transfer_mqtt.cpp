#include "transfer_mqtt.h"
#include "mqtt_config.h"
#include "time_sync.h"
#include "hardware.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <ctime>
#include <string.h>

static const char* TAG = "transfer_mqtt";

extern bool connect_wifi(void);
extern bool sync_ntp_time(void);
extern wifi_ap_record_t ap_info;

extern const char* transfer_mqtt_get_host(void);
extern uint16_t transfer_mqtt_get_port(void);
extern const char* transfer_mqtt_get_username(void);
extern const char* transfer_mqtt_get_password(void);
extern const char* transfer_mqtt_get_main_topic(void);
extern bool transfer_mqtt_get_ha_autodiscovery(void);
extern const char* transfer_mqtt_get_hostname(void);

static bool mqtt_initialized = false;
static volatile bool mqtt_connected = false;
/** Nur diese msg_id (letzter Publish-Versuch) löst Erfolg aus – verspätete PUBACKs älterer IDs ignorieren. */
static volatile int mqtt_pending_msg_id = -1;
static volatile bool mqtt_publish_acked = false;

static void build_topic(char* out, size_t out_size, const char* main_topic, const char* suffix) {
    snprintf(out, out_size, "%s/%s", main_topic, suffix);
}

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            mqtt_connected = true;
            mqtt_pending_msg_id = -1;
            mqtt_publish_acked = false;
            break;
        case MQTT_EVENT_DISCONNECTED:
            mqtt_connected = false;
            break;
        case MQTT_EVENT_PUBLISHED:
            if (event->msg_id == mqtt_pending_msg_id) {
                mqtt_publish_acked = true;
            }
            break;
        default:
            break;
    }
}

/** Wartet auf PUBACK (MQTT_EVENT_PUBLISHED) für mqtt_pending_msg_id; bricht bei Ack sofort ab. */
static bool mqtt_wait_publish_ack(void) {
    uint32_t waited_ms = 0;
    const uint32_t poll_ms = 50;
    while (!mqtt_publish_acked && mqtt_connected && waited_ms < MQTT_PUBLISH_ACK_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }
    return mqtt_publish_acked && mqtt_connected;
}

static bool mqtt_publish_with_retry(esp_mqtt_client_handle_t client, const char* topic, const char* payload) {
    for (int attempt = 0; attempt < MQTT_MAX_PUBLISH_ATTEMPTS; ++attempt) {
        if (!mqtt_connected) {
            return false;
        }
        mqtt_publish_acked = false;
        mqtt_pending_msg_id = -1;

        int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, MQTT_QOS, MQTT_RETAIN);
        if (msg_id < 0) {
            vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_RETRY_DELAY_MS));
            continue;
        }

        mqtt_pending_msg_id = msg_id;
        if (mqtt_wait_publish_ack()) {
            mqtt_pending_msg_id = -1;
            return true;
        }

        ESP_LOGW(TAG, "MQTT PUBACK Timeout (msg_id=%d, topic=%s), Versuch %d/%d", msg_id, topic, attempt + 1,
                 MQTT_MAX_PUBLISH_ATTEMPTS);
        mqtt_pending_msg_id = -1;
        vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_RETRY_DELAY_MS));
    }
    return false;
}

/** Slug für HA-Discovery-Topics: `/` → `_`, Leerzeichen entfernen (laut Projekt-Doku). */
static void mqtt_ha_main_topic_slug(char* out, size_t cap, const char* main_topic) {
    size_t j = 0;
    if (main_topic == nullptr) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; main_topic[i] != '\0' && j + 1 < cap; ++i) {
        if (main_topic[i] == '/') {
            out[j++] = '_';
        } else if (main_topic[i] != ' ') {
            out[j++] = main_topic[i];
        }
    }
    out[j] = '\0';
    if (j == 0) {
        strncpy(out, "device", cap - 1);
        out[cap - 1] = '\0';
    }
}

/** HA object_id / unique_id: gas_o_meter2_<slug>_<tail> (wie Discovery-Topic, ohne homeassistant/…/config). */
static void mqtt_ha_build_entity_uid(char* out, size_t cap, const char* slug, const char* object_id_tail) {
    snprintf(out, cap, "%s_%s_%s", MQTT_HA_DEVICE_TOPIC_PREFIX, slug, object_id_tail);
}

/* ~1,9 kB Puffer nicht auf dem Main-Task-Stack (sonst Stack protection fault mit connect_wifi/send_data). */
static char s_ha_slug[MQTT_MAIN_TOPIC_MAX_LEN + 1];
static char s_ha_unique_id[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_data[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_rssi[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_ntp[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_status[MQTT_MAIN_TOPIC_MAX_LEN + 16];
static char s_ha_avail_fragment[MQTT_MAIN_TOPIC_MAX_LEN + 128];
static char s_ha_ident[MQTT_MAIN_TOPIC_MAX_LEN + 48];
static char s_ha_device_tail[96];
static char s_ha_device_json[320];
static char s_ha_topic[160];
static char s_ha_payload[1024];

/**
 * Home Assistant MQTT Discovery (retain, QoS aus mqtt_config.h).
 * enable=true: JSON-Config pro Entity; false: leerer Payload zum Entfernen retained Config.
 */
static void transfer_mqtt_publish_ha_discovery(esp_mqtt_client_handle_t client, const char* main_topic,
                                               const char* hostname, bool enable, const char* fw_version) {
    mqtt_ha_main_topic_slug(s_ha_slug, sizeof(s_ha_slug), main_topic);

    const char* dev_name = (hostname != nullptr && hostname[0] != '\0') ? hostname : MQTT_HA_MODEL;

    build_topic(s_ha_state_data, sizeof(s_ha_state_data), main_topic, MQTT_TOPIC_SUFFIX_DATA);
    build_topic(s_ha_state_rssi, sizeof(s_ha_state_rssi), main_topic, MQTT_TOPIC_SUFFIX_RSSI);
    build_topic(s_ha_state_ntp, sizeof(s_ha_state_ntp), main_topic, MQTT_TOPIC_SUFFIX_NTP_STATUS);
    build_topic(s_ha_state_status, sizeof(s_ha_state_status), main_topic, MQTT_TOPIC_SUFFIX_STATUS);
    snprintf(s_ha_avail_fragment, sizeof(s_ha_avail_fragment),
             ",\"availability_topic\":\"%s\",\"payload_available\":\"%s\",\"payload_not_available\":\"%s\"",
             s_ha_state_status, MQTT_AVAIL_PAYLOAD_ONLINE, MQTT_AVAIL_PAYLOAD_OFFLINE);

    snprintf(s_ha_ident, sizeof(s_ha_ident), "%s_%s", MQTT_HA_DEVICE_TOPIC_PREFIX, s_ha_slug);

    s_ha_device_tail[0] = '\0';
    if (fw_version != nullptr && fw_version[0] != '\0') {
        snprintf(s_ha_device_tail, sizeof(s_ha_device_tail), ",\"sw_version\":\"%s\"", fw_version);
    }

    snprintf(s_ha_device_json, sizeof(s_ha_device_json),
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"%s\",\"model\":\"%s\"%s}",
             s_ha_ident, dev_name, MQTT_HA_MANUFACTURER, MQTT_HA_MODEL, s_ha_device_tail);

    char* const state_topic_data = s_ha_state_data;
    char* const state_topic_rssi = s_ha_state_rssi;
    char* const state_topic_ntp = s_ha_state_ntp;
    char* const device_json = s_ha_device_json;

    struct {
        const char* component;
        const char* object_id_tail;
        const char* json_body;
    } entries[] = {
        {"sensor",
         "gas",
         "{\"name\":\"Gas Counter\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
         "\"value_template\":\"{{ value_json.gas }}\",\"unit_of_measurement\":\"m\\u00b3\","
         "\"device_class\":\"gas\",\"state_class\":\"total_increasing\",\"icon\":\"mdi:meter-gas\",%s%s}"},
        {"sensor",
         "battery",
         "{\"name\":\"Battery\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
         "\"value_template\":\"{{ value_json.battery }}\",\"unit_of_measurement\":\"%%\","
         "\"device_class\":\"battery\",\"state_class\":\"measurement\",\"icon\":\"mdi:battery\",%s%s}"},
        {"sensor",
         "voltage",
         "{\"name\":\"Battery Voltage\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
         "\"value_template\":\"{{ value_json.battery_voltage }}\",\"unit_of_measurement\":\"V\","
         "\"device_class\":\"voltage\",\"state_class\":\"measurement\",\"icon\":\"mdi:flash\",%s%s}"},
        {"binary_sensor",
         "battery_low",
         "{\"name\":\"Battery Low\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
         "\"value_template\":\"{%% if value_json.battery_low %%}ON{%% else %%}OFF{%% endif %%}\","
         "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device_class\":\"battery\","
         "\"icon\":\"mdi:battery-alert\",%s%s}"},
        {"sensor",
         "firmware",
         "{\"name\":\"Firmware\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
         "\"value_template\":\"{{ value_json.firmware_version }}\",\"icon\":\"mdi:information\","
         "\"entity_category\":\"diagnostic\",%s%s}"},
        {"sensor",
         "rssi",
         "{\"name\":\"RSSI\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
         "\"value_template\":\"{{ value | int }}\",\"unit_of_measurement\":\"dBm\","
         "\"device_class\":\"signal_strength\",\"state_class\":\"measurement\",%s%s}"},
        {"sensor",
         "ntp_status",
         "{\"name\":\"Last NTP Sync\",\"unique_id\":\"%s\",\"state_topic\":\"%s\","
         "\"device_class\":\"timestamp\",\"entity_category\":\"diagnostic\",%s%s}"},
    };

    const char* state_topics[] = {
        state_topic_data,
        state_topic_data,
        state_topic_data,
        state_topic_data,
        state_topic_data,
        state_topic_rssi,
        state_topic_ntp,
    };

    const size_t n_entries = sizeof(entries) / sizeof(entries[0]);

    if (!enable) {
        unsigned cleared = 0;
        for (size_t i = 0; i < n_entries; ++i) {
            mqtt_ha_build_entity_uid(s_ha_unique_id, sizeof(s_ha_unique_id), s_ha_slug, entries[i].object_id_tail);
            snprintf(s_ha_topic, sizeof(s_ha_topic), "homeassistant/%s/%s/config", entries[i].component,
                     s_ha_unique_id);
            if (mqtt_publish_with_retry(client, s_ha_topic, "")) {
                ++cleared;
            } else {
                ESP_LOGW(TAG, "MQTT HA Auto-Discovery löschen fehlgeschlagen: %s", s_ha_topic);
            }
        }
        ESP_LOGI(TAG, "MQTT HA Auto-Discovery aus: %u/%u Discovery-Topics mit leerem retain entfernt", cleared,
                 (unsigned)n_entries);
        return;
    }

    for (size_t i = 0; i < n_entries; ++i) {
        mqtt_ha_build_entity_uid(s_ha_unique_id, sizeof(s_ha_unique_id), s_ha_slug, entries[i].object_id_tail);

        snprintf(s_ha_topic, sizeof(s_ha_topic), "homeassistant/%s/%s/config", entries[i].component,
                 s_ha_unique_id);

        snprintf(s_ha_payload, sizeof(s_ha_payload), entries[i].json_body, s_ha_unique_id, state_topics[i],
                 s_ha_avail_fragment, device_json);

        if (mqtt_publish_with_retry(client, s_ha_topic, s_ha_payload)) {
            ESP_LOGI(TAG, "MQTT HA Auto-Discovery gesendet: %s", s_ha_topic);
        } else {
            ESP_LOGW(TAG, "MQTT HA Auto-Discovery fehlgeschlagen: %s", s_ha_topic);
        }
    }
}

bool transfer_mqtt_init(void) {
    mqtt_initialized = true;
    return true;
}

transfer_status_t transfer_mqtt_send_data(const transfer_data_t* data) {
    if (!mqtt_initialized && !transfer_mqtt_init()) {
        return TRANSFER_STATUS_INIT_FAILED;
    }
    if (data == nullptr) {
        return TRANSFER_STATUS_UNKNOWN_ERROR;
    }

    const char* mqtt_host = transfer_mqtt_get_host();
    const char* mqtt_username = transfer_mqtt_get_username();
    const char* mqtt_password = transfer_mqtt_get_password();
    const char* mqtt_main_topic = transfer_mqtt_get_main_topic();
    uint16_t mqtt_port = transfer_mqtt_get_port();

    if (mqtt_host == nullptr || mqtt_host[0] == '\0' || mqtt_main_topic == nullptr || mqtt_main_topic[0] == '\0') {
        ESP_LOGE(TAG, "MQTT-Konfiguration unvollständig");
        return TRANSFER_STATUS_NOT_CONFIGURED;
    }
    if (strcmp(mqtt_host, MQTT_DUMMY_HOST) == 0) {
        ESP_LOGW(TAG, "MQTT Host ist Dummy, Übertragung übersprungen");
        return TRANSFER_STATUS_NOT_CONFIGURED;
    }

    if (!connect_wifi()) {
        ESP_LOGE(TAG, "WiFi-Verbindung für MQTT fehlgeschlagen");
        return TRANSFER_STATUS_CONNECTION_FAILED;
    }

    bool ntp_ok = sync_ntp_time();

    char timestamp_iso[40] = "";
    if (ntp_ok) {
        time_t now = time(NULL);
        struct tm tm_utc;
        if (gmtime_r(&now, &tm_utc) != nullptr) {
            strftime(timestamp_iso, sizeof(timestamp_iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        }
    }

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", mqtt_host, (unsigned int)mqtt_port);

    char status_topic[MQTT_MAIN_TOPIC_MAX_LEN + 16];
    build_topic(status_topic, sizeof(status_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_STATUS);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = uri;
    mqtt_cfg.session.keepalive = 30;
    mqtt_cfg.network.timeout_ms = MQTT_CONNECT_TIMEOUT_MS;
    mqtt_cfg.credentials.username = mqtt_username;
    mqtt_cfg.credentials.authentication.password = mqtt_password;
    mqtt_cfg.session.last_will.topic = status_topic;
    mqtt_cfg.session.last_will.msg = MQTT_AVAIL_PAYLOAD_OFFLINE;
    mqtt_cfg.session.last_will.qos = MQTT_QOS;
    mqtt_cfg.session.last_will.retain = MQTT_RETAIN;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "MQTT Client Init fehlgeschlagen");
        return TRANSFER_STATUS_INIT_FAILED;
    }

    mqtt_connected = false;
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    if (esp_mqtt_client_start(client) != ESP_OK) {
        esp_mqtt_client_destroy(client);
        ESP_LOGE(TAG, "MQTT Client Start fehlgeschlagen");
        return TRANSFER_STATUS_CONNECTION_FAILED;
    }

    uint32_t waited_ms = 0;
    while (!mqtt_connected && waited_ms < MQTT_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }
    if (!mqtt_connected) {
        ESP_LOGE(TAG, "MQTT Connect Timeout");
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        return TRANSFER_STATUS_CONNECTION_FAILED;
    }

    bool ok = mqtt_publish_with_retry(client, status_topic, MQTT_AVAIL_PAYLOAD_ONLINE);
    if (!ok) {
        ESP_LOGW(TAG, "MQTT availability online fehlgeschlagen: %s", status_topic);
    }

    /* HA Discovery vor Telemetrie (retain), damit Entities existieren bevor State ankommt. */
    transfer_mqtt_publish_ha_discovery(client, mqtt_main_topic, transfer_mqtt_get_hostname(),
                                       transfer_mqtt_get_ha_autodiscovery(), data->firmware_version);

    char payload_gas[32];
    char payload_battery[16];
    char payload_battery_voltage[16];
    char payload_battery_low[8];
    char payload_firmware_version[32];
    char payload_timestamp[40];
    char payload_rssi[16];
    char payload_data[256];

    snprintf(payload_gas, sizeof(payload_gas), "%.2f", data->pulse_counter / 100.0f);
    snprintf(payload_battery, sizeof(payload_battery), "%d", (int)data->battery_percent);
    snprintf(payload_battery_voltage, sizeof(payload_battery_voltage), "%.2f", data->battery_voltage);
    snprintf(payload_battery_low, sizeof(payload_battery_low), "%s",
             (data->battery_voltage < BATTERY_VOLTAGE_30) ? "true" : "false");
    snprintf(payload_firmware_version, sizeof(payload_firmware_version), "%s",
             data->firmware_version ? data->firmware_version : "");
    snprintf(payload_timestamp, sizeof(payload_timestamp), "%s", timestamp_iso);
    snprintf(payload_rssi, sizeof(payload_rssi), "%d", (int)ap_info.rssi);

    snprintf(payload_data, sizeof(payload_data),
             "{\"gas\":%s,\"battery\":%s,\"battery_voltage\":%s,\"battery_low\":%s,"
             "\"firmware_version\":\"%s\",\"timestamp\":\"%s\"}",
             payload_gas, payload_battery, payload_battery_voltage, payload_battery_low,
             payload_firmware_version, payload_timestamp);

    char topic[128];

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_DATA);
    ok &= mqtt_publish_with_retry(client, topic, payload_data);

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_RSSI);
    ok &= mqtt_publish_with_retry(client, topic, payload_rssi);

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_NTP_STATUS);
    ok &= mqtt_publish_with_retry(client, topic,
                                  (ntp_ok && timestamp_iso[0] != '\0') ? timestamp_iso : "");

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_GAS);
    ok &= mqtt_publish_with_retry(client, topic, payload_gas);

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_BATTERY);
    ok &= mqtt_publish_with_retry(client, topic, payload_battery);

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_BATTERY_VOLTAGE);
    ok &= mqtt_publish_with_retry(client, topic, payload_battery_voltage);

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_BATTERY_LOW);
    ok &= mqtt_publish_with_retry(client, topic, payload_battery_low);

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_FIRMWARE_VERSION);
    ok &= mqtt_publish_with_retry(client, topic, payload_firmware_version);

    build_topic(topic, sizeof(topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_TIMESTAMP);
    ok &= mqtt_publish_with_retry(client, topic, payload_timestamp);

    if (!mqtt_publish_with_retry(client, status_topic, MQTT_AVAIL_PAYLOAD_OFFLINE)) {
        ESP_LOGW(TAG, "MQTT availability offline fehlgeschlagen: %s", status_topic);
        ok = false;
    }

    /* PUBACK pro Publish bereits abgewartet; kurze Pause vor sauberem Disconnect. */
    vTaskDelay(pdMS_TO_TICKS(MQTT_PUBLISH_RETRY_DELAY_MS));
    esp_mqtt_client_stop(client);
    vTaskDelay(pdMS_TO_TICKS(MQTT_DISCONNECT_TIMEOUT_MS));
    esp_mqtt_client_destroy(client);

    return ok ? TRANSFER_STATUS_OK : TRANSFER_STATUS_SEND_FAILED;
}

void transfer_mqtt_deinit(void) {
    mqtt_initialized = false;
}
