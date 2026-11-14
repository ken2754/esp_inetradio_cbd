#include "album_art.h"
#include "wifi_manager.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"

static const char *TAG = "album_art";

#define ART_HTTP_MAX_JSON_BYTES (16 * 1024)
#define ART_HTTP_MAX_IMG_BYTES  (96 * 1024)
#define TJPGD_WORK_BUF_SIZE     4096

static SemaphoreHandle_t s_art_mutex = NULL;
static TaskHandle_t s_art_task = NULL;

static uint16_t *s_active_pixels = NULL;
static uint16_t *s_scratch_pixels = NULL;
static bool s_active_is_cover = false;
static uint32_t s_art_version = 0;

/* Pending request state guarded by s_art_mutex */
static int s_req_station = 0;
static char s_req_artist[96] = {0};
static char s_req_track[128] = {0};
static uint32_t s_req_gen = 0;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    bool overflow;
} http_buf_t;

typedef struct {
    const uint8_t *jpeg;
    size_t size;
    size_t pos;
    uint16_t *out_buf;
    uint16_t out_w;
    uint16_t out_h;
} jpeg_ctx_t;

static void *art_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(bytes);
    }
    return p;
}

static inline uint16_t rgb565_make(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

/* Render a clean dark vinyl/gradient card tinted subtly by station index */
static void render_fallback_art(uint16_t *dst, int station_index)
{
    static const uint8_t palette[][3] = {
        { 56, 189, 248 }, /* Sky */
        { 168, 85, 247 }, /* Purple */
        { 34, 197,  94 }, /* Emerald */
        { 249, 115, 22 }, /* Orange */
        { 236,  72, 153}, /* Pink */
        { 234, 179,   8}, /* Amber */
        { 20, 184, 166 }, /* Teal */
        { 99, 102, 241 }, /* Indigo */
    };
    const int n_pal = (int)(sizeof(palette) / sizeof(palette[0]));
    int pi = (station_index < 0 ? 0 : station_index) % n_pal;
    uint8_t ar = palette[pi][0];
    uint8_t ag = palette[pi][1];
    uint8_t ab = palette[pi][2];

    const int cx = ALBUM_ART_WIDTH / 2;
    const int cy = ALBUM_ART_HEIGHT / 2;

    for (int y = 0; y < ALBUM_ART_HEIGHT; y++) {
        for (int x = 0; x < ALBUM_ART_WIDTH; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;

            /* Base diagonal dark slate gradient with subtle accent tint */
            int mix = (x + y) * 32 / (ALBUM_ART_WIDTH + ALBUM_ART_HEIGHT);
            uint8_t r = (uint8_t)(16 + (ar * mix) / 255);
            uint8_t g = (uint8_t)(23 + (ag * mix) / 255);
            uint8_t b = (uint8_t)(40 + (ab * mix) / 255);

            if (d2 <= 46 * 46) {
                /* Vinyl disc body */
                if (d2 <= 6 * 6) {
                    /* Spindle hole */
                    r = 12; g = 16; b = 26;
                } else if (d2 <= 18 * 18) {
                    /* Colored center label */
                    r = ar; g = ag; b = ab;
                } else {
                    /* Subtle concentric vinyl grooves */
                    int rad = (int)sqrtf((float)d2);
                    bool groove = ((rad % 4) == 0);
                    r = groove ? 26 : 14;
                    g = groove ? 32 : 18;
                    b = groove ? 44 : 26;
                    /* Glossy diagonal reflection */
                    if ((dx * dy) > 0 && abs(dx - dy) < 18) {
                        r = (uint8_t)(r + 14);
                        g = (uint8_t)(g + 16);
                        b = (uint8_t)(b + 20);
                    }
                }
            }
            dst[y * ALBUM_ART_WIDTH + x] = rgb565_make(r, g, b);
        }
    }
}

static bool is_non_song_metadata(const char *artist, const char *track)
{
    if ((!artist || !artist[0]) && (!track || !track[0])) {
        return true;
    }
    const char *a = artist ? artist : "";
    const char *t = track ? track : "";
    if (!a[0] && strlen(t) < 3) return true;

    static const char *skip_words[] = {
        "somafm", "soma.fm", "commercial-free", "listener-supported",
        "listener supported", "big sonic", "now playing", "station id",
    };
    for (size_t i = 0; i < sizeof(skip_words) / sizeof(skip_words[0]); i++) {
        if (strcasestr(a, skip_words[i]) || strcasestr(t, skip_words[i])) {
            return true;
        }
    }
    return false;
}

static void url_encode(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 4 < dst_len; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else {
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0x0F];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    http_buf_t *hb = (http_buf_t *)evt->user_data;
    if (!hb) return ESP_OK;

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (hb->len + (size_t)evt->data_len >= hb->cap) {
            hb->overflow = true;
            return ESP_FAIL;
        }
        memcpy(hb->buf + hb->len, evt->data, (size_t)evt->data_len);
        hb->len += (size_t)evt->data_len;
        hb->buf[hb->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_get_to_buf(const char *url, uint8_t *buf, size_t cap, size_t *out_len)
{
    http_buf_t hb = {
        .buf = buf,
        .len = 0,
        .cap = cap,
        .overflow = false,
    };
    buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_cb,
        .user_data = &hb,
        .timeout_ms = 6000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "ES3C28P-Radio/1.0",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || hb.overflow || status != 200 || hb.len == 0) {
        ESP_LOGW(TAG, "HTTP GET failed: err=%s status=%d len=%u overflow=%d",
                 esp_err_to_name(err), status, (unsigned)hb.len, hb.overflow);
        return ESP_FAIL;
    }

    *out_len = hb.len;
    return ESP_OK;
}

static UINT tjpgd_in_func(JDEC *jd, BYTE *buff, UINT nbyte)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    size_t avail = ctx->size - ctx->pos;
    if (nbyte > avail) nbyte = (UINT)avail;
    if (buff) {
        memcpy(buff, ctx->jpeg + ctx->pos, nbyte);
    }
    ctx->pos += nbyte;
    return nbyte;
}

static UINT tjpgd_out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    const uint8_t *rgb = (const uint8_t *)bitmap;
    uint16_t rw = (uint16_t)(rect->right - rect->left + 1);
    uint16_t rh = (uint16_t)(rect->bottom - rect->top + 1);

    for (uint16_t y = 0; y < rh; y++) {
        uint16_t dy = (uint16_t)(rect->top + y);
        if (dy >= ctx->out_h) continue;
        for (uint16_t x = 0; x < rw; x++) {
            uint16_t dx = (uint16_t)(rect->left + x);
            const uint8_t *p = rgb + ((size_t)y * rw + x) * 3;
            if (dx < ctx->out_w) {
                ctx->out_buf[(size_t)dy * ctx->out_w + dx] = rgb565_make(p[0], p[1], p[2]);
            }
        }
    }
    return 1;
}

