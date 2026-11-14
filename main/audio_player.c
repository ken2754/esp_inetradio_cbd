#include "audio_player.h"
#include "app_config.h"
#include "board_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio_player";

#define AUDIO_RINGBUF_SIZE      (192 * 1024)
#define AUDIO_PREBUFFER_BYTES   (24 * 1024)
#define HTTP_READ_CHUNK_SIZE    4096
#define DEC_IN_BUF_SIZE         8192
#define DEC_OUT_BUF_SIZE        8192
#define MAX_REDIRECTS           5

typedef enum {
    CODEC_AUTO = 0,
    CODEC_MP3,
    CODEC_AAC,
} stream_codec_t;

static i2s_chan_handle_t s_tx_handle = NULL;
static esp_codec_dev_handle_t s_codec_dev = NULL;
static RingbufHandle_t s_ringbuf = NULL;
static SemaphoreHandle_t s_status_mutex = NULL;
static SemaphoreHandle_t s_codec_mutex = NULL;

static player_status_t s_status;
static volatile uint32_t s_play_gen = 0;
static volatile bool s_want_play = false;
static volatile int s_target_station = 0;
static volatile stream_codec_t s_detected_codec = CODEC_MP3;
static volatile int s_icy_metaint = 0;
static volatile bool s_stream_eof = false;
static uint32_t s_current_sample_rate = 44100;
static bool s_codec_is_open = false;

static void update_status_locked_start(void)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
}

static void update_status_locked_end(void)
{
    xSemaphoreGive(s_status_mutex);
}

static void set_status_msg(player_state_t state, const char *msg)
{
    update_status_locked_start();
    s_status.state = state;
    if (msg) {
        snprintf(s_status.status_msg, sizeof(s_status.status_msg), "%s", msg);
    }
    update_status_locked_end();
}

static esp_err_t init_i2s_and_codec(i2c_master_bus_handle_t i2c_bus)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 512;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_PIN_MCLK,
            .bclk = BOARD_AUDIO_PIN_BCLK,
            .ws   = BOARD_AUDIO_PIN_LRCK,
            .dout = BOARD_AUDIO_PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_AUDIO_I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return ESP_FAIL;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        return ESP_FAIL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_AUDIO_PIN_PA_EN,
        .pa_reverted = true, /* ES3C28P FM8002E: LOW = Enable, HIGH = Shutdown */
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = 256,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_codec_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = 44100,
    };
    if (esp_codec_dev_open(s_codec_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        return ESP_FAIL;
    }
    s_codec_is_open = true;
    s_current_sample_rate = 44100;

    static esp_codec_dev_vol_map_t s_vol_map[] = {
        { .vol = 0,   .db_value = -96.0f },
        { .vol = 1,   .db_value = -36.0f },
        { .vol = 50,  .db_value = -16.0f },
        { .vol = 100, .db_value = 0.0f   },
    };
    esp_codec_dev_vol_curve_t vol_curve = {
        .vol_map = s_vol_map,
        .count = sizeof(s_vol_map) / sizeof(s_vol_map[0]),
    };
    esp_codec_dev_set_vol_curve(s_codec_dev, &vol_curve);

    app_config_meta_t cfg;
    app_config_get_meta(&cfg);
    esp_codec_dev_set_out_vol(s_codec_dev, cfg.volume);
    esp_codec_dev_set_out_mute(s_codec_dev, true);

    ESP_LOGI(TAG, "ES8311 + I2S initialized (PA GPIO%d active-low)", BOARD_AUDIO_PIN_PA_EN);
    return ESP_OK;
}

