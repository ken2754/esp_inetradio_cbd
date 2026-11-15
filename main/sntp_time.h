#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sntp_sync_cb_t)(void);

typedef struct {
    int hour12;       // 1 to 12
    int min;          // 0 to 59
    bool is_pm;       // true if PM, false if AM
    char am_pm[3];    // "AM" or "PM"
    char time_str[8]; // e.g. " 3:45" or "12:00"
    char date_str[32];// e.g. "Sat, Oct 17"
    bool is_synced;
} clock_display_time_t;

esp_err_t sntp_time_init(sntp_sync_cb_t cb);
void sntp_time_restart(void);
void sntp_time_update_tz(const char *tz_string);
bool sntp_time_is_synced(void);
bool sntp_time_get_local(struct tm *out_tm);
void sntp_time_get_current(clock_display_time_t *out_time);

#ifdef __cplusplus
}
#endif
