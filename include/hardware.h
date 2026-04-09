#ifndef HARDWARE_H
#define HARDWARE_H

// ============================================
// Framework-Erkennung und Header-Includes
// ============================================

#ifdef ARDUINO
    // Arduino Framework
    #include <Arduino.h>
#else
    // ESP-IDF Framework
    #include "driver/gpio.h"
    #include "freertos/FreeRTOS.h"
    #include "freertos/task.h"
    #include "esp_log.h"
#endif

#include "hal/adc_types.h"


// WiFi Log-Level Konfiguration (nur ESP-IDF)
#ifndef ARDUINO
    // WiFi-Debug-Nachrichten reduzieren (nur WARNINGS und ERRORS)
    // WICHTIG: esp_log.h muss vor diesem Makro eingebunden sein
    #define SET_WIFI_LOG_LEVEL() esp_log_level_set("wifi", ESP_LOG_WARN)
#else
    // Arduino: NOP (keine ESP-IDF Log-Funktionen)
    #define SET_WIFI_LOG_LEVEL()  ((void)0)
#endif

// ============================================
// Arduino HIGH/LOW Definitionen (für beide Frameworks)
// ============================================
// HIGH/LOW werden von Arduino.h definiert, für ESP-IDF müssen wir sie selbst definieren
#ifndef HIGH
    #define HIGH    1
#endif
#ifndef LOW
    #define LOW     0
#endif

// ============================================
// Hardware Pin-Definitionen
// ============================================

// GPIO-Pins für Taster und Sensoren
#define BUTTON_A_GPIO      1   // Taster A (Software Pull-Up, active-low gegen Masse)
#define REED_GPIO          2   // Reed-Kontakt (externer Pull-Up, active-low)
#define BUTTON_B_GPIO      21  // Taster B (Software Pull-Up, active-low gegen Masse)

// REED-Kontakt Konfiguration
#define REED_MIN_PULSE_DURATION_US  (3 * 1000000ULL)  // Minimale Puls-Länge: 3 Sekunden (in Mikrosekunden)

// Interne LED (XIAO ESP32C6: GPIO15)
// HINWEIS: Bei einigen Boards ist die LED active-low (ON = LOW, OFF = HIGH)
// Falls die LED nicht leuchtet, versuchen Sie LOW statt HIGH
#define LED_BUILTIN_GPIO   15  // Interne LED

// ============================================
// Framework-spezifische Definitionen
// ============================================

#ifdef ARDUINO
    // Arduino Pin-Modi
    // INPUT_PULLUP und INPUT_PULLDOWN werden von Arduino.h definiert
    // (werden hier nicht neu definiert, da sie bereits in Arduino.h vorhanden sind)
    
    #define BUTTON_A_GPIO_MODE  INPUT_PULLUP
    #define REED_GPIO_MODE      INPUT          // Kein Pull-Up/Pull-Down (externer Pull-Up)
    #define BUTTON_B_GPIO_MODE  INPUT_PULLUP
#else
    // ESP-IDF Pin-Modi
    // Explizite Casts zu int, um deprecated-Warnungen zu vermeiden (ESP-IDF 5.x)
    #define BUTTON_A_GPIO_MODE  ((int)(GPIO_MODE_INPUT) | (int)(GPIO_PULLUP_ONLY))
    #define REED_GPIO_MODE      GPIO_MODE_INPUT  // Kein Pull-Up/Pull-Down (externer Pull-Up)
    #define BUTTON_B_GPIO_MODE  ((int)(GPIO_MODE_INPUT) | (int)(GPIO_PULLUP_ONLY))
    
    // Arduino GPIO-Modi für Kompatibilität (ESP-IDF)
    // Explizite Casts zu int, um deprecated-Warnungen zu vermeiden (ESP-IDF 5.x)
    #define INPUT_PULLUP      ((int)(GPIO_MODE_INPUT) | (int)(GPIO_PULLUP_ONLY))
    #define INPUT_PULLDOWN    ((int)(GPIO_MODE_INPUT) | (int)(GPIO_PULLDOWN_ONLY))
