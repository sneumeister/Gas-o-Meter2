#include "transfer_ble.h"
#include "ble_config.h"
#include "hardware.h"
#include "time_sync.h"
#include "version.h"

#ifndef ARDUINO

#include "esp_log.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_store.h"

/* ESP-IDF NimBLE Store-Config (NVS-Persist); nur bei CONFIG_BT_NIMBLE_NVS_PERSIST=y verlinkt */
#if CONFIG_BT_NIMBLE_NVS_PERSIST
extern "C" void ble_store_config_init(void);
#endif

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char* TAG = "transfer_ble";

// ============================================
// Zustandsvariablen
// ============================================
static bool ble_initialized = false;
static bool ble_connected = false;
static bool ble_advertising = false;
static bool ble_sync_ready = false;
static bool ble_start_adv_on_sync = false;
static uint8_t ble_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint16_t ble_conn_handle = 0;

// Attribut-Handles (vom Stack zugewiesen)
static uint16_t ble_custom_data_val_handle;
static uint16_t ble_cts_val_handle;
static uint16_t ble_firmware_val_handle;

// Daten-Puffer
static char ble_json_buf[128];
static char ble_firmware_buf[32];

// Hostname-Zugriff (definiert in main_idf.cpp)
extern const char* transfer_ble_get_hostname(void);

// ============================================
// No-Op Store Callbacks (kein Bonding, aber NimBLE ruft store_write_cb
// ohne NULL-Check auf wenn es neue IRKs generiert → Crash ohne diese)
// ============================================
static int ble_store_noop_read(int obj_type, const union ble_store_key *key,
                                union ble_store_value *value) {
    return BLE_HS_ENOENT;
}

static int ble_store_noop_write(int obj_type, const union ble_store_value *val) {
    return 0;
}

static int ble_store_noop_delete(int obj_type, const union ble_store_key *key) {
    return 0;
}

/* Store status callback (Signatur: int (*)(ble_store_status_event*, void*)) */
static int ble_store_status_cb(struct ble_store_status_event *event, void *arg) {
    (void)event;
    (void)arg;
    return 0;
}

// ============================================
// Statische UUID-Instanzen (BLE_UUID16_DECLARE ist in C++ nicht in Initializers nutzbar)
// ============================================
static ble_uuid16_t uuid_custom_svc   = BLE_UUID16_INIT(BLE_CUSTOM_SERVICE_UUID);
static ble_uuid16_t uuid_custom_data  = BLE_UUID16_INIT(BLE_CUSTOM_DATA_CHAR_UUID);
static ble_uuid16_t uuid_cts_svc      = BLE_UUID16_INIT(BLE_CTS_SERVICE_UUID);
static ble_uuid16_t uuid_cts_char     = BLE_UUID16_INIT(BLE_CTS_CURRENT_TIME_UUID);
static ble_uuid16_t uuid_dis_svc      = BLE_UUID16_INIT(BLE_DIS_SERVICE_UUID);
static ble_uuid16_t uuid_dis_fw_rev   = BLE_UUID16_INIT(BLE_DIS_FIRMWARE_REV_UUID);

// ============================================
// GAP Event Handler
// ============================================

static int ble_gap_event(struct ble_gap_event *event, void *arg);