static void resize_rgb565_bilinear_or_nn(const uint16_t *src, uint16_t sw, uint16_t sh,
                                         uint16_t *dst, uint16_t dw, uint16_t dh)
{
    if (sw == dw && sh == dh) {
        memcpy(dst, src, (size_t)dw * dh * sizeof(uint16_t));
        return;
    }
    for (uint16_t y = 0; y < dh; y++) {
        uint16_t sy = (uint16_t)(((uint32_t)y * sh) / dh);
        if (sy >= sh) sy = sh - 1;
        for (uint16_t x = 0; x < dw; x++) {
            uint16_t sx = (uint16_t)(((uint32_t)x * sw) / dw);
            if (sx >= sw) sx = sw - 1;
            dst[(size_t)y * dw + x] = src[(size_t)sy * sw + sx];
        }
    }
}

static bool decode_jpeg_to_rgb565(const uint8_t *jpeg_data, size_t jpeg_size, uint16_t *dst_128x128)
{
    if (!jpeg_data || jpeg_size < 4 || jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        return false;
    }

    void *work = art_alloc(TJPGD_WORK_BUF_SIZE);
    if (!work) return false;

    jpeg_ctx_t ctx = {
        .jpeg = jpeg_data,
        .size = jpeg_size,
        .pos = 0,
        .out_buf = NULL,
        .out_w = 0,
        .out_h = 0,
    };

    JDEC jdec;
    JRESULT res = jd_prepare(&jdec, tjpgd_in_func, work, TJPGD_WORK_BUF_SIZE, &ctx);
    if (res != JDR_OK || jdec.width == 0 || jdec.height == 0 || jdec.width > 1024 || jdec.height > 1024) {
        ESP_LOGW(TAG, "jd_prepare failed: res=%d (%ux%u)", res, jdec.width, jdec.height);
        free(work);
        return false;
    }

    BYTE scale = 0;
    while (scale < 3 && ((jdec.width >> (scale + 1)) >= ALBUM_ART_WIDTH) &&
           ((jdec.height >> (scale + 1)) >= ALBUM_ART_HEIGHT)) {
        scale++;
    }

    ctx.out_w = (uint16_t)(jdec.width >> scale);
    ctx.out_h = (uint16_t)(jdec.height >> scale);
    if (ctx.out_w == 0 || ctx.out_h == 0) {
        free(work);
        return false;
    }

    uint16_t *decoded = art_alloc((size_t)ctx.out_w * ctx.out_h * sizeof(uint16_t));
    if (!decoded) {
        free(work);
        return false;
    }
    ctx.out_buf = decoded;

    res = jd_decomp(&jdec, tjpgd_out_func, scale);
    if (res == JDR_OK) {
        resize_rgb565_bilinear_or_nn(decoded, ctx.out_w, ctx.out_h,
                                     dst_128x128, ALBUM_ART_WIDTH, ALBUM_ART_HEIGHT);
        ESP_LOGI(TAG, "Decoded JPEG %ux%u (scale=%u -> %ux%u)",
                 jdec.width, jdec.height, scale, ctx.out_w, ctx.out_h);
    } else {
        ESP_LOGW(TAG, "jd_decomp failed: res=%d", res);
    }

    free(decoded);
    free(work);
    return (res == JDR_OK);
}