#endif

// Logik-Level (identisch für beide Frameworks)
#define LED_OFF            HIGH  // LED AUS (bei HIGH!)
#define LED_ON             LOW   // LED AN (bei LOW!)

// Antennenumschaltung Logik-Level (identisch für beide Frameworks)
#define ANTENNA_RF_SWITCH_ENABLE LOW  // RF-Switch aktivieren
#define ANTENNA_INTERNAL        LOW   // Logik-Level für interne Antenne (GPIO14)
#define ANTENNA_EXTERNAL        HIGH  // Logik-Level für externe Antenne (GPIO14)

// ADC-Pins für Akku-Monitoring
#define BATTERY_ADC_PIN    0   // Akku-Monitoring ADC Pin (GPIO0)
#define BATTERY_ADC_UNIT   ADC_UNIT_1  // ADC Unit 1 (GPIO0 gehört zu ADC1)
#define BATTERY_ADC_CHANNEL ADC_CHANNEL_0  // ADC1 Channel 0

// Antennenumschaltung (ESP32C6: RF-Switch + Antennenauswahl)
#define ANTENNA_RF_SWITCH_GPIO  3   // GPIO3: RF-Switch aktivieren (LOW = aktiviert)
#define ANTENNA_SELECT_GPIO     14  // GPIO14: Antennenauswahl (LOW = intern, HIGH = extern)

// ============================================
// ADC-Konfiguration
// ============================================

// Median-Filter Konfiguration
#define ADC_SAMPLE_COUNT   5   // Anzahl Messungen für Median-Filter

// Spannungsteiler-Konfiguration
#define VOLTAGE_DIVIDER_RATIO  2.0   // Spannungsteiler-Verhältnis (2:1 = 2.0)

// ADC-Korrekturfaktor (Skalierung)
// 1.0 = keine Korrektur, typischer Kalibrierwert: Multimeter-Spannung / angezeigte Spannung
#define ADC_VOLTAGE_MULTIPLIER  1.00f

// ADC-Konfiguration für ADC_ATTEN_DB_12
// Gemessen: 3.3V Eingang → 1.65V am ADC → ADC-Wert 1595
// Berechnung: 1.65V / (1595/4095) = 4.24V maximale ADC-Spannung
#define ADC_MAX_ADC_VOLTAGE    4.24f  // Maximale Spannung am ADC-Eingang bei ADC_ATTEN_DB_12 (V) - gemessen
#define ADC_RESOLUTION         4095   // 12-bit ADC Auflösung (2^12 - 1)

// Akku-Spannungsbereich (gemessene Werte aus Tabelle)
// Diese Werte werden gegen die mit ADC_TO_VOLTAGE() berechneten Spannungen geprüft
#define BATTERY_VOLTAGE_FULL      4.02f   // ≥4.02V = 100% = "Voll"
#define BATTERY_VOLTAGE_80        3.92f   // 3.92V = 80% = "Gut"
#define BATTERY_VOLTAGE_50        3.72f   // 3.72V = 50% = "Mittel"
#define BATTERY_VOLTAGE_30        3.57f   // 3.57V = 30% (Schwelle für Ring-Speicher-Schreibung)
#define BATTERY_VOLTAGE_20        3.42f   // 3.42V = 20% = "Niedrig"
#define BATTERY_VOLTAGE_PROTECTION 3.15f   // ≤3.15V = 0% = "Schutz!" (Betrieb einstellen)

// Min/Max für Kompatibilität
#define BATTERY_MIN_VOLTAGE   BATTERY_VOLTAGE_PROTECTION  // Minimale Akku-Spannung (V) = 0%
#define BATTERY_MAX_VOLTAGE   BATTERY_VOLTAGE_FULL        // Maximale Akku-Spannung (V) = 100%