static void ble_advertise(void) {
    if (!ble_sync_ready) {
        ble_start_adv_on_sync = true;
        ESP_LOGI(TAG, "Stack noch nicht sync → Advertising wird nach Sync gestartet");
        return;
    }

    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    // 16-bit Service UUIDs im Advertising
    static ble_uuid16_t adv_uuids[2];
    adv_uuids[0] = uuid_custom_svc;
    adv_uuids[1] = uuid_cts_svc;
    fields.uuids16 = adv_uuids;
    fields.num_uuids16 = sizeof(adv_uuids) / sizeof(adv_uuids[0]);
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields fehlgeschlagen: %d", rc);
        return;
    }

    /* Scan Response mit Gerätenamen – viele Scanner (z. B. Node-RED) lesen den Namen daraus */
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t *)name;
    rsp_fields.name_len = strlen(name);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_adv_rsp_set_fields fehlgeschlagen: %d (Advertising läuft trotzdem)", rc);
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_ADV_ITVL;
    adv_params.itvl_max = BLE_ADV_ITVL;

    rc = ble_gap_adv_start(ble_own_addr_type, NULL,
                           BLE_ADVERTISING_DURATION_MS,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start fehlgeschlagen: %d", rc);
        return;
    }

    ble_advertising = true;
    ESP_LOGI(TAG, "Advertising gestartet (Intervall: %d ms, Dauer: %d s)", 
             BLE_ADVERTISING_INTERVAL_MS, BLE_ADVERTISING_DURATION_MS / 1000);
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ble_connected = true;
            ble_advertising = false;
            ble_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Central verbunden (handle: %d)", ble_conn_handle);
        } else {
            ESP_LOGW(TAG, "Verbindung fehlgeschlagen (status: %d)", event->connect.status);
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ble_connected = false;
        ble_advertising = false;
        ESP_LOGI(TAG, "Central getrennt (reason: %d)", event->disconnect.reason);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_advertising = false;
        if (!ble_connected) {
            ESP_LOGI(TAG, "Advertising-Timeout (keine Verbindung in %d s)", 
                     BLE_ADVERTISING_DURATION_MS / 1000);
        }
        break;

    default:
        break;
    }
    return 0;
}

// ============================================
// GATT Access Callbacks
// ============================================

// Custom Data Characteristic (0xFFF1) – Read
static int ble_custom_data_access(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, ble_json_buf, strlen(ble_json_buf));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// Firmware Revision Characteristic (0x2A26) – Read
static int ble_firmware_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        int rc = os_mbuf_append(ctxt->om, ble_firmware_buf, strlen(ble_firmware_buf));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// Current Time Characteristic (0x2A2B) – Read + Write
static int ble_cts_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // Aktuelle Zeit als Exact Time 256 zurückgeben
        time_t now = time(NULL);
        struct tm timeinfo;
        gmtime_r(&now, &timeinfo);

        uint8_t cts_buf[BLE_CTS_EXACT_TIME_256_LEN];
        uint16_t year = timeinfo.tm_year + 1900;
        cts_buf[0] = year & 0xFF;
        cts_buf[1] = (year >> 8) & 0xFF;
        cts_buf[2] = timeinfo.tm_mon + 1;
        cts_buf[3] = timeinfo.tm_mday;
        cts_buf[4] = timeinfo.tm_hour;
        cts_buf[5] = timeinfo.tm_min;
        cts_buf[6] = timeinfo.tm_sec;
        cts_buf[7] = (timeinfo.tm_wday == 0) ? 7 : timeinfo.tm_wday;  // 1=Mo..7=So
        cts_buf[8] = 0;  // Fractions256
        cts_buf[9] = 0;  // Adjust Reason

        int rc = os_mbuf_append(ctxt->om, cts_buf, sizeof(cts_buf));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len < BLE_CTS_EXACT_TIME_256_LEN) {
            ESP_LOGW(TAG, "CTS Write: Unvollständig (%d < %d Bytes)", om_len, BLE_CTS_EXACT_TIME_256_LEN);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t cts_buf[BLE_CTS_EXACT_TIME_256_LEN];
        os_mbuf_copydata(ctxt->om, 0, sizeof(cts_buf), cts_buf);
        
        uint16_t year = cts_buf[0] | (cts_buf[1] << 8);
        uint8_t month = cts_buf[2];
        uint8_t day = cts_buf[3];
        uint8_t hour = cts_buf[4];
        uint8_t min = cts_buf[5];
        uint8_t sec = cts_buf[6];

        // Plausibilitätsprüfung
        if (year < BLE_TIME_MIN_YEAR || year > BLE_TIME_MAX_YEAR ||
            month < 1 || month > 12 || day < 1 || day > 31 ||
            hour > 23 || min > 59 || sec > 59) {
            ESP_LOGW(TAG, "CTS Write: Ungültige Zeit (%04d-%02d-%02d %02d:%02d:%02d)", 
                     year, month, day, hour, min, sec);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        struct tm timeinfo = {};
        timeinfo.tm_year = year - 1900;
        timeinfo.tm_mon = month - 1;
        timeinfo.tm_mday = day;
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = min;
        timeinfo.tm_sec = sec;
        // mktime interpretiert als Lokalzeit; setenv TZ=UTC davor
        setenv("TZ", "UTC0", 1);
        tzset();
        time_t unix_time = mktime(&timeinfo);

        if (unix_time < 0) {
            ESP_LOGW(TAG, "CTS Write: timegm fehlgeschlagen");
            return BLE_ATT_ERR_UNLIKELY;
        }

        ESP_LOGI(TAG, "CTS Write: Zeit empfangen %04d-%02d-%02d %02d:%02d:%02d UTC", 
                 year, month, day, hour, min, sec);
        
        time_t now = time(NULL);
        int64_t delta = (int64_t)unix_time - (int64_t)now;
        ESP_LOGI(TAG, "CTS Write: Delta ESP32 ↔ BLE: %lld s", (long long)delta);

        time_sync_set_hard(unix_time, "BLE");
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// ============================================
// GATT Service-Definition (C++-kompatibel)
// ============================================

// Characteristic-Definitionen (Feld-Reihenfolge: uuid, access_cb, arg, descriptors, flags, min_key_size, val_handle, cpfd)
static struct ble_gatt_chr_def custom_chars[] = {
    {
        .uuid       = &uuid_custom_data.u,
        .access_cb  = ble_custom_data_access,
        .arg        = NULL,
        .descriptors = NULL,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &ble_custom_data_val_handle,
        .cpfd       = NULL,
    },
    { 0 }
};

static struct ble_gatt_chr_def cts_chars[] = {
    {
        .uuid       = &uuid_cts_char.u,
        .access_cb  = ble_cts_access,
        .arg        = NULL,
        .descriptors = NULL,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .min_key_size = 0,
        .val_handle = &ble_cts_val_handle,
        .cpfd       = NULL,
    },
    { 0 }
};

static struct ble_gatt_chr_def dis_chars[] = {
    {
        .uuid       = &uuid_dis_fw_rev.u,
        .access_cb  = ble_firmware_access,
        .arg        = NULL,
        .descriptors = NULL,
        .flags      = BLE_GATT_CHR_F_READ,
        .min_key_size = 0,
        .val_handle = &ble_firmware_val_handle,
        .cpfd       = NULL,
    },
    { 0 }
};

static const struct ble_gatt_svc_def ble_gatt_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &uuid_custom_svc.u,
        .includes        = NULL,
        .characteristics = custom_chars,
    },
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &uuid_cts_svc.u,
        .includes        = NULL,
        .characteristics = cts_chars,
    },
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &uuid_dis_svc.u,
        .includes        = NULL,
        .characteristics = dis_chars,
    },
    { 0 }
};