static bool fetch_itunes_art_url(const char *artist, const char *track, char *out_url, size_t out_len)
{
    char query[256];
    if (artist[0] && track[0]) {
        snprintf(query, sizeof(query), "%s %s", artist, track);
    } else if (track[0]) {
        snprintf(query, sizeof(query), "%s", track);
    } else {
        snprintf(query, sizeof(query), "%s", artist);
    }

    char encoded[512];
    url_encode(query, encoded, sizeof(encoded));

    char search_url[600];
    snprintf(search_url, sizeof(search_url),
             "https://itunes.apple.com/search?term=%s&entity=song&limit=1", encoded);

    uint8_t *json_buf = art_alloc(ART_HTTP_MAX_JSON_BYTES);
    if (!json_buf) return false;

    size_t json_len = 0;
    esp_err_t err = http_get_to_buf(search_url, json_buf, ART_HTTP_MAX_JSON_BYTES, &json_len);
    if (err != ESP_OK) {
        free(json_buf);
        return false;
    }

    bool found = false;
    cJSON *root = cJSON_Parse((const char *)json_buf);
    free(json_buf);
    if (!root) return false;

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        cJSON *art = cJSON_GetObjectItem(first, "artworkUrl100");
        if (cJSON_IsString(art) && art->valuestring && strlen(art->valuestring) < out_len) {
            snprintf(out_url, out_len, "%s", art->valuestring);
            char *dim = strstr(out_url, "100x100bb");
            if (dim) {
                memcpy(dim, "128x128bb", 9);
            }
            found = true;
        }
    }
    cJSON_Delete(root);
    return found;
}

