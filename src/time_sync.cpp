/**
 * @file time_sync.cpp
 * @brief Implementierung der zentralen Zeit-Synchronisation
 */

#include "time_sync.h"
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "esp_attr.h"  // RTC_DATA_ATTR

static const char *TAG = "time_sync";

RTC_DATA_ATTR uint64_t time_sync_last_epoch = 0;
RTC_DATA_ATTR char time_sync_last_source[12] = "";  // "NTP", "ZigBee", "BLE", etc.

bool time_sync_set_hard(time_t unix_time, const char *source) {
    struct timeval tv = {
        .tv_sec = unix_time,
        .tv_usec = 0
    };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday fehlgeschlagen (source: %s)", source ? source : "?");
        return false;
    }
    time_sync_last_epoch = (uint64_t)unix_time;
    if (source && strlen(source) < sizeof(time_sync_last_source)) {
        strncpy(time_sync_last_source, source, sizeof(time_sync_last_source) - 1);
        time_sync_last_source[sizeof(time_sync_last_source) - 1] = '\0';
    } else {
        time_sync_last_source[0] = '\0';
    }
    ESP_LOGI(TAG, "Zeit synchronisiert via %s (epoch: %llu)", source ? source : "?",
             (unsigned long long)time_sync_last_epoch);
    return true;
}

int64_t time_sync_seconds_since_last(void) {
    if (time_sync_last_epoch == 0) {
        return -1;
    }
    time_t now = time(NULL);
    return (int64_t)now - (int64_t)time_sync_last_epoch;
}