// ============================================
// NimBLE Host Task und Sync-Callback
// ============================================

static void ble_on_sync(void) {
    /* Reihenfolge wie ESP-IDF bleprph: ensure_addr → id_infer_auto → advertise */
    int rc = ble_hs_util_ensure_addr(0);  /* 0 = prefer public */
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr fehlgeschlagen: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto fehlgeschlagen: %d", rc);
        return;
    }

    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(ble_own_addr_type == BLE_OWN_ADDR_RANDOM ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC,
                             addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE Public Address: %02X:%02X:%02X:%02X:%02X:%02X",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }

    ble_sync_ready = true;

    if (ble_start_adv_on_sync) {
        ble_start_adv_on_sync = false;
        ble_advertise();
    }
}

static void ble_on_reset(int reason) {
    ESP_LOGW(TAG, "BLE Host Reset (reason: %d)", reason);
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE Host Task gestartet");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ============================================
// Public API
// ============================================

bool transfer_ble_init(void) {
    if (ble_initialized) {
        ESP_LOGW(TAG, "BLE bereits initialisiert");
        return true;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "BLE-Stack Initialisierung");
    ESP_LOGI(TAG, "========================================");

    int rc;

    // [1] NimBLE Port initialisieren (inkl. Controller)
    rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fehlgeschlagen: %d", rc);
        return false;
    }
    ESP_LOGI(TAG, "→ nimble_port_init OK");

    /* BLE-Sendeleistung für Advertising (Node-RED/Scanner-Erkennbarkeit) */
    esp_power_level_t pwr = (BLE_TX_POWER_DBM >= 20) ? ESP_PWR_LVL_P20 :
                            (BLE_TX_POWER_DBM >= 18) ? ESP_PWR_LVL_P18 :
                            (BLE_TX_POWER_DBM >= 15) ? ESP_PWR_LVL_P15 :
                            (BLE_TX_POWER_DBM >= 12) ? ESP_PWR_LVL_P12 :
                            (BLE_TX_POWER_DBM >= 9)  ? ESP_PWR_LVL_P9  :
                            (BLE_TX_POWER_DBM >= 6)  ? ESP_PWR_LVL_P6  : ESP_PWR_LVL_P3;
    esp_err_t pw = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, pwr);
    if (pw == ESP_OK) {
        ESP_LOGI(TAG, "→ BLE TX Power (Advertising): %d dBm", BLE_TX_POWER_DBM);
    } else {
        ESP_LOGW(TAG, "esp_ble_tx_power_set(ADV) fehlgeschlagen: %s", esp_err_to_name(pw));
    }

    // [2] Host-Konfiguration (VOR Service-Registrierung)
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // Kein Bonding (ad-hoc); SM ist build-mäßig aktiv für GAP-Init
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;