// Stromversorgungs-Erkennung
// Spannung < USB_DETECTION_THRESHOLD: USB-Stromversorgung (ESP32C6 würde sonst nicht starten)
// Spannung >= USB_DETECTION_THRESHOLD aber < BATTERY_VOLTAGE_20: Akku zu niedrig → Deep-Sleep
#define USB_DETECTION_THRESHOLD   2.0f    // Schwellwert für USB-Erkennung (V) - darunter: USB angeschlossen

// NTP-Konfiguration
#define DEFAULT_NTP_SERVER    "pool.ntp.org"  // Default NTP-Server (gut für Europa)
#define NTP_TIMEOUT_MS        5000            // Timeout für NTP-Synchronisation (ms)

// WiFi TX Power Konfiguration (Sendeleistung)
// ESP32-C6: Gültiger Bereich: 8-80 (in 0.25 dBm Einheiten)
//   Minimum: 8  =  2 dBm
//   Maximum: 80 = 20 dBm
//   Default: 80 = 20 dBm (Maximum für beste Reichweite)
// Höhere Werte = größere Reichweite, aber höherer Stromverbrauch
// WICHTIG: Wert außerhalb [8, 80] führt zu ESP_ERR_INVALID_ARG
// WICHTIG: Dieser Wert wird zur Compile-Zeit in hardware.h definiert, daher wird keine Laufzeit-Prüfung durchgeführt
#define WIFI_TX_POWER_DEFAULT       80      // Default WiFi TX Power: 20 dBm (80 * 0.25 dBm = 20 dBm)

// Hostname/BLE-Device-Name: Gemeinsamer Wert für WiFi und BLE
// BLE-Limit: 26 Zeichen (esp_ble_gap_set_device_name, IDFGH-5588)
#define HOSTNAME_MAX_LEN  26

// Deep-Sleep Konfiguration
#define WIFI_WAIT_FOR_SLEEP   5               // Minuten ohne Web-Server-Zugriff → Deep-Sleep

// Access Point Konfiguration
#define AP_IP_ADDRESS_1       10               // AP IP-Adresse: 10.0.0.1
#define AP_IP_ADDRESS_2       0
#define AP_IP_ADDRESS_3       0
#define AP_IP_ADDRESS_4       1

// LP-Core Konfiguration
// LP-Core Intervall = REED_MIN_PULSE_DURATION - 0.5 Sekunden (um sicherzustellen, dass Pulse erkannt werden)
#define LP_CORE_INTERVAL_US   (REED_MIN_PULSE_DURATION_US - (500 * 1000ULL))  // 0.5 Sekunden = 500ms = 500000µs
#define LP_CORE_WATCHDOG_MS   ((LP_CORE_INTERVAL_US / 1000) + 500)  // Watchdog-Timeout: LP_CORE_INTERVAL + 500ms

// ============================================
// Wake-up Konfiguration
// ============================================

// Standard Wake-up Interval (wird von config.json überschrieben) zur ADC-Abfrage
// Cron-ähnliche Logik: Wake-up erfolgt zu Minuten der Stunde, die durch diesen Wert teilbar sind
// Beispiel: DEFAULT_WAKEUP_INTERVAL_MIN = 10 → Wake-up bei 0, 10, 20, 30, 40, 50 Minuten jeder Stunde
// Muss Teiler von 60 sein: 1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60
#define DEFAULT_WAKEUP_INTERVAL_MIN  10  // Minuten
#define DEFAULT_WAKEUP_INTERVAL_US   (DEFAULT_WAKEUP_INTERVAL_MIN * 60 * 1000000ULL)

// Puffer-Zeit vor Wake-up-Berechnung (verhindert sofortiges Wake-up wenn kurz vor dem Zeitpunkt)
// Wird zur aktuellen Zeit addiert, bevor nächster Wake-up-Zeitpunkt berechnet wird
#define WAKEUP_BUFFER_SEC  30  // Sekunden

