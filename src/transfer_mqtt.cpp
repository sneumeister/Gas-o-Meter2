#include "transfer_mqtt.h"
#include "mqtt_config.h"
#include "time_sync.h"
#include "hardware.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_event.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <ctime>

static const char* TAG = "transfer_mqtt";

extern bool sync_ntp_time(void);

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

/** Slug für HA-Discovery-Topics: `/` und `-` → `_`, Leerzeichen entfernen (HA-Topic ohne Mehrdeutigkeit). */
static void mqtt_ha_main_topic_slug(char* out, size_t cap, const char* main_topic) {
    size_t j = 0;
    if (main_topic == nullptr) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; main_topic[i] != '\0' && j + 1 < cap; ++i) {
        if (main_topic[i] == '/' || main_topic[i] == '-') {
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

/* ~1,9 kB Puffer nicht auf dem Main-Task-Stack (sonst Stack protection fault mit wifi_connect_sta/send_data). */
static char s_ha_slug[MQTT_MAIN_TOPIC_MAX_LEN + 1];
static char s_ha_unique_id[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_data[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_rssi[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_ntp[MQTT_MAIN_TOPIC_MAX_LEN + 32];
static char s_ha_state_status[MQTT_MAIN_TOPIC_MAX_LEN + 16];
static char s_ha_expire_fragment[32];
static char s_ha_avail_fragment[MQTT_MAIN_TOPIC_MAX_LEN + 128];
static char s_ha_ident[MQTT_MAIN_TOPIC_MAX_LEN + 48];
static char s_ha_device_tail[96];
static char s_ha_device_json[320];
static char s_ha_topic[160];
static char s_ha_payload[1536];

/* Send-Pfad: nicht auf Main-Task-Stack (zusammen mit wifi_connect_sta). */
static char s_mqtt_uri[128];
static char s_mqtt_status_topic[MQTT_MAIN_TOPIC_MAX_LEN + 16];
static char s_mqtt_topic[128];
static char s_mqtt_timestamp_iso[40];
static char s_payload_gas[32];
static char s_payload_battery[16];
static char s_payload_battery_voltage[16];
static char s_payload_battery_low[8];
static char s_payload_firmware_version[32];
static char s_payload_rssi[16];
static char s_payload_ntp_epoch[24];
static char s_payload_data[256];
static esp_mqtt_client_config_t s_mqtt_cfg;

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
    snprintf(s_ha_expire_fragment, sizeof(s_ha_expire_fragment), "\"expire_after\":%u,",
             (unsigned)MQTT_HA_EXPIRE_AFTER_SEC);
    /* Kein führendes Komma: json_body endet bereits mit "," vor %s (sonst ",," → ungültiges JSON). */
    snprintf(s_ha_avail_fragment, sizeof(s_ha_avail_fragment),
             "\"availability_topic\":\"%s\",\"payload_available\":\"%s\",\"payload_not_available\":\"%s\"",
             s_ha_state_status, MQTT_AVAIL_PAYLOAD_ONLINE, MQTT_AVAIL_PAYLOAD_OFFLINE);

    snprintf(s_ha_ident, sizeof(s_ha_ident), "%s_%s", MQTT_HA_DEVICE_TOPIC_PREFIX, s_ha_slug);

    s_ha_device_tail[0] = '\0';
    if (fw_version != nullptr && fw_version[0] != '\0') {
        snprintf(s_ha_device_tail, sizeof(s_ha_device_tail), ",\"sw_version\":\"%s\"", fw_version);
    }

    /* Führendes Komma: wird direkt nach s_ha_avail_fragment eingefügt (…"offline",<hier>device…). */
    snprintf(s_ha_device_json, sizeof(s_ha_device_json),
             ",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\",\"manufacturer\":\"%s\",\"model\":\"%s\"%s}",
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
         "{\"name\":\"Gas Counter\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",%s"
         "\"value_template\":\"{{ value_json.gas }}\",\"unit_of_measurement\":\"m\\u00b3\","
         "\"device_class\":\"gas\",\"state_class\":\"total_increasing\",\"icon\":\"mdi:meter-gas\",%s%s}"},
        {"sensor",
         "battery",
         "{\"name\":\"Battery\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",%s"
         "\"value_template\":\"{{ value_json.battery }}\",\"unit_of_measurement\":\"%%\","
         "\"device_class\":\"battery\",\"state_class\":\"measurement\",\"icon\":\"mdi:battery\",%s%s}"},
        {"sensor",
         "voltage",
         "{\"name\":\"Battery Voltage\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",%s"
         "\"value_template\":\"{{ value_json.battery_voltage }}\",\"unit_of_measurement\":\"V\","
         "\"device_class\":\"voltage\",\"state_class\":\"measurement\",\"icon\":\"mdi:flash\",%s%s}"},
        {"binary_sensor",
         "battery_low",
         "{\"name\":\"Battery Low\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",%s"
         "\"value_template\":\"{%% if value_json.battery_low %%}ON{%% else %%}OFF{%% endif %%}\","
         "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",\"device_class\":\"battery\","
         "\"icon\":\"mdi:battery-alert\",%s%s}"},
        {"sensor",
         "firmware",
         "{\"name\":\"Firmware\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",%s"
         "\"value_template\":\"{{ value_json.firmware_version }}\",\"icon\":\"mdi:information\","
         "\"entity_category\":\"diagnostic\",%s%s}"},
        {"sensor",
         "rssi",
         "{\"name\":\"RSSI\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",%s"
         "\"value_template\":\"{{ value | int }}\",\"unit_of_measurement\":\"dBm\","
         "\"device_class\":\"signal_strength\",\"state_class\":\"measurement\","
         "\"entity_category\":\"diagnostic\",%s%s}"},
        {"sensor",
         "ntp_status",
         "{\"name\":\"Last NTP Sync\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",%s"
         "\"device_class\":\"timestamp\",\"value_template\":\"{{ as_datetime(value) }}\","
         "\"entity_category\":\"diagnostic\",%s%s}"},
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

        int payload_len = snprintf(s_ha_payload, sizeof(s_ha_payload), entries[i].json_body, s_ha_unique_id,
                                   state_topics[i], s_ha_expire_fragment, s_ha_avail_fragment, device_json);
        if (payload_len < 0 || (size_t)payload_len >= sizeof(s_ha_payload)) {
            ESP_LOGW(TAG, "MQTT HA Auto-Discovery Payload zu lang (%d/%u): %s", payload_len,
                     (unsigned)sizeof(s_ha_payload), s_ha_topic);
            continue;
        }

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

    if (!wifi_connect_sta()) {
        ESP_LOGE(TAG, "WiFi-Verbindung für MQTT fehlgeschlagen");
        return TRANSFER_STATUS_CONNECTION_FAILED;
    }

    bool ntp_ok = sync_ntp_time();

    s_mqtt_timestamp_iso[0] = '\0';
    time_t ntp_sync_epoch = 0;
    if (ntp_ok) {
        ntp_sync_epoch = time_sync_last_epoch;
        struct tm tm_utc;
        if (ntp_sync_epoch > 0 && gmtime_r(&ntp_sync_epoch, &tm_utc) != nullptr) {
            strftime(s_mqtt_timestamp_iso, sizeof(s_mqtt_timestamp_iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        } else {
            ntp_sync_epoch = 0;
        }
    }

    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtt://%s:%u", mqtt_host, (unsigned int)mqtt_port);

    build_topic(s_mqtt_status_topic, sizeof(s_mqtt_status_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_STATUS);

    memset(&s_mqtt_cfg, 0, sizeof(s_mqtt_cfg));
    s_mqtt_cfg.broker.address.uri = s_mqtt_uri;
    s_mqtt_cfg.session.keepalive = 30;
    s_mqtt_cfg.network.timeout_ms = MQTT_CONNECT_TIMEOUT_MS;
    s_mqtt_cfg.credentials.username = mqtt_username;
    s_mqtt_cfg.credentials.authentication.password = mqtt_password;
    s_mqtt_cfg.session.last_will.topic = s_mqtt_status_topic;
    s_mqtt_cfg.session.last_will.msg = MQTT_AVAIL_PAYLOAD_OFFLINE;
    s_mqtt_cfg.session.last_will.qos = MQTT_QOS;
    s_mqtt_cfg.session.last_will.retain = MQTT_RETAIN;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&s_mqtt_cfg);
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

    bool ok = mqtt_publish_with_retry(client, s_mqtt_status_topic, MQTT_AVAIL_PAYLOAD_ONLINE);
    if (!ok) {
        ESP_LOGW(TAG, "MQTT availability online fehlgeschlagen: %s", s_mqtt_status_topic);
    }

    /* HA Discovery vor Telemetrie (retain), damit Entities existieren bevor State ankommt. */
    transfer_mqtt_publish_ha_discovery(client, mqtt_main_topic, transfer_mqtt_get_hostname(),
                                       transfer_mqtt_get_ha_autodiscovery(), data->firmware_version);

    /* MQTT: ntp_status = Unix-Epoch letzter NTP-Sync; timestamp = dieselbe Zeit als ISO-UTC.
     * Nie Sync: ntp_status = MQTT_NTP_STATUS_NEVER_SYNCED ("-1"), kein /timestamp-Publish.
     * Sync früher ok, dieser Wake fehlgeschlagen: beide Topics nicht publishen (Retain bleibt). */
    const bool ntp_timestamp_valid =
        ntp_ok && ntp_sync_epoch > 0 && s_mqtt_timestamp_iso[0] != '\0' &&
        (strcmp(time_sync_last_source, "NTP") == 0);

    snprintf(s_payload_gas, sizeof(s_payload_gas), "%.2f", data->pulse_counter / 100.0f);
    snprintf(s_payload_battery, sizeof(s_payload_battery), "%d", (int)data->battery_percent);
    snprintf(s_payload_battery_voltage, sizeof(s_payload_battery_voltage), "%.2f", data->battery_voltage);
    snprintf(s_payload_battery_low, sizeof(s_payload_battery_low), "%s",
             (data->battery_voltage < BATTERY_VOLTAGE_30) ? "true" : "false");
    snprintf(s_payload_firmware_version, sizeof(s_payload_firmware_version), "%s",
             data->firmware_version ? data->firmware_version : "");
    snprintf(s_payload_rssi, sizeof(s_payload_rssi), "%d", (int)wifi_get_ap_info()->rssi);

    if (ntp_timestamp_valid) {
        snprintf(s_payload_data, sizeof(s_payload_data),
                 "{\"gas\":%s,\"battery\":%s,\"battery_voltage\":%s,\"battery_low\":%s,"
                 "\"firmware_version\":\"%s\",\"timestamp\":\"%s\"}",
                 s_payload_gas, s_payload_battery, s_payload_battery_voltage, s_payload_battery_low,
                 s_payload_firmware_version, s_mqtt_timestamp_iso);
    } else {
        snprintf(s_payload_data, sizeof(s_payload_data),
                 "{\"gas\":%s,\"battery\":%s,\"battery_voltage\":%s,\"battery_low\":%s,"
                 "\"firmware_version\":\"%s\"}",
                 s_payload_gas, s_payload_battery, s_payload_battery_voltage, s_payload_battery_low,
                 s_payload_firmware_version);
    }

    build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_DATA);
    ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_data);

    build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_RSSI);
    ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_rssi);

    if (ntp_timestamp_valid) {
        snprintf(s_payload_ntp_epoch, sizeof(s_payload_ntp_epoch), "%lld", (long long)ntp_sync_epoch);
        build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_NTP_STATUS);
        ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_ntp_epoch);
        build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_TIMESTAMP);
        ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_mqtt_timestamp_iso);
    } else if (time_sync_last_epoch == 0) {
        /* Nie synchronisiert: Sentinel -1 (kein ISO auf /timestamp — Topic fehlt/leer in HA). */
        build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_NTP_STATUS);
        ok &= mqtt_publish_with_retry(client, s_mqtt_topic, MQTT_NTP_STATUS_NEVER_SYNCED);
    }

    build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_GAS);
    ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_gas);

    build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_BATTERY);
    ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_battery);

    build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_BATTERY_VOLTAGE);
    ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_battery_voltage);

    build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_BATTERY_LOW);
    ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_battery_low);

    build_topic(s_mqtt_topic, sizeof(s_mqtt_topic), mqtt_main_topic, MQTT_TOPIC_SUFFIX_FIRMWARE_VERSION);
    ok &= mqtt_publish_with_retry(client, s_mqtt_topic, s_payload_firmware_version);

    /* Nach erfolgreichem Zyklus online retained lassen (Deep Sleep). offline nur bei Fehler; LWT bei Abbruch. */
    if (!ok) {
        if (!mqtt_publish_with_retry(client, s_mqtt_status_topic, MQTT_AVAIL_PAYLOAD_OFFLINE)) {
            ESP_LOGW(TAG, "MQTT availability offline fehlgeschlagen: %s", s_mqtt_status_topic);
        }
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

#define MQTT_TEST_SYS_VERSION_TOPIC "$SYS/broker/version"
#define MQTT_TEST_SYS_WAIT_MS 2000

struct mqtt_test_ctx {
    volatile bool connected;
    volatile bool version_received;
    char broker_version[64];
};

static void mqtt_test_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id,
                                    void* event_data) {
    (void)base;
    mqtt_test_ctx* ctx = static_cast<mqtt_test_ctx*>(handler_args);
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ctx->connected = true;
            if (event->client != nullptr) {
                esp_mqtt_client_subscribe(event->client, MQTT_TEST_SYS_VERSION_TOPIC, 0);
            }
            break;
        case MQTT_EVENT_DATA: {
            if (event->topic_len <= 0 || event->data_len <= 0) {
                break;
            }
            const size_t topic_len = (size_t)event->topic_len;
            const size_t ver_topic_len = strlen(MQTT_TEST_SYS_VERSION_TOPIC);
            if (topic_len != ver_topic_len) {
                break;
            }
            if (strncmp(event->topic, MQTT_TEST_SYS_VERSION_TOPIC, topic_len) != 0) {
                break;
            }
            size_t copy_len = (size_t)event->data_len;
            if (copy_len >= sizeof(ctx->broker_version)) {
                copy_len = sizeof(ctx->broker_version) - 1;
            }
            memcpy(ctx->broker_version, event->data, copy_len);
            ctx->broker_version[copy_len] = '\0';
            ctx->version_received = true;
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ctx->connected = false;
            break;
        default:
            break;
    }
}