#if CONFIG_BT_NIMBLE_NVS_PERSIST
    // Referenz bleprph: Store-Layer initialisieren (NVS), damit GAP/Identity-Strukturen vollständig da sind
    ble_store_config_init();
#else
    ble_hs_cfg.store_read_cb = ble_store_noop_read;
    ble_hs_cfg.store_write_cb = ble_store_noop_write;
    ble_hs_cfg.store_delete_cb = ble_store_noop_delete;
    ble_hs_cfg.store_status_cb = ble_store_status_cb;
#endif

    // [3] Standard-Services registrieren (Reihenfolge wie Referenz bleprph)
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_LOGI(TAG, "→ GAP/GATT init OK");

    // [4] Custom GATT-Services registrieren
    rc = ble_gatts_count_cfg(ble_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg fehlgeschlagen: %d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(ble_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs fehlgeschlagen: %d", rc);
        return false;
    }
    ESP_LOGI(TAG, "→ GATT Services registriert");

    // [5] GAP Device Name setzen (NACH gap_init, wie im ESP-IDF Beispiel)
    const char *hostname = transfer_ble_get_hostname();
    rc = ble_svc_gap_device_name_set(hostname);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_svc_gap_device_name_set fehlgeschlagen: %d", rc);
    }
    ESP_LOGI(TAG, "→ Device Name: %s", hostname);

    // [6] Host Task starten
    nimble_port_freertos_init(ble_host_task);

    ble_initialized = true;
    ESP_LOGI(TAG, "→ BLE-Stack initialisiert (NimBLE)");
    ESP_LOGI(TAG, "→ Services: CTS (0x1805), DIS (0x180A), Custom (0xFFF0)");
    ESP_LOGI(TAG, "========================================");

    return true;
}