// Standard Transfer Interval (wird von config.json überschrieben) : x * DEFAULT_WAKEUP_INTERVAL_MIN
// Cron-ähnliche Logik:
// Beispiel: DEFAULT_WAKEUP_INTERVAL_MIN = 10 und DEFAULT_TRANSFER_INTERVAL_X = 2 → Wake-up bei 0, 20, 40, 60 Minuten jeder Stunde
#define DEFAULT_TRANSFER_INTERVAL_X  2  // Multiplikator für Transfer-Intervall
#define DEFAULT_TRANSFER_INTERVAL_US   (DEFAULT_TRANSFER_INTERVAL_X * DEFAULT_WAKEUP_INTERVAL_MIN * 60 * 1000000ULL)

// ============================================
// Pulse-Zähler Konfiguration
// ============================================

// Pulse-Zähler verwendet uint32_t (32-bit unsigned int)
// Vom Gas-zähler max 9999999 (inklusive der 2 Nachkommastellen 99999.99)
// Maximaler Wert: UINT32_MAX = 4294967295 (automatisch durch Datentyp)

// Pulse-Counter Divisor (für Formatierung: Integer → Dezimal)
// Wird verwendet für: Web-Darstellung (pulse_counter_left/right), Log-Ausgabe, ZigBee-Übertragung
// Beispiel: pulse_counter = 12345 → 123.45 m³ (12345 / 100 = 123, 12345 % 100 = 45)
#define PULSE_COUNTER_DIVISOR  100  // Divisor für 2 Nachkommastellen (m³)

// Ringspeicher-Konfiguration
#define RING_BUFFER_SIZE  12000  // Anzahl Einträge im Ringspeicher (10 Jahre @ 1200 Pulse/Jahr)

// ============================================
// RTC Memory Konfiguration
// ============================================

// RTC Memory Adressen
#define ULP_RTC_MEM_BASE   0x50000000  // RTC Fast Memory Start-Adresse
#define ULP_DATA_OFFSET    0x100       // Offset für Daten im RTC Memory

// Magic Number für RTC Memory Validierung
#define ULP_DATA_MAGIC     0x475A4D4F  // "GZMO" (Gas-Zähler-Meter-Original)

// ============================================
// NVS Konfiguration
// ============================================

// NVS Partition und Namespace
#define NVS_PARTITION_PULSE  "pulse_nv"   // Partition-Name aus partitions.csv
#define NVS_NAMESPACE_PULSE  "pulse_ring"

// NVS Keys
// NVS_KEY_INDEX entfernt - ring_idx wird jetzt im RTC-RAM gehalten (vermeidet Wear-Leveling-Hotspot)
// NVS_KEY_COUNTER entfernt - wird nicht verwendet
#define NVS_KEY_VERSION      "version"  // Versionsnummer für Initialisierungs-Check
#define NVS_KEY_PREFIX       "p_"
#define MAX_KEY_LENGTH       8   // Max: "p_11999" = 7 Zeichen + 1 Null-Terminator = 8

// Ring-Speicher Versionsnummer
// Wird automatisch aus BUILD_TIMESTAMP (version.h) übernommen
#include "version.h"
// RING_BUFFER_VERSION ist bereits in version.h definiert

// ============================================
// Helper Makros
// ============================================

// ADC-Wert zu Eingangsspannung konvertieren
// Formel: (ADC-Wert / 4095) * ADC_MAX_ADC_VOLTAGE * VOLTAGE_DIVIDER_RATIO
// Beispiel: ADC=1595 → (1595/4095) * 4.24V * 2.0 = 3.3V Eingangsspannung
#define ADC_TO_VOLTAGE(adc_value) \
    ((float)(adc_value) / ADC_RESOLUTION * ADC_MAX_ADC_VOLTAGE * VOLTAGE_DIVIDER_RATIO)