static void codec_ensure_sample_rate(uint32_t sample_rate)
{
    if (sample_rate < 8000 || sample_rate > 96000) return;

    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    if (!s_codec_is_open || s_current_sample_rate != sample_rate) {
        ESP_LOGI(TAG, "Reconfiguring codec sample rate: %lu -> %lu Hz",
                 (unsigned long)s_current_sample_rate, (unsigned long)sample_rate);
        if (s_codec_is_open) {
            esp_codec_dev_close(s_codec_dev);
            s_codec_is_open = false;
        }
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 2,
            .sample_rate = sample_rate,
        };
        if (esp_codec_dev_open(s_codec_dev, &fs) == ESP_CODEC_DEV_OK) {
            s_codec_is_open = true;
            s_current_sample_rate = sample_rate;
            update_status_locked_start();
            uint8_t vol = s_status.volume;
            bool muted = s_status.muted;
            update_status_locked_end();
            esp_codec_dev_set_out_vol(s_codec_dev, vol);
            esp_codec_dev_set_out_mute(s_codec_dev, muted);
        }
    }
    xSemaphoreGive(s_codec_mutex);
}

static void codec_set_active_mute(bool mute)
{
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    if (s_codec_is_open) {
        esp_codec_dev_set_out_mute(s_codec_dev, mute);
    }
    xSemaphoreGive(s_codec_mutex);
}

static void flush_ringbuf(void)
{
    size_t item_size = 0;
    while (1) {
        void *p = xRingbufferReceiveUpTo(s_ringbuf, &item_size, 0, 4096);
        if (!p || item_size == 0) break;
        vRingbufferReturnItem(s_ringbuf, p);
    }
}

static uint8_t get_ringbuf_pct(void)
{
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_ringbuf, NULL, NULL, NULL, NULL, &waiting);
    uint32_t pct = ((uint32_t)waiting * 100) / AUDIO_RINGBUF_SIZE;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

typedef struct {
    char content_type[64];
    char location[256];
    char icy_name[64];
    int icy_metaint;
    int icy_br;
} http_hdr_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        http_hdr_ctx_t *ctx = (http_hdr_ctx_t *)evt->user_data;
        if (!ctx) return ESP_OK;

        if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            snprintf(ctx->content_type, sizeof(ctx->content_type), "%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "Location") == 0) {
            snprintf(ctx->location, sizeof(ctx->location), "%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            ctx->icy_metaint = atoi(evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
            snprintf(ctx->icy_name, sizeof(ctx->icy_name), "%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
            ctx->icy_br = atoi(evt->header_value);
        }
    }
    return ESP_OK;
}

static void clear_track_metadata_locked(void)
{
    s_status.stream_title[0] = '\0';
    s_status.stream_artist[0] = '\0';
    s_status.stream_track[0] = '\0';
}

