/**
 * @file time_sync.h
 * @brief Zentrale Zeit-Synchronisation (NTP, ZigBee, BLE)
 *
 * Gemeinsame Sub-Routine für "harte" Zeitkorrektur (settimeofday).
 * time_sync_last_epoch speichert die UNIX-Epoch der letzten erfolgreichen
 * Synchronisation (64-bit für ESP-IDF 5.x).
 */

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RTC-Variable: 0 = nie synchronisiert, sonst UNIX-Epoch (64-bit) der letzten Sync.
 * Ermöglicht: Sekunden seit letztem Sync = time(NULL) - time_sync_last_epoch
 */
extern uint64_t time_sync_last_epoch;

/**
 * Setzt Systemzeit per settimeofday() und aktualisiert time_sync_last_epoch.
 *
 * @param unix_time UTC-Zeit (Sekunden seit 1970-01-01)
 * @param source    Quelle für Logging ("NTP", "ZigBee", "BLE")
 * @return true bei Erfolg
 */
bool time_sync_set_hard(time_t unix_time, const char *source);

/**
 * Sekunden seit letztem erfolgreichen Sync.
 *
 * @return Sekunden, oder -1 wenn nie synchronisiert (time_sync_last_epoch == 0)
 */
int64_t time_sync_seconds_since_last(void);

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_H */