// Spannung zu Akku-Prozent konvertieren (diskrete Werte basierend auf Tabelle)
// Basierend auf den definierten Spannungswerten aus der Tabelle
inline uint8_t VOLTAGE_TO_PERCENT(float voltage) {
    if (voltage >= BATTERY_VOLTAGE_FULL) {
        return 100;  // "Voll"
    } else if (voltage >= BATTERY_VOLTAGE_80) {
        // 100% - 80%: 4.02V - 3.92V = 0.10V
        return 80 + (uint8_t)(((voltage - BATTERY_VOLTAGE_80) / (BATTERY_VOLTAGE_FULL - BATTERY_VOLTAGE_80)) * 20.0f);
    } else if (voltage >= BATTERY_VOLTAGE_50) {
        // 80% - 50%: 3.92V - 3.72V = 0.20V
        return 50 + (uint8_t)(((voltage - BATTERY_VOLTAGE_50) / (BATTERY_VOLTAGE_80 - BATTERY_VOLTAGE_50)) * 30.0f);
    } else if (voltage >= BATTERY_VOLTAGE_20) {
        // 50% - 20%: 3.72V - 3.42V = 0.30V
        return 20 + (uint8_t)(((voltage - BATTERY_VOLTAGE_20) / (BATTERY_VOLTAGE_50 - BATTERY_VOLTAGE_20)) * 30.0f);
    } else if (voltage >= BATTERY_VOLTAGE_PROTECTION) {
        // 20% - 0%: 3.42V - 3.15V = 0.27V
        return 0 + (uint8_t)(((voltage - BATTERY_VOLTAGE_PROTECTION) / (BATTERY_VOLTAGE_20 - BATTERY_VOLTAGE_PROTECTION)) * 20.0f);
    } else {
        return 0;     // "Schutz!" - unter 3.15V
    }
}

// ADC-Wert direkt zu Akku-Prozent
#define ADC_TO_PERCENT(adc_value) \
    VOLTAGE_TO_PERCENT(ADC_TO_VOLTAGE(adc_value))

// ============================================
// Antennenumschaltung Helper Makros
// ============================================

#ifdef ARDUINO
    // Arduino-Version der Antennenumschaltung
    #define INIT_ANTENNA_SWITCH(antenna_type) \
        do { \
            pinMode(ANTENNA_RF_SWITCH_GPIO, OUTPUT); \
            pinMode(ANTENNA_SELECT_GPIO, OUTPUT); \
            digitalWrite(ANTENNA_RF_SWITCH_GPIO, ANTENNA_RF_SWITCH_ENABLE); \
            delay(100); /* Stabilisierung */ \
            digitalWrite(ANTENNA_SELECT_GPIO, antenna_type); \
        } while(0)

    #define SET_ANTENNA_INTERNAL() \
        digitalWrite(ANTENNA_SELECT_GPIO, ANTENNA_INTERNAL)

    #define SET_ANTENNA_EXTERNAL() \
        digitalWrite(ANTENNA_SELECT_GPIO, ANTENNA_EXTERNAL)
#else
    // ESP-IDF-Version der Antennenumschaltung
    #define INIT_ANTENNA_SWITCH(antenna_type) \
        do { \
            gpio_config_t io_conf = {}; \
            io_conf.pin_bit_mask = (1ULL << ANTENNA_RF_SWITCH_GPIO) | (1ULL << ANTENNA_SELECT_GPIO); \
            io_conf.mode = GPIO_MODE_OUTPUT; \
            io_conf.pull_up_en = GPIO_PULLUP_DISABLE; \
            io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; \
            io_conf.intr_type = GPIO_INTR_DISABLE; \
            gpio_config(&io_conf); \
            gpio_set_level((gpio_num_t)ANTENNA_RF_SWITCH_GPIO, ANTENNA_RF_SWITCH_ENABLE); \
            vTaskDelay(pdMS_TO_TICKS(100)); /* Stabilisierung */ \
            gpio_set_level((gpio_num_t)ANTENNA_SELECT_GPIO, antenna_type); \
        } while(0)

    #define SET_ANTENNA_INTERNAL() \
        gpio_set_level((gpio_num_t)ANTENNA_SELECT_GPIO, ANTENNA_INTERNAL)

    #define SET_ANTENNA_EXTERNAL() \
        gpio_set_level((gpio_num_t)ANTENNA_SELECT_GPIO, ANTENNA_EXTERNAL)
#endif

#endif // HARDWARE_H