static void mqtt_test_write_error(char* json_out, size_t json_out_len, const char* message) {
    snprintf(json_out, json_out_len, "{\"status\":\"error\",\"message\":\"%s\"}", message);
}

bool transfer_mqtt_test_connection(const char* host, uint16_t port, const char* username,
                                     const char* password, char* json_out, size_t json_out_len) {
    if (json_out == nullptr || json_out_len < 32) {
        return false;
    }
    json_out[0] = '\0';

    if (!wifi_is_connected()) {
        mqtt_test_write_error(json_out, json_out_len,
                              "MQTT-Test nur im Heim-WLAN (STA), nicht im Konfigurations-Hotspot");
        return false;
    }
    if (host == nullptr || host[0] == '\0') {
        mqtt_test_write_error(json_out, json_out_len, "MQTT Host fehlt");
        return false;
    }
    if (strcmp(host, MQTT_DUMMY_HOST) == 0) {
        mqtt_test_write_error(json_out, json_out_len, "Dummy-Host ist kein gueltiger MQTT-Server");
        return false;
    }
    if (port == 0) {
        mqtt_test_write_error(json_out, json_out_len, "MQTT Port ungueltig");
        return false;
    }

    char test_uri[128];
    snprintf(test_uri, sizeof(test_uri), "mqtt://%s:%u", host, (unsigned int)port);

    mqtt_test_ctx ctx = {};
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = test_uri;
    cfg.session.keepalive = 30;
    cfg.network.timeout_ms = MQTT_CONNECT_TIMEOUT_MS;
    if (username != nullptr && username[0] != '\0') {
        cfg.credentials.username = username;
        cfg.credentials.authentication.password = (password != nullptr) ? password : "";
    }

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (client == nullptr) {
        mqtt_test_write_error(json_out, json_out_len, "MQTT Client Init fehlgeschlagen");
        return false;
    }

    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_test_event_handler, &ctx);
    if (esp_mqtt_client_start(client) != ESP_OK) {
        esp_mqtt_client_destroy(client);
        mqtt_test_write_error(json_out, json_out_len, "MQTT Client Start fehlgeschlagen");
        return false;
    }

    uint32_t waited_ms = 0;
    const uint32_t poll_ms = 50;
    while (!ctx.connected && waited_ms < MQTT_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }

    if (!ctx.connected) {
        esp_mqtt_client_stop(client);
        vTaskDelay(pdMS_TO_TICKS(MQTT_DISCONNECT_TIMEOUT_MS));
        esp_mqtt_client_destroy(client);
        mqtt_test_write_error(json_out, json_out_len, "MQTT-Server connect: Timeout");
        return false;
    }

    waited_ms = 0;
    while (!ctx.version_received && waited_ms < MQTT_TEST_SYS_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        waited_ms += poll_ms;
    }

    esp_mqtt_client_stop(client);
    vTaskDelay(pdMS_TO_TICKS(MQTT_DISCONNECT_TIMEOUT_MS));
    esp_mqtt_client_destroy(client);

    if (ctx.version_received && ctx.broker_version[0] != '\0') {
        snprintf(json_out, json_out_len,
                 "{\"status\":\"ok\",\"message\":\"MQTT-Server connect: OK\","
                 "\"broker_version\":\"%s\"}",
                 ctx.broker_version);
    } else {
        snprintf(json_out, json_out_len,
                 "{\"status\":\"ok\",\"message\":\"MQTT-Server connect: OK\"}");
    }
    return true;
}
