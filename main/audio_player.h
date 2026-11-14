#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_STATE_STOPPED = 0,
    PLAYER_STATE_CONNECTING,
    PLAYER_STATE_BUFFERING,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_ERROR,
} player_state_t;

typedef struct {
    player_state_t state;
    int station_index;
    char station_name[64];
    char stream_title[160];
    char stream_artist[96];
    char stream_track[128];
    char codec_name[16];
    char status_msg[96];
    uint32_t sample_rate;
    uint32_t bitrate_kbps;
    uint8_t volume;
    bool muted;
    uint8_t buffer_pct;
} player_status_t;

esp_err_t audio_player_init(i2c_master_bus_handle_t i2c_bus);

/* Playback control */
esp_err_t audio_player_play_station(int index);
esp_err_t audio_player_stop(void);
esp_err_t audio_player_toggle_play_pause(void);
esp_err_t audio_player_next(void);
esp_err_t audio_player_prev(void);

/* Volume & Mute */
esp_err_t audio_player_set_volume(uint8_t volume);
esp_err_t audio_player_set_volume_live(uint8_t volume, bool save_nvs);
uint8_t audio_player_get_volume(void);
esp_err_t audio_player_set_mute(bool mute);
bool audio_player_is_muted(void);

/* Thread-safe status snapshot */
void audio_player_get_status(player_status_t *out_status);

/* Called when station list changes (e.g. from web UI) */
void audio_player_notify_stations_changed(void);

#ifdef __cplusplus
}
#endif
