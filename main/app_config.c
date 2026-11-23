#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_config";

#define NVS_NAMESPACE   "radio_cfg"
#define NVS_KEY_META    "meta_v2"
#define NVS_KEY_ST_FMT  "st_%d"
#define NVS_KEY_CLOCK   "clock_v1"
#define DEFAULT_CLOCK_TZ "JST-9"

typedef struct {
    app_config_meta_t meta;
    radio_station_t stations[APP_MAX_STATIONS];
} app_config_t;

static app_config_t s_cfg;
static app_clock_config_t s_clock;
static SemaphoreHandle_t s_cfg_mutex = NULL;

static void safe_copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static const radio_station_t s_default_somafm_stations[] = {
    {
        .name  = "SomaFM: Groove Salad",
        .url   = "http://ice1.somafm.com/groovesalad-128-mp3",
        .genre = "Ambient / Downtempo"
    },
    {
        .name  = "SomaFM: Drone Zone",
        .url   = "http://ice1.somafm.com/dronezone-128-mp3",
        .genre = "Atmospheric Ambient"
    },
    {
        .name  = "SomaFM: Space Station",
        .url   = "http://ice1.somafm.com/spacestation-128-mp3",
        .genre = "Spaced-out Electronica"
    },
    {
        .name  = "SomaFM: Secret Agent",
        .url   = "http://ice1.somafm.com/secretagent-128-mp3",
        .genre = "Lounge / Downtempo"
    },
    {
        .name  = "SomaFM: Lush",
        .url   = "http://ice1.somafm.com/lush-128-mp3",
        .genre = "Mellow Vocals"
    },
    {
        .name  = "SomaFM: Deep Space One",
        .url   = "http://ice1.somafm.com/deepspaceone-128-mp3",
        .genre = "Deep Space Ambient"
    },
    {
        .name  = "SomaFM: Beat Blender",
        .url   = "http://ice1.somafm.com/beatblender-128-mp3",
        .genre = "Deep House / Beats"
    },
    {
        .name  = "SomaFM: Indie Pop Rocks!",
        .url   = "http://ice1.somafm.com/indiepop-128-mp3",
        .genre = "Indie Pop / Rock"
    }
};

static void load_default_stations_locked(void)
{
    size_t count = sizeof(s_default_somafm_stations) / sizeof(s_default_somafm_stations[0]);
    if (count > APP_MAX_STATIONS) {
        count = APP_MAX_STATIONS;
    }
    memset(s_cfg.stations, 0, sizeof(s_cfg.stations));
    memcpy(s_cfg.stations, s_default_somafm_stations, count * sizeof(radio_station_t));
    s_cfg.meta.station_count = (uint8_t)count;
    if (s_cfg.meta.current_station >= s_cfg.meta.station_count) {
        s_cfg.meta.current_station = 0;
    }
}

static void load_defaults_locked(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.meta.volume = 65;
    s_cfg.meta.brightness = 80;
    s_cfg.meta.current_station = 0;
    s_cfg.meta.autoplay = true;
    s_cfg.meta.invert_color = true;
    s_cfg.meta.flip_180 = false;
    s_cfg.meta.swap_rb = false;
    load_default_stations_locked();
}

static void load_clock_defaults_locked(void)
{
    memset(&s_clock, 0, sizeof(s_clock));
    safe_copy_str(s_clock.timezone, sizeof(s_clock.timezone), DEFAULT_CLOCK_TZ);
    s_clock.wc_font_mode = 0;
    s_clock.idle_clock_sec = 0;
}