static void art_task(void *arg)
{
    (void)arg;
    uint32_t last_processed_gen = 0;

    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        xSemaphoreTake(s_art_mutex, portMAX_DELAY);
        uint32_t gen = s_req_gen;
        int st_idx = s_req_station;
        char artist[96];
        char track[128];
        snprintf(artist, sizeof(artist), "%s", s_req_artist);
        snprintf(track, sizeof(track), "%s", s_req_track);
        xSemaphoreGive(s_art_mutex);

        if (gen == 0 || gen == last_processed_gen) {
            continue;
        }

        if (is_non_song_metadata(artist, track)) {
            render_fallback_art(s_scratch_pixels, st_idx);
            xSemaphoreTake(s_art_mutex, portMAX_DELAY);
            if (gen == s_req_gen) {
                memcpy(s_active_pixels, s_scratch_pixels, ALBUM_ART_BYTES);
                s_active_is_cover = false;
                s_art_version++;
            }
            xSemaphoreGive(s_art_mutex);
            last_processed_gen = gen;
            continue;
        }

        /* Wait until STA Wi-Fi is connected */
        wifi_mgr_status_t wst;
        wifi_manager_get_status(&wst);
        if (wst.state != WIFI_MGR_CONNECTED_STA) {
            continue;
        }

        /* Show station fallback immediately while fetching new cover */
        render_fallback_art(s_scratch_pixels, st_idx);
        xSemaphoreTake(s_art_mutex, portMAX_DELAY);
        if (gen == s_req_gen) {
            memcpy(s_active_pixels, s_scratch_pixels, ALBUM_ART_BYTES);
            s_active_is_cover = false;
            s_art_version++;
        }
        xSemaphoreGive(s_art_mutex);

        last_processed_gen = gen;

        char img_url[256] = {0};
        if (!fetch_itunes_art_url(artist, track, img_url, sizeof(img_url))) {
            ESP_LOGI(TAG, "No iTunes artwork for '%s - %s'", artist, track);
            continue;
        }

        /* Abort stale fetch if track changed already */
        xSemaphoreTake(s_art_mutex, portMAX_DELAY);
        bool stale = (gen != s_req_gen);
        xSemaphoreGive(s_art_mutex);
        if (stale) continue;

        uint8_t *img_buf = art_alloc(ART_HTTP_MAX_IMG_BYTES);
        if (!img_buf) continue;

        size_t img_len = 0;
        esp_err_t dl_err = http_get_to_buf(img_url, img_buf, ART_HTTP_MAX_IMG_BYTES, &img_len);
        bool ok = false;
        if (dl_err == ESP_OK) {
            ok = decode_jpeg_to_rgb565(img_buf, img_len, s_scratch_pixels);
        }
        free(img_buf);

        if (ok) {
            xSemaphoreTake(s_art_mutex, portMAX_DELAY);
            if (gen == s_req_gen) {
                memcpy(s_active_pixels, s_scratch_pixels, ALBUM_ART_BYTES);
                s_active_is_cover = true;
                s_art_version++;
                ESP_LOGI(TAG, "Artwork updated for '%s - %s'", artist, track);
            }
            xSemaphoreGive(s_art_mutex);
        }
    }
}

esp_err_t album_art_init(void)
{
    s_art_mutex = xSemaphoreCreateMutex();
    s_active_pixels = art_alloc(ALBUM_ART_BYTES);
    s_scratch_pixels = art_alloc(ALBUM_ART_BYTES);
    if (!s_art_mutex || !s_active_pixels || !s_scratch_pixels) {
        return ESP_ERR_NO_MEM;
    }

    render_fallback_art(s_active_pixels, 0);
    s_active_is_cover = false;
    s_art_version = 1;

    xTaskCreatePinnedToCore(art_task, "album_art", 8192, NULL, 3, &s_art_task, 0);
    return ESP_OK;
}

void album_art_request(int station_index, const char *artist, const char *track)
{
    if (!s_art_mutex) return;
    const char *a = artist ? artist : "";
    const char *t = track ? track : "";

    xSemaphoreTake(s_art_mutex, portMAX_DELAY);
    bool same = (s_req_station == station_index &&
                 strcmp(s_req_artist, a) == 0 &&
                 strcmp(s_req_track, t) == 0);
    if (!same) {
        s_req_station = station_index;
        snprintf(s_req_artist, sizeof(s_req_artist), "%s", a);
        snprintf(s_req_track, sizeof(s_req_track), "%s", t);
        s_req_gen++;
    }
    xSemaphoreGive(s_art_mutex);

    if (!same && s_art_task) {
        xTaskNotifyGive(s_art_task);
    }
}

void album_art_clear(int station_index)
{
    album_art_request(station_index, "", "");
}

uint32_t album_art_get_version(void)
{
    if (!s_art_mutex) return 0;
    xSemaphoreTake(s_art_mutex, portMAX_DELAY);
    uint32_t v = s_art_version;
    xSemaphoreGive(s_art_mutex);
    return v;
}

bool album_art_copy_rgb565(uint16_t *dst_pixels, size_t dst_bytes, bool *out_is_cover, uint32_t *out_version)
{
    if (!s_art_mutex || !dst_pixels || dst_bytes < ALBUM_ART_BYTES) return false;
    xSemaphoreTake(s_art_mutex, portMAX_DELAY);
    memcpy(dst_pixels, s_active_pixels, ALBUM_ART_BYTES);
    if (out_is_cover) *out_is_cover = s_active_is_cover;
    if (out_version) *out_version = s_art_version;
    xSemaphoreGive(s_art_mutex);
    return true;
}
