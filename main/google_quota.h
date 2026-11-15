#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    char name[32];
    float remaining_fraction;
    int64_t reset_epoch_sec;
    char reset_time_str[32];
} quota_window_t;

typedef struct {
    bool is_loading;
    bool has_error;
    char error_msg[64];
    quota_window_t limit_5h;
    quota_window_t limit_weekly;
    quota_window_t limit_claude_5h;
    quota_window_t limit_claude_wk;
    quota_window_t limit_argon;
    int64_t last_update_sec;
} google_quota_data_t;

void google_quota_init(void);
void google_quota_set_active(bool active);
void google_quota_request_refresh(void);
void google_quota_get_data(google_quota_data_t *out_data);

#ifdef __cplusplus
}
#endif