static esp_err_t save_blob_locked(const char *key, const void *data, size_t len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save %s: %s", key, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t save_meta_locked(void)
{
    return save_blob_locked(NVS_KEY_META, &s_cfg.meta, sizeof(s_cfg.meta));
}

static esp_err_t save_clock_locked(void)
{
    return save_blob_locked(NVS_KEY_CLOCK, &s_clock, sizeof(s_clock));
}

static esp_err_t save_all_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY_META, &s_cfg.meta, sizeof(s_cfg.meta));
    if (err == ESP_OK) {
        char key[16];
        for (int i = 0; i < s_cfg.meta.station_count; i++) {
            snprintf(key, sizeof(key), NVS_KEY_ST_FMT, i);
            err = nvs_set_blob(handle, key, &s_cfg.stations[i], sizeof(radio_station_t));
            if (err != ESP_OK) break;
        }
        /* Clean up unused trailing keys if any */
        if (err == ESP_OK) {
            for (int i = s_cfg.meta.station_count; i < APP_MAX_STATIONS; i++) {
                snprintf(key, sizeof(key), NVS_KEY_ST_FMT, i);
                nvs_erase_key(handle, key);
            }
            err = nvs_commit(handle);
        }
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_config_init(void)
{
    if (s_cfg_mutex == NULL) {
        s_cfg_mutex = xSemaphoreCreateMutex();
        if (s_cfg_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    bool loaded = false;
    if (err == ESP_OK) {
        size_t required_size = sizeof(s_cfg.meta);
        err = nvs_get_blob(handle, NVS_KEY_META, &s_cfg.meta, &required_size);
        if (err == ESP_OK && required_size == sizeof(s_cfg.meta)) {
            if (s_cfg.meta.station_count > APP_MAX_STATIONS) {
                s_cfg.meta.station_count = APP_MAX_STATIONS;
            }
            int valid_cnt = 0;
            char key[16];
            for (int i = 0; i < s_cfg.meta.station_count; i++) {
                snprintf(key, sizeof(key), NVS_KEY_ST_FMT, i);
                size_t st_sz = sizeof(radio_station_t);
                if (nvs_get_blob(handle, key, &s_cfg.stations[valid_cnt], &st_sz) == ESP_OK &&
                    st_sz == sizeof(radio_station_t)) {
                    valid_cnt++;
                }
            }
            s_cfg.meta.station_count = (uint8_t)valid_cnt;
            if (s_cfg.meta.station_count == 0) {
                load_default_stations_locked();
            }
            if (s_cfg.meta.current_station >= s_cfg.meta.station_count) {
                s_cfg.meta.current_station = 0;
            }
            if (s_cfg.meta.volume > 100) {
                s_cfg.meta.volume = 65;
            }
            if (s_cfg.meta.brightness < 10 || s_cfg.meta.brightness > 100) {
                s_cfg.meta.brightness = 80;
            }
            loaded = true;
            ESP_LOGI(TAG, "Loaded config from NVS (%d stations, wifi='%s')",
                     s_cfg.meta.station_count, s_cfg.meta.wifi_ssid);
        }
        nvs_close(handle);
    }

    if (!loaded) {
        ESP_LOGI(TAG, "Initializing default config with 8 SomaFM presets");
        load_defaults_locked();
        save_all_locked();
    }

    bool clock_loaded = false;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t clock_size = sizeof(s_clock);
        err = nvs_get_blob(handle, NVS_KEY_CLOCK, &s_clock, &clock_size);
        if (err == ESP_OK && clock_size == sizeof(s_clock)) {
            s_clock.timezone[sizeof(s_clock.timezone) - 1] = '\0';
            if (s_clock.timezone[0] == '\0') {
                safe_copy_str(s_clock.timezone, sizeof(s_clock.timezone), DEFAULT_CLOCK_TZ);
            }
            clock_loaded = true;
            ESP_LOGI(TAG, "Loaded clock config from NVS (tz='%s', idle=%us)",
                     s_clock.timezone, (unsigned)s_clock.idle_clock_sec);
        }
        nvs_close(handle);
    }

    if (!clock_loaded) {
        ESP_LOGI(TAG, "Initializing default clock config (tz='%s')", DEFAULT_CLOCK_TZ);
        load_clock_defaults_locked();
        save_clock_locked();
    }

    xSemaphoreGive(s_cfg_mutex);
    return ESP_OK;
}

void app_config_get_meta(app_config_meta_t *out_meta)
{
    if (!out_meta) return;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(out_meta, &s_cfg.meta, sizeof(app_config_meta_t));
    xSemaphoreGive(s_cfg_mutex);
}

int app_config_get_station_count(void)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    int cnt = s_cfg.meta.station_count;
    xSemaphoreGive(s_cfg_mutex);
    return cnt;
}

bool app_config_get_station(int index, radio_station_t *out_station)
{
    if (!out_station) return false;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool ok = (index >= 0 && index < s_cfg.meta.station_count);
    if (ok) {
        memcpy(out_station, &s_cfg.stations[index], sizeof(radio_station_t));
    }
    xSemaphoreGive(s_cfg_mutex);
    return ok;
}

esp_err_t app_config_add_station(const char *name, const char *url, const char *genre)
{
    if (!name || !url || name[0] == '\0' || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (s_cfg.meta.station_count >= APP_MAX_STATIONS) {
        xSemaphoreGive(s_cfg_mutex);
        return ESP_ERR_NO_MEM;
    }
    radio_station_t *st = &s_cfg.stations[s_cfg.meta.station_count];
    safe_copy_str(st->name, sizeof(st->name), name);
    safe_copy_str(st->url, sizeof(st->url), url);
    safe_copy_str(st->genre, sizeof(st->genre), genre ? genre : "");
    s_cfg.meta.station_count++;
    esp_err_t err = save_all_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_update_station(int index, const char *name, const char *url, const char *genre)
{
    if (!name || !url || name[0] == '\0' || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_cfg.meta.station_count) {
        xSemaphoreGive(s_cfg_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    radio_station_t *st = &s_cfg.stations[index];
    safe_copy_str(st->name, sizeof(st->name), name);
    safe_copy_str(st->url, sizeof(st->url), url);
    safe_copy_str(st->genre, sizeof(st->genre), genre ? genre : "");
    esp_err_t err = save_all_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_delete_station(int index)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_cfg.meta.station_count || s_cfg.meta.station_count <= 1) {
        xSemaphoreGive(s_cfg_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = index; i < s_cfg.meta.station_count - 1; i++) {
        s_cfg.stations[i] = s_cfg.stations[i + 1];
    }
    s_cfg.meta.station_count--;
    memset(&s_cfg.stations[s_cfg.meta.station_count], 0, sizeof(radio_station_t));

    if (s_cfg.meta.current_station == index) {
        if (s_cfg.meta.current_station >= s_cfg.meta.station_count) {
            s_cfg.meta.current_station = s_cfg.meta.station_count - 1;
        }
    } else if (s_cfg.meta.current_station > index) {
        s_cfg.meta.current_station--;
    }

    esp_err_t err = save_all_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_move_station(int from_index, int to_index)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (from_index < 0 || from_index >= s_cfg.meta.station_count ||
        to_index < 0 || to_index >= s_cfg.meta.station_count ||
        from_index == to_index) {
        xSemaphoreGive(s_cfg_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    radio_station_t tmp = s_cfg.stations[from_index];
    if (from_index < to_index) {
        for (int i = from_index; i < to_index; i++) {
            s_cfg.stations[i] = s_cfg.stations[i + 1];
        }
    } else {
        for (int i = from_index; i > to_index; i--) {
            s_cfg.stations[i] = s_cfg.stations[i - 1];
        }
    }
    s_cfg.stations[to_index] = tmp;

    if (s_cfg.meta.current_station == from_index) {
        s_cfg.meta.current_station = (uint8_t)to_index;
    } else if (from_index < to_index && s_cfg.meta.current_station > from_index && s_cfg.meta.current_station <= to_index) {
        s_cfg.meta.current_station--;
    } else if (from_index > to_index && s_cfg.meta.current_station >= to_index && s_cfg.meta.current_station < from_index) {
        s_cfg.meta.current_station++;
    }

    esp_err_t err = save_all_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_reset_default_stations(void)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    load_default_stations_locked();
    s_cfg.meta.current_station = 0;
    esp_err_t err = save_all_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    safe_copy_str(s_cfg.meta.wifi_ssid, sizeof(s_cfg.meta.wifi_ssid), ssid);
    safe_copy_str(s_cfg.meta.wifi_pass, sizeof(s_cfg.meta.wifi_pass), pass ? pass : "");
    esp_err_t err = save_meta_locked();
    xSemaphoreGive(s_cfg_mutex);
    ESP_LOGI(TAG, "Saved Wi-Fi SSID='%s' (%s)", s_cfg.meta.wifi_ssid, esp_err_to_name(err));
    return err;
}

esp_err_t app_config_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg.meta.volume = volume;
    esp_err_t err = save_meta_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_current_station(uint8_t index)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    if (index >= s_cfg.meta.station_count) {
        xSemaphoreGive(s_cfg_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg.meta.current_station = index;
    esp_err_t err = save_meta_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_autoplay(bool autoplay)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg.meta.autoplay = autoplay;
    esp_err_t err = save_meta_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_display(uint8_t brightness, bool invert_color, bool flip_180, bool swap_rb)
{
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg.meta.brightness = brightness;
    s_cfg.meta.invert_color = invert_color;
    s_cfg.meta.flip_180 = flip_180;
    s_cfg.meta.swap_rb = swap_rb;
    esp_err_t err = save_meta_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_brightness(uint8_t brightness)
{
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg.meta.brightness = brightness;
    esp_err_t err = save_meta_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

void app_config_get_clock(app_clock_config_t *out_clock)
{
    if (!out_clock) return;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    memcpy(out_clock, &s_clock, sizeof(app_clock_config_t));
    xSemaphoreGive(s_cfg_mutex);
}

esp_err_t app_config_set_clock(const app_clock_config_t *clock)
{
    if (!clock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    safe_copy_str(s_clock.timezone, sizeof(s_clock.timezone),
                  (clock->timezone[0] != '\0') ? clock->timezone : DEFAULT_CLOCK_TZ);
    s_clock.wc_font_mode = clock->wc_font_mode;
    s_clock.idle_clock_sec = clock->idle_clock_sec;
    esp_err_t err = save_clock_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t app_config_set_wc_font(uint8_t font_mode)
{
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_clock.wc_font_mode = font_mode;
    esp_err_t err = save_clock_locked();
    xSemaphoreGive(s_cfg_mutex);
    return err;
}