transfer_status_t transfer_ble_send_data(const transfer_data_t* data) {
    if (!ble_initialized) {
        ESP_LOGE(TAG, "BLE nicht initialisiert");
        return TRANSFER_STATUS_INIT_FAILED;
    }

    // Gas-Counter by value kopieren (LP-Core kann jederzeit ändern)
    uint32_t pulse_count = data->pulse_counter;
    uint16_t batt_mv = (uint16_t)(data->battery_voltage * 1000.0f);
    uint8_t batt_pct = (uint8_t)data->battery_percent;
    uint8_t batt_low = (data->battery_voltage < 3.57f) ? 1 : 0;

    // Firmware in Puffer setzen
    if (data->firmware_version) {
        strncpy(ble_firmware_buf, data->firmware_version, sizeof(ble_firmware_buf) - 1);
        ble_firmware_buf[sizeof(ble_firmware_buf) - 1] = '\0';
    }

    // JSON für Custom Characteristic
    snprintf(ble_json_buf, sizeof(ble_json_buf),
             "{\"p\":%lu,\"bv\":%u,\"bp\":%u,\"bl\":%u}",
             (unsigned long)pulse_count, batt_mv, batt_pct, batt_low);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "BLE-Datenübertragung");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "→ JSON: %s", ble_json_buf);
    ESP_LOGI(TAG, "→ Firmware: %s", ble_firmware_buf);
    ESP_LOGI(TAG, "→ Warte auf Central-Verbindung...");

    // Advertising starten (falls nicht bereits)
    if (!ble_connected && !ble_advertising) {
        ble_advertise();
    }

    // Warte auf Verbindung (max. BLE_ADVERTISING_DURATION_MS)
    uint32_t waited = 0;
    while (!ble_connected && waited < BLE_ADVERTISING_DURATION_MS) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }

    if (!ble_connected) {
        ESP_LOGW(TAG, "Keine BLE-Verbindung (Timeout nach %d s)", BLE_ADVERTISING_DURATION_MS / 1000);
        return TRANSFER_STATUS_CONNECTION_FAILED;
    }

    ESP_LOGI(TAG, "→ Central verbunden (nach %lu ms)", (unsigned long)waited);

    // Kurz warten, bis der Central Service Discovery und Subscription auf 0xFFF1 abgeschlossen hat.
    // Ohne Verzögerung geht das Notify oft verloren (Node-RED: Missing → Disconnected, keine Daten).
    if (BLE_NOTIFY_DELAY_MS > 0) {
        ESP_LOGI(TAG, "→ Warte %d ms vor Notify (Central-Subscription)", BLE_NOTIFY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(BLE_NOTIFY_DELAY_MS));
    }

    // Notify senden (falls Central subscribed)
    if (ble_custom_data_val_handle != 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(ble_json_buf, strlen(ble_json_buf));
        if (om) {
            int rc = ble_gatts_notify_custom(ble_conn_handle, ble_custom_data_val_handle, om);
            if (rc != 0) {
                ESP_LOGW(TAG, "Notify fehlgeschlagen: %d (Central liest per Read)", rc);
            }
        }
    }

    // Warte auf Disconnect oder Session-Timeout
    uint32_t session_waited = 0;
    while (ble_connected && session_waited < BLE_SESSION_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(500));
        session_waited += 500;
    }

    if (ble_connected) {
        ESP_LOGI(TAG, "Session-Timeout (%d s), trenne Verbindung", BLE_SESSION_TIMEOUT_MS / 1000);
        ble_gap_terminate(ble_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "BLE-Datenübertragung abgeschlossen");
    ESP_LOGI(TAG, "========================================");

    return TRANSFER_STATUS_OK;
}

void transfer_ble_deinit(void) {
    if (!ble_initialized) {
        return;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "BLE-Stack Deinitialisierung");
    ESP_LOGI(TAG, "========================================");

    if (ble_connected) {
        ble_gap_terminate(ble_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (ble_advertising) {
        ble_gap_adv_stop();
    }

    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
        ESP_LOGI(TAG, "→ NimBLE deinitialisiert");
    } else {
        ESP_LOGW(TAG, "→ nimble_port_stop fehlgeschlagen: %d", rc);
    }

    ble_initialized = false;
    ble_connected = false;
    ble_advertising = false;
    ble_sync_ready = false;
    ble_start_adv_on_sync = false;
    ESP_LOGI(TAG, "========================================");
}

bool transfer_ble_get_status_json(char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size,
             "{\"initialized\":%s,\"connected\":%s,\"advertising\":%s}",
             ble_initialized ? "true" : "false",
             ble_connected ? "true" : "false",
             ble_advertising ? "true" : "false");
    return true;
}

bool transfer_ble_start_pairing(void) {
    if (!ble_initialized) {
        if (!transfer_ble_init()) {
            return false;
        }
    }

    strncpy(ble_firmware_buf, PROJECT_VERSION, sizeof(ble_firmware_buf) - 1);
    ble_firmware_buf[sizeof(ble_firmware_buf) - 1] = '\0';

    // JSON-Puffer mit Dummy-Daten füllen
    snprintf(ble_json_buf, sizeof(ble_json_buf), "{\"p\":0,\"bv\":0,\"bp\":0,\"bl\":0}");

    if (!ble_advertising && !ble_connected) {
        ble_advertise();
        return true;
    }
    return ble_advertising;
}

bool transfer_ble_is_advertising(void) {
    return ble_advertising;
}

#endif // !ARDUINO