static void trim_ws(char *s)
{
    if (!s || !*s) return;
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static void parse_icy_metadata(const char *meta_str)
{
    const char *p = strstr(meta_str, "StreamTitle='");
    if (!p) {
        p = strstr(meta_str, "StreamTitle=\"");
    }
    if (!p) return;

    char quote = p[12];
    p += 13;
    const char *end = strstr(p, quote == '\'' ? "';" : "\";");
    if (!end) {
        end = strchr(p, quote);
    }
    if (!end) return;

    size_t len = (size_t)(end - p);
    if (len >= sizeof(s_status.stream_title)) {
        len = sizeof(s_status.stream_title) - 1;
    }

    char title_buf[160];
    memcpy(title_buf, p, len);
    title_buf[len] = '\0';
    trim_ws(title_buf);

    char artist_buf[96] = {0};
    char track_buf[128] = {0};
    const char *sep = strstr(title_buf, " - ");
    if (sep) {
        size_t alen = (size_t)(sep - title_buf);
        if (alen >= sizeof(artist_buf)) alen = sizeof(artist_buf) - 1;
        memcpy(artist_buf, title_buf, alen);
        artist_buf[alen] = '\0';
        trim_ws(artist_buf);

        snprintf(track_buf, sizeof(track_buf), "%.127s", sep + 3);
        trim_ws(track_buf);
    } else {
        snprintf(track_buf, sizeof(track_buf), "%.127s", title_buf);
    }

    update_status_locked_start();
    snprintf(s_status.stream_title, sizeof(s_status.stream_title), "%s", title_buf);
    snprintf(s_status.stream_artist, sizeof(s_status.stream_artist), "%s", artist_buf);
    snprintf(s_status.stream_track, sizeof(s_status.stream_track), "%s", track_buf);
    update_status_locked_end();
    ESP_LOGI(TAG, "ICY StreamTitle: %s (artist='%s', track='%s')", title_buf, artist_buf, track_buf);
}

static bool extract_playlist_url(const char *text, char *out_url, size_t out_len)
{
    const char *p = text;
    while (p && *p) {
        const char *line_end = strpbrk(p, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);
        if (line_len > 7) {
            if (strncasecmp(p, "File1=", 6) == 0) {
                p += 6;
                line_len -= 6;
            }
            if (strncasecmp(p, "http://", 7) == 0 || strncasecmp(p, "https://", 8) == 0) {
                size_t copy_len = line_len < out_len - 1 ? line_len : out_len - 1;
                memcpy(out_url, p, copy_len);
                out_url[copy_len] = '\0';
                /* trim trailing whitespace */
                while (copy_len > 0 && isspace((unsigned char)out_url[copy_len - 1])) {
                    out_url[--copy_len] = '\0';
                }
                return true;
            }
        }
        p = line_end ? (line_end + 1) : NULL;
    }
    return false;
}

static stream_codec_t detect_codec(const char *content_type, const char *url)
{
    if (content_type[0] != '\0') {
        if (strcasestr(content_type, "aac") != NULL) {
            return CODEC_AAC;
        }
        if (strcasestr(content_type, "mpeg") != NULL || strcasestr(content_type, "mp3") != NULL) {
            return CODEC_MP3;
        }
    }
    if (url && (strcasestr(url, "aac") != NULL || strcasestr(url, ".m4a") != NULL)) {
        return CODEC_AAC;
    }
    return CODEC_MP3;
}

static bool push_audio_bytes(const uint8_t *data, size_t len, uint32_t gen)
{
    size_t offset = 0;
    while (offset < len) {
        if (!s_want_play || s_play_gen != gen) {
            return false;
        }
        size_t chunk = len - offset;
        if (chunk > 2048) chunk = 2048;
        if (xRingbufferSend(s_ringbuf, data + offset, chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
            offset += chunk;
        }
    }
    return true;
}

static void stream_task(void *arg)
{
    uint8_t *rx_buf = heap_caps_malloc(HTTP_READ_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx_buf) {
        rx_buf = malloc(HTTP_READ_CHUNK_SIZE);
    }

    char current_url[APP_STATION_URL_LEN];
    char redirect_url[APP_STATION_URL_LEN];

    while (1) {
        if (!s_want_play) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t gen = s_play_gen;
        int st_idx = s_target_station;
        radio_station_t st;
        if (!app_config_get_station(st_idx, &st)) {
            set_status_msg(PLAYER_STATE_ERROR, "Invalid station");
            s_want_play = false;
            continue;
        }

        update_status_locked_start();
        s_status.state = PLAYER_STATE_CONNECTING;
        s_status.station_index = st_idx;
        snprintf(s_status.station_name, sizeof(s_status.station_name), "%s", st.name);
        clear_track_metadata_locked();
        s_status.codec_name[0] = '\0';
        s_status.bitrate_kbps = 0;
        snprintf(s_status.status_msg, sizeof(s_status.status_msg), "Connecting...");
        update_status_locked_end();

        snprintf(current_url, sizeof(current_url), "%s", st.url);
        int backoff_ms = 1000;

        while (s_want_play && s_play_gen == gen) {
            s_stream_eof = false;
            int redirects = 0;
            bool opened = false;
            http_hdr_ctx_t hdr = {0};
            esp_http_client_handle_t client = NULL;

            while (redirects < MAX_REDIRECTS && s_want_play && s_play_gen == gen) {
                memset(&hdr, 0, sizeof(hdr));
                esp_http_client_config_t cfg = {
                    .url = current_url,
                    .timeout_ms = 8000,
                    .buffer_size = 4096,
                    .buffer_size_tx = 1024,
                    .user_agent = "ES3C28P-Radio/1.0",
                    .event_handler = http_event_handler,
                    .user_data = &hdr,
                    .crt_bundle_attach = esp_crt_bundle_attach,
                    .disable_auto_redirect = true,
                };
                client = esp_http_client_init(&cfg);
                if (!client) break;

                esp_http_client_set_header(client, "Icy-MetaData", "1");

                esp_err_t err = esp_http_client_open(client, 0);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "HTTP open failed: %s", esp_err_to_name(err));
                    esp_http_client_cleanup(client);
                    client = NULL;
                    break;
                }

                int content_length = esp_http_client_fetch_headers(client);
                (void)content_length;
                int status = esp_http_client_get_status_code(client);

                if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                    if (hdr.location[0] != '\0') {
                        ESP_LOGI(TAG, "Redirect %d -> %s", status, hdr.location);
                        snprintf(redirect_url, sizeof(redirect_url), "%s", hdr.location);
                        snprintf(current_url, sizeof(current_url), "%s", redirect_url);
                        esp_http_client_close(client);
                        esp_http_client_cleanup(client);
                        client = NULL;
                        redirects++;
                        continue;
                    }
                }

                if (status >= 200 && status < 300) {
                    opened = true;
                } else {
                    ESP_LOGW(TAG, "Unexpected HTTP status: %d", status);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    client = NULL;
                }
                break;
            }

            if (!opened || !client) {
                if (s_want_play && s_play_gen == gen) {
                    set_status_msg(PLAYER_STATE_ERROR, "Connection failed, retrying...");
                    for (int t = 0; t < backoff_ms && s_want_play && s_play_gen == gen; t += 100) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    if (backoff_ms < 5000) backoff_ms += 1000;
                }
                continue;
            }

            /* Handle .pls / .m3u playlist response if server returned text */
            if (strcasestr(hdr.content_type, "scpls") != NULL ||
                strcasestr(hdr.content_type, "mpegurl") != NULL ||
                strcasestr(hdr.content_type, "text/plain") != NULL) {
                int r = esp_http_client_read(client, (char *)rx_buf, HTTP_READ_CHUNK_SIZE - 1);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                if (r > 0) {
                    rx_buf[r] = '\0';
                    if (extract_playlist_url((const char *)rx_buf, redirect_url, sizeof(redirect_url))) {
                        ESP_LOGI(TAG, "Playlist resolved to: %s", redirect_url);
                        snprintf(current_url, sizeof(current_url), "%s", redirect_url);
                        continue;
                    }
                }
                set_status_msg(PLAYER_STATE_ERROR, "Playlist parse failed");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            backoff_ms = 1000;
            s_icy_metaint = hdr.icy_metaint;
            s_detected_codec = detect_codec(hdr.content_type, current_url);

            update_status_locked_start();
            s_status.state = PLAYER_STATE_BUFFERING;
            s_status.bitrate_kbps = (uint32_t)hdr.icy_br;
            snprintf(s_status.codec_name, sizeof(s_status.codec_name), "%s",
                     s_detected_codec == CODEC_AAC ? "AAC" : "MP3");
            snprintf(s_status.status_msg, sizeof(s_status.status_msg), "Buffering...");
            update_status_locked_end();

            ESP_LOGI(TAG, "Stream open: codec=%s metaint=%d br=%d ct='%s'",
                     s_detected_codec == CODEC_AAC ? "AAC" : "MP3",
                     hdr.icy_metaint, hdr.icy_br, hdr.content_type);

            int metaint = hdr.icy_metaint;
            int audio_rem = metaint;
            int meta_len_rem = -1;
            int meta_pos = 0;
            static char meta_buf[4096];

            while (s_want_play && s_play_gen == gen) {
                int read_len = esp_http_client_read(client, (char *)rx_buf, HTTP_READ_CHUNK_SIZE);
                if (read_len < 0) {
                    ESP_LOGW(TAG, "Stream read error");
                    break;
                }
                if (read_len == 0) {
                    if (esp_http_client_is_complete_data_received(client)) {
                        ESP_LOGI(TAG, "Stream EOF");
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }

                if (metaint <= 0) {
                    if (!push_audio_bytes(rx_buf, (size_t)read_len, gen)) {
                        break;
                    }
                } else {
                    int idx = 0;
                    while (idx < read_len && s_want_play && s_play_gen == gen) {
                        if (meta_len_rem < 0) {
                            int take = read_len - idx;
                            if (take > audio_rem) take = audio_rem;
                            if (take > 0) {
                                if (!push_audio_bytes(rx_buf + idx, (size_t)take, gen)) {
                                    break;
                                }
                                idx += take;
                                audio_rem -= take;
                            }
                            if (audio_rem == 0 && idx < read_len) {
                                uint8_t len_byte = rx_buf[idx++];
                                int mlen = (int)len_byte * 16;
                                if (mlen == 0) {
                                    audio_rem = metaint;
                                } else {
                                    meta_len_rem = mlen;
                                    meta_pos = 0;
                                }
                            }
                        } else {
                            int take = read_len - idx;
                            if (take > meta_len_rem) take = meta_len_rem;
                            int copy_max = (int)sizeof(meta_buf) - 1 - meta_pos;
                            if (copy_max > 0) {
                                int cp = take < copy_max ? take : copy_max;
                                memcpy(meta_buf + meta_pos, rx_buf + idx, (size_t)cp);
                                meta_pos += cp;
                            }
                            idx += take;
                            meta_len_rem -= take;
                            if (meta_len_rem == 0) {
                                meta_buf[meta_pos] = '\0';
                                parse_icy_metadata(meta_buf);
                                meta_len_rem = -1;
                                audio_rem = metaint;
                            }
                        }
                    }
                }

                update_status_locked_start();
                s_status.buffer_pct = get_ringbuf_pct();
                update_status_locked_end();
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            s_stream_eof = true;

            if (s_want_play && s_play_gen == gen) {
                set_status_msg(PLAYER_STATE_CONNECTING, "Reconnecting...");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}

static int mix_and_write_pcm(uint8_t *pcm_bytes, size_t byte_len, uint8_t channels)
{
    int16_t *samples = (int16_t *)pcm_bytes;
    size_t num_samples = byte_len / sizeof(int16_t);

    if (channels == 1) {
        /* Expand mono to stereo buffer */
        static int16_t stereo_tmp[4096];
        size_t offset = 0;
        while (offset < num_samples) {
            size_t chunk = num_samples - offset;
            if (chunk > 2048) chunk = 2048;
            for (size_t i = 0; i < chunk; i++) {
                int16_t s = samples[offset + i];
                stereo_tmp[i * 2] = s;
                stereo_tmp[i * 2 + 1] = s;
            }
            int wret = esp_codec_dev_write(s_codec_dev, stereo_tmp, (int)(chunk * 2 * sizeof(int16_t)));
            if (wret != ESP_CODEC_DEV_OK) return wret;
            offset += chunk;
        }
        return ESP_CODEC_DEV_OK;
    } else {
        /* Downmix L+R to mono across both L & R I2S slots so single built-in speaker never misses one channel */
        for (size_t i = 0; i + 1 < num_samples; i += 2) {
            int32_t avg = ((int32_t)samples[i] + (int32_t)samples[i + 1]) >> 1;
            samples[i] = (int16_t)avg;
            samples[i + 1] = (int16_t)avg;
        }
        return esp_codec_dev_write(s_codec_dev, pcm_bytes, (int)byte_len);
    }
}

static void decode_task(void *arg)
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();

    uint8_t *in_buf = heap_caps_malloc(DEC_IN_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!in_buf) in_buf = malloc(DEC_IN_BUF_SIZE);

    size_t out_buf_cap = DEC_OUT_BUF_SIZE;
    uint8_t *out_buf = heap_caps_malloc(out_buf_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) out_buf = malloc(out_buf_cap);

    while (1) {
        if (!s_want_play) {
            codec_set_active_mute(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t gen = s_play_gen;

        /* Wait for prebuffer */
        while (s_want_play && s_play_gen == gen) {
            UBaseType_t waiting = 0;
            vRingbufferGetInfo(s_ringbuf, NULL, NULL, NULL, NULL, &waiting);
            if (waiting >= AUDIO_PREBUFFER_BYTES || (s_stream_eof && waiting > 0)) {
                break;
            }
            update_status_locked_start();
            s_status.buffer_pct = get_ringbuf_pct();
            update_status_locked_end();
            vTaskDelay(pdMS_TO_TICKS(40));
        }

        if (!s_want_play || s_play_gen != gen) {
            continue;
        }

        stream_codec_t codec = s_detected_codec;
        esp_aac_dec_cfg_t aac_cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        aac_cfg.aac_plus_enable = true;

        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = (codec == CODEC_AAC) ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = (codec == CODEC_AAC) ? &aac_cfg : NULL,
            .cfg_size = (codec == CODEC_AAC) ? sizeof(aac_cfg) : 0,
            .use_frame_dec = false,
        };

        esp_audio_simple_dec_handle_t decoder = NULL;
        if (esp_audio_simple_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to open simple decoder");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool unmuted = false;
        size_t in_valid = 0;
        bool info_ready = false;
        esp_audio_simple_dec_info_t dec_info = {0};
        int err_streak = 0;

        while (s_want_play && s_play_gen == gen) {
            if (in_valid < DEC_IN_BUF_SIZE) {
                size_t got = 0;
                size_t space = DEC_IN_BUF_SIZE - in_valid;
                void *rb_data = xRingbufferReceiveUpTo(s_ringbuf, &got, pdMS_TO_TICKS(100), space);
                if (rb_data && got > 0) {
                    memcpy(in_buf + in_valid, rb_data, got);
                    vRingbufferReturnItem(s_ringbuf, rb_data);
                    in_valid += got;
                }
            }

            if (in_valid == 0) {
                if (s_stream_eof) break;
                set_status_msg(PLAYER_STATE_BUFFERING, "Buffering...");
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf,
                .len = (uint32_t)in_valid,
                .eos = false,
            };
            esp_audio_simple_dec_out_t out_frame = {
                .buffer = out_buf,
                .len = (uint32_t)out_buf_cap,
            };

            esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out_frame.needed_size > out_buf_cap) {
                uint8_t *new_buf = heap_caps_realloc(out_buf, out_frame.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (new_buf) {
                    out_buf = new_buf;
                    out_buf_cap = out_frame.needed_size;
                    continue;
                }
            }

            if (raw.consumed > 0 && raw.consumed <= in_valid) {
                size_t remain = in_valid - raw.consumed;
                if (remain > 0) {
                    memmove(in_buf, in_buf + raw.consumed, remain);
                }
                in_valid = remain;
            } else if (ret != ESP_AUDIO_ERR_OK) {
                /* Skip 1 byte to resync frame header if stuck on bad data */
                if (in_valid > 1) {
                    memmove(in_buf, in_buf + 1, in_valid - 1);
                    in_valid--;
                } else {
                    in_valid = 0;
                }
                err_streak++;
                if (err_streak > 256) {
                    ESP_LOGW(TAG, "Too many decode errors, resetting decoder");
                    esp_audio_simple_dec_reset(decoder);
                    err_streak = 0;
                }
                continue;
            }

            if (ret == ESP_AUDIO_ERR_OK && out_frame.decoded_size > 0) {
                err_streak = 0;
                if (!info_ready) {
                    if (esp_audio_simple_dec_get_info(decoder, &dec_info) == ESP_AUDIO_ERR_OK &&
                        dec_info.sample_rate > 0) {
                        info_ready = true;
                        codec_ensure_sample_rate(dec_info.sample_rate);
                        update_status_locked_start();
                        s_status.sample_rate = dec_info.sample_rate;
                        if (s_status.bitrate_kbps == 0 && dec_info.bitrate > 0) {
                            s_status.bitrate_kbps = dec_info.bitrate / 1000;
                        }
                        s_status.state = PLAYER_STATE_PLAYING;
                        snprintf(s_status.status_msg, sizeof(s_status.status_msg), "Playing");
                        update_status_locked_end();
                    }
                }

                bool first_log = false;
                if (!unmuted) {
                    update_status_locked_start();
                    bool user_muted = s_status.muted;
                    uint8_t cur_vol = s_status.volume;
                    update_status_locked_end();
                    codec_set_active_mute(user_muted);
                    ESP_LOGI(TAG, "Playback active: %lu Hz, %u ch, vol=%u, mute=%d",
                             (unsigned long)dec_info.sample_rate, (unsigned)dec_info.channel,
                             (unsigned)cur_vol, (int)user_muted);
                    unmuted = true;
                    first_log = true;
                }

                uint8_t ch = dec_info.channel ? dec_info.channel : 2;
                int wret = mix_and_write_pcm(out_frame.buffer, out_frame.decoded_size, ch);
                if (first_log || wret != ESP_CODEC_DEV_OK) {
                    ESP_LOGI(TAG, "Codec write ret=%d decoded=%lu", wret, (unsigned long)out_frame.decoded_size);
                }

                update_status_locked_start();
                s_status.buffer_pct = get_ringbuf_pct();
                if (s_status.state != PLAYER_STATE_PLAYING) {
                    s_status.state = PLAYER_STATE_PLAYING;
                    snprintf(s_status.status_msg, sizeof(s_status.status_msg), "Playing");
                }
                update_status_locked_end();
            } else if (ret == ESP_AUDIO_ERR_DATA_LACK || (ret == ESP_AUDIO_ERR_OK && raw.consumed == 0)) {
                /* Need more bytes in buffer */
                size_t got = 0;
                size_t space = DEC_IN_BUF_SIZE - in_valid;
                if (space == 0) {
                    in_valid = 0;
                } else {
                    void *rb_data = xRingbufferReceiveUpTo(s_ringbuf, &got, pdMS_TO_TICKS(50), space);
                    if (rb_data && got > 0) {
                        memcpy(in_buf + in_valid, rb_data, got);
                        vRingbufferReturnItem(s_ringbuf, rb_data);
                        in_valid += got;
                    }
                }
            }
        }

        codec_set_active_mute(true);
        esp_audio_simple_dec_close(decoder);
        flush_ringbuf();
    }
}

esp_err_t audio_player_init(i2c_master_bus_handle_t i2c_bus)
{
    s_status_mutex = xSemaphoreCreateMutex();
    s_codec_mutex = xSemaphoreCreateMutex();
    if (!s_status_mutex || !s_codec_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_ringbuf = xRingbufferCreateWithCaps(AUDIO_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_ringbuf) {
        ESP_LOGW(TAG, "PSRAM ringbuf failed, fallback to internal");
        s_ringbuf = xRingbufferCreate(64 * 1024, RINGBUF_TYPE_BYTEBUF);
    }
    if (!s_ringbuf) {
        return ESP_ERR_NO_MEM;
    }

    app_config_meta_t cfg;
    app_config_get_meta(&cfg);

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = PLAYER_STATE_STOPPED;
    s_status.station_index = cfg.current_station;
    s_status.volume = cfg.volume;
    s_status.muted = false;
    snprintf(s_status.status_msg, sizeof(s_status.status_msg), "Ready");
    radio_station_t st;
    if (app_config_get_station(cfg.current_station, &st)) {
        snprintf(s_status.station_name, sizeof(s_status.station_name), "%s", st.name);
    }

    ESP_ERROR_CHECK(init_i2s_and_codec(i2c_bus));

    xTaskCreatePinnedToCore(stream_task, "audio_stream", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(decode_task, "audio_decode", 8192, NULL, 6, NULL, 1);

    return ESP_OK;
}

esp_err_t audio_player_play_station(int index)
{
    int cnt = app_config_get_station_count();
    if (cnt <= 0) return ESP_ERR_INVALID_STATE;
    if (index < 0 || index >= cnt) index = 0;

    app_config_set_current_station((uint8_t)index);

    s_want_play = false;
    s_play_gen++;
    vTaskDelay(pdMS_TO_TICKS(60));
    flush_ringbuf();

    radio_station_t st;
    app_config_get_station(index, &st);

    update_status_locked_start();
    s_status.state = PLAYER_STATE_CONNECTING;
    s_status.station_index = index;
    snprintf(s_status.station_name, sizeof(s_status.station_name), "%s", st.name);
    clear_track_metadata_locked();
    s_status.buffer_pct = 0;
    snprintf(s_status.status_msg, sizeof(s_status.status_msg), "Connecting...");
    update_status_locked_end();

    s_target_station = index;
    s_want_play = true;
    return ESP_OK;
}

esp_err_t audio_player_stop(void)
{
    s_want_play = false;
    s_play_gen++;
    codec_set_active_mute(true);
    flush_ringbuf();

    update_status_locked_start();
    s_status.state = PLAYER_STATE_STOPPED;
    clear_track_metadata_locked();
    s_status.buffer_pct = 0;
    snprintf(s_status.status_msg, sizeof(s_status.status_msg), "Stopped");
    update_status_locked_end();
    return ESP_OK;
}

esp_err_t audio_player_toggle_play_pause(void)
{
    if (s_want_play) {
        return audio_player_stop();
    } else {
        app_config_meta_t cfg;
        app_config_get_meta(&cfg);
        return audio_player_play_station(cfg.current_station);
    }
}

esp_err_t audio_player_next(void)
{
    int cnt = app_config_get_station_count();
    if (cnt <= 0) return ESP_ERR_INVALID_STATE;
    update_status_locked_start();
    int cur = s_status.station_index;
    update_status_locked_end();
    return audio_player_play_station((cur + 1) % cnt);
}

esp_err_t audio_player_prev(void)
{
    int cnt = app_config_get_station_count();
    if (cnt <= 0) return ESP_ERR_INVALID_STATE;
    update_status_locked_start();
    int cur = s_status.station_index;
    update_status_locked_end();
    return audio_player_play_station((cur - 1 + cnt) % cnt);
}

esp_err_t audio_player_set_volume_live(uint8_t volume, bool save_nvs)
{
    if (volume > 100) volume = 100;
    if (save_nvs) {
        app_config_set_volume(volume);
    }

    update_status_locked_start();
    s_status.volume = volume;
    if (volume > 0) {
        s_status.muted = false;
    }
    bool muted = s_status.muted;
    update_status_locked_end();

    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    if (s_codec_is_open) {
        esp_codec_dev_set_out_vol(s_codec_dev, volume);
        if (s_want_play) {
            esp_codec_dev_set_out_mute(s_codec_dev, muted);
        }
    }
    xSemaphoreGive(s_codec_mutex);
    return ESP_OK;
}

esp_err_t audio_player_set_volume(uint8_t volume)
{
    return audio_player_set_volume_live(volume, true);
}

uint8_t audio_player_get_volume(void)
{
    update_status_locked_start();
    uint8_t v = s_status.volume;
    update_status_locked_end();
    return v;
}

esp_err_t audio_player_set_mute(bool mute)
{
    update_status_locked_start();
    s_status.muted = mute;
    update_status_locked_end();
    if (s_want_play) {
        codec_set_active_mute(mute);
    }
    return ESP_OK;
}

bool audio_player_is_muted(void)
{
    update_status_locked_start();
    bool m = s_status.muted;
    update_status_locked_end();
    return m;
}

void audio_player_get_status(player_status_t *out_status)
{
    if (!out_status) return;
    update_status_locked_start();
    memcpy(out_status, &s_status, sizeof(player_status_t));
    update_status_locked_end();
}

void audio_player_notify_stations_changed(void)
{
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);
    update_status_locked_start();
    if (s_status.station_index >= cfg.station_count) {
        s_status.station_index = cfg.current_station;
    }
    radio_station_t st;
    if (app_config_get_station(s_status.station_index, &st)) {
        snprintf(s_status.station_name, sizeof(s_status.station_name), "%s", st.name);
    }
    update_status_locked_end();
}
