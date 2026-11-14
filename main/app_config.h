#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_MAX_STATIONS        24
#define APP_STATION_NAME_LEN    64
#define APP_STATION_URL_LEN     256
#define APP_STATION_GENRE_LEN   48
#define APP_WIFI_SSID_LEN       33
#define APP_WIFI_PASS_LEN       65
#define APP_DEFAULT_AP_SSID     "ES3C28P-Radio"
#define APP_MDNS_HOSTNAME       "es3c28p-radio"

typedef struct {
    char name[APP_STATION_NAME_LEN];
    char url[APP_STATION_URL_LEN];
    char genre[APP_STATION_GENRE_LEN];
} radio_station_t;

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_LEN];
    char wifi_pass[APP_WIFI_PASS_LEN];

    uint8_t volume;          /* 0 - 100 */
    uint8_t brightness;      /* 10 - 100 */
    uint8_t current_station; /* 0-based index */
    bool autoplay;
    bool invert_color;       /* ILI9341 IPS inversion */
    bool flip_180;           /* Flip landscape 180 deg */
    bool swap_rb;            /* RGB <-> BGR order */

    uint8_t station_count;
} app_config_meta_t;

esp_err_t app_config_init(void);

/* Lightweight thread-safe copy (excludes station array to keep stack small) */
void app_config_get_meta(app_config_meta_t *out_meta);

/* Station helpers */
int app_config_get_station_count(void);
bool app_config_get_station(int index, radio_station_t *out_station);
esp_err_t app_config_add_station(const char *name, const char *url, const char *genre);
esp_err_t app_config_update_station(int index, const char *name, const char *url, const char *genre);
esp_err_t app_config_delete_station(int index);
esp_err_t app_config_move_station(int from_index, int to_index);
esp_err_t app_config_reset_default_stations(void);

/* Individual setters (persisted to NVS) */
esp_err_t app_config_set_wifi(const char *ssid, const char *pass);
esp_err_t app_config_set_volume(uint8_t volume);
esp_err_t app_config_set_current_station(uint8_t index);
esp_err_t app_config_set_autoplay(bool autoplay);
esp_err_t app_config_set_display(uint8_t brightness, bool invert_color, bool flip_180, bool swap_rb);

#ifdef __cplusplus
}
#endif
