#include "google_quota.h"
#include "app_config.h"
#include "wifi_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "google_quota";

#define GOOGLE_OAUTH_TOKEN_URL        "https://oauth2.googleapis.com/token"
#define GOOGLE_QUOTA_SUMMARY_ENDPOINT "https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary"
#define ANTIGRAVITY_USER_AGENT        "antigravity/cli/1.4.2 (aidev_client; os_type=linux; arch=amd64; cl=1001591392; auth_method=consumer)"
#define ANTIGRAVITY_PROJECT_ID        "aicode-consumers"

#define MAX_HTTP_RECV_BUFFER          16384
#define QUOTA_POLL_INTERVAL_MS        60000

static SemaphoreHandle_t s_quota_mutex = NULL;
static TaskHandle_t s_quota_task = NULL;
static google_quota_data_t s_quota_data = {0};

static volatile bool s_is_active = false;
static volatile bool s_force_token_refresh = false;
static volatile bool s_force_refresh = false;

static char s_access_token[1024] = {0};
static int64_t s_token_expires_at_uptime_sec = 0;

typedef struct {
    char *buffer;
    int buffer_len;
} http_response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (resp && resp->buffer) {
            if (resp->buffer_len + evt->data_len < MAX_HTTP_RECV_BUFFER - 1) {
                memcpy(resp->buffer + resp->buffer_len, evt->data, (size_t)evt->data_len);
                resp->buffer_len += evt->data_len;
                resp->buffer[resp->buffer_len] = '\0';
            }
        }
    }
    return ESP_OK;
}

static void url_encode(char *dst, size_t dst_len, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 4 < dst_len; s++) {
        unsigned char c = (unsigned char)src[s];
        if (isalnum((int)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[d++] = (char)c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[c >> 4];
            dst[d++] = hex[c & 0x0F];
        }
    }
    dst[d] = '\0';
}

/* Portable Howard Hinnant algorithm to convert UTC struct tm to Unix epoch time */
static time_t my_timegm(struct tm *tm)
{
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m - 3) + 2) / 5 + tm->tm_mday - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)(era * 146097 + (int)doe - 719468);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

static void parse_reset_time(const char *iso_time, quota_window_t *window, bool is_5h)
{
    if (!iso_time || strlen(iso_time) < 19) {
        snprintf(window->reset_time_str, sizeof(window->reset_time_str), "Active");
        return;
    }
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(iso_time, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) >= 5) {
        struct tm tm_utc = {0};
        tm_utc.tm_year = year - 1900;
        tm_utc.tm_mon = mon - 1;
        tm_utc.tm_mday = day;
        tm_utc.tm_hour = hour;
        tm_utc.tm_min = min;
        tm_utc.tm_sec = sec;
        time_t utc_epoch = my_timegm(&tm_utc);
        window->reset_epoch_sec = (int64_t)utc_epoch;

        struct tm tm_local = {0};
        localtime_r(&utc_epoch, &tm_local);

        if (is_5h) {
            snprintf(window->reset_time_str, sizeof(window->reset_time_str),
                     "Reset %02d:%02d", tm_local.tm_hour, tm_local.tm_min);
        } else {
            snprintf(window->reset_time_str, sizeof(window->reset_time_str),
                     "Reset %d/%d %02d:%02d", tm_local.tm_mon + 1, tm_local.tm_mday,
                     tm_local.tm_hour, tm_local.tm_min);
        }
    } else {
        snprintf(window->reset_time_str, sizeof(window->reset_time_str), "Active");
    }
}

static bool has_complete_oauth_creds(const app_agy_config_t *cfg)
{
    return cfg->oauth_client_id[0] != '\0' &&
           cfg->oauth_client_secret[0] != '\0' &&
           cfg->oauth_refresh_token[0] != '\0';
}

static bool refresh_oauth_access_token(const app_agy_config_t *agy_cfg)
{
    if (!has_complete_oauth_creds(agy_cfg)) {
        ESP_LOGW(TAG, "OAuth credentials incomplete in config");
        return false;
    }

    if (s_force_token_refresh) {
        s_force_token_refresh = false;
        s_access_token[0] = '\0';
        s_token_expires_at_uptime_sec = 0;
    }

    int64_t now_uptime = esp_timer_get_time() / 1000000LL;
    if (s_access_token[0] != '\0' && now_uptime < (s_token_expires_at_uptime_sec - 120)) {
        ESP_LOGD(TAG, "Reusing cached OAuth access token (valid for %lld more sec)",
                 (long long)(s_token_expires_at_uptime_sec - now_uptime));
        return true;
    }

    ESP_LOGI(TAG, "Refreshing Google OAuth Access Token via %s", GOOGLE_OAUTH_TOKEN_URL);

    char *resp_buf = calloc(1, MAX_HTTP_RECV_BUFFER);
    char *enc_cid = malloc(384);
    char *enc_sec = malloc(384);
    char *enc_tok = malloc(1536);
    char *post_data = malloc(2400);
    if (!resp_buf || !enc_cid || !enc_sec || !enc_tok || !post_data) {
        ESP_LOGE(TAG, "Failed to allocate OAuth request buffers");
        free(resp_buf);
        free(enc_cid);
        free(enc_sec);
        free(enc_tok);
        free(post_data);
        return false;
    }

    url_encode(enc_cid, 384, agy_cfg->oauth_client_id);
    url_encode(enc_sec, 384, agy_cfg->oauth_client_secret);
    url_encode(enc_tok, 1536, agy_cfg->oauth_refresh_token);

    int post_len = snprintf(post_data, 2400,
                            "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                            enc_cid, enc_sec, enc_tok);
    free(enc_cid);
    free(enc_sec);
    free(enc_tok);

    http_response_buffer_t resp = { .buffer = resp_buf, .buffer_len = 0 };

    esp_http_client_config_t config = {
        .url = GOOGLE_OAUTH_TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        free(resp_buf);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, post_len > 0 ? post_len : (int)strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    bool success = false;
    if (err == ESP_OK && status_code == 200 && resp.buffer_len > 0) {
        cJSON *root = cJSON_Parse(resp.buffer);
        if (root) {
            cJSON *tok_item = cJSON_GetObjectItem(root, "access_token");
            cJSON *exp_item = cJSON_GetObjectItem(root, "expires_in");
            if (cJSON_IsString(tok_item) && tok_item->valuestring[0] != '\0') {
                strncpy(s_access_token, tok_item->valuestring, sizeof(s_access_token) - 1);
                s_access_token[sizeof(s_access_token) - 1] = '\0';
                int exp_sec = cJSON_IsNumber(exp_item) ? exp_item->valueint : 3600;
                s_token_expires_at_uptime_sec = (esp_timer_get_time() / 1000000LL) + exp_sec;
                ESP_LOGI(TAG, "OAuth access token refreshed (valid for %d sec)", exp_sec);
                success = true;
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGE(TAG, "OAuth token refresh failed: err=%s, status=%d, resp=%.200s",
                 esp_err_to_name(err), status_code, resp.buffer);
    }

    free(resp_buf);
    return success;
}

static void update_window(quota_window_t *win, const char *display_name, float frac,
                          const char *reset_str, bool is_5h)
{
    win->valid = true;
    snprintf(win->name, sizeof(win->name), "%s", display_name);
    win->remaining_fraction = frac;
    parse_reset_time(reset_str, win, is_5h);
}

static bool fetch_cloudcode_quota_summary(google_quota_data_t *out_data)
{
    app_agy_config_t agy_cfg;
    app_config_get_agy(&agy_cfg);

    if (!refresh_oauth_access_token(&agy_cfg)) {
        out_data->has_error = true;
        if (!has_complete_oauth_creds(&agy_cfg)) {
            snprintf(out_data->error_msg, sizeof(out_data->error_msg), "Configure OAuth in Portal");
        } else {
            snprintf(out_data->error_msg, sizeof(out_data->error_msg), "OAuth Token Expired");
        }
        return false;
    }

    ESP_LOGI(TAG, "Fetching Antigravity User Quota Summary via %s", GOOGLE_QUOTA_SUMMARY_ENDPOINT);

    char *resp_buf = calloc(1, MAX_HTTP_RECV_BUFFER);
    char *auth_hdr = malloc(1040);
    if (!resp_buf || !auth_hdr) {
        ESP_LOGE(TAG, "Failed to allocate Quota request buffers");
        free(resp_buf);
        free(auth_hdr);
        out_data->has_error = true;
        snprintf(out_data->error_msg, sizeof(out_data->error_msg), "Out of Memory");
        return false;
    }

    http_response_buffer_t resp = { .buffer = resp_buf, .buffer_len = 0 };

    esp_http_client_config_t config = {
        .url = GOOGLE_QUOTA_SUMMARY_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(auth_hdr);
        free(resp_buf);
        out_data->has_error = true;
        snprintf(out_data->error_msg, sizeof(out_data->error_msg), "HTTP Init Failed");
        return false;
    }

    snprintf(auth_hdr, 1040, "Bearer %s", s_access_token);
    const char post_body[] = "{\"project\":\"" ANTIGRAVITY_PROJECT_ID "\"}";

    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", ANTIGRAVITY_USER_AGENT);
    esp_http_client_set_post_field(client, post_body, strlen(post_body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(auth_hdr);

    bool parsed_success = false;

    if (err == ESP_OK && status_code == 200 && resp.buffer_len > 0) {
        cJSON *root = cJSON_Parse(resp.buffer);
        if (root) {
            cJSON *groups = cJSON_GetObjectItem(root, "groups");
            if (groups && cJSON_IsArray(groups)) {
                int num_groups = cJSON_GetArraySize(groups);
                for (int g = 0; g < num_groups; g++) {
                    cJSON *group = cJSON_GetArrayItem(groups, g);
                    cJSON *grp_name = cJSON_GetObjectItem(group, "displayName");
                    const char *grp_str = cJSON_IsString(grp_name) ? grp_name->valuestring : "";
                    cJSON *buckets = cJSON_GetObjectItem(group, "buckets");
                    if (!buckets || !cJSON_IsArray(buckets)) continue;

                    int num_buckets = cJSON_GetArraySize(buckets);
                    for (int b = 0; b < num_buckets; b++) {
                        cJSON *bucket = cJSON_GetArrayItem(buckets, b);
                        cJSON *b_id = cJSON_GetObjectItem(bucket, "bucketId");
                        cJSON *b_name = cJSON_GetObjectItem(bucket, "displayName");
                        cJSON *b_frac = cJSON_GetObjectItem(bucket, "remainingFraction");
                        cJSON *b_reset = cJSON_GetObjectItem(bucket, "resetTime");

                        const char *id_str = cJSON_IsString(b_id) ? b_id->valuestring : "";
                        const char *name_str = cJSON_IsString(b_name) ? b_name->valuestring : "";
                        float frac = cJSON_IsNumber(b_frac) ? (float)b_frac->valuedouble : 0.0f;
                        if (frac < 0.0f) frac = 0.0f;
                        if (frac > 1.0f) frac = 1.0f;
                        const char *reset_str = cJSON_IsString(b_reset) ? b_reset->valuestring : "";

                        if (strstr(grp_str, "Gemini Models") != NULL || strcmp(grp_str, "Gemini Models") == 0) {
                            if (strcmp(id_str, "gemini-5h") == 0 || strstr(name_str, "Five Hour") != NULL) {
                                update_window(&out_data->limit_5h, "Gemini 5h", frac, reset_str, true);
                                parsed_success = true;
                                ESP_LOGI(TAG, "Parsed Gemini 5h: %.1f%%, %s",
                                         frac * 100.0f, out_data->limit_5h.reset_time_str);
                            } else if (strcmp(id_str, "gemini-weekly") == 0 || strstr(name_str, "Weekly") != NULL) {
                                update_window(&out_data->limit_weekly, "Gemini Wk", frac, reset_str, false);
                                parsed_success = true;
                                ESP_LOGI(TAG, "Parsed Gemini Weekly: %.1f%%, %s",
                                         frac * 100.0f, out_data->limit_weekly.reset_time_str);
                            }
                        } else if (strstr(grp_str, "Claude") != NULL) {
                            if (strcmp(id_str, "3p-5h") == 0 || strstr(name_str, "Five Hour") != NULL) {
                                update_window(&out_data->limit_claude_5h, "Claude 5h", frac, reset_str, true);
                                parsed_success = true;
                                ESP_LOGI(TAG, "Parsed Claude 5h: %.1f%%, %s",
                                         frac * 100.0f, out_data->limit_claude_5h.reset_time_str);
                            } else if (strcmp(id_str, "3p-weekly") == 0 || strstr(name_str, "Weekly") != NULL) {
                                update_window(&out_data->limit_claude_wk, "Claude Wk", frac, reset_str, false);
                                parsed_success = true;
                                ESP_LOGI(TAG, "Parsed Claude Weekly: %.1f%%, %s",
                                         frac * 100.0f, out_data->limit_claude_wk.reset_time_str);
                            }
                        } else if (strstr(grp_str, "Argon") != NULL || strcmp(grp_str, "Gemini 4 Argon") == 0) {
                            if (strcmp(id_str, "gemini-argon-wtu-weekly") == 0 ||
                                strstr(name_str, "Argon") != NULL ||
                                strstr(name_str, "Weekly") != NULL) {
                                update_window(&out_data->limit_argon, "Argon Wk", frac, reset_str, false);
                                parsed_success = true;
                                ESP_LOGI(TAG, "Parsed Gemini 4 Argon: %.1f%%, %s",
                                         frac * 100.0f, out_data->limit_argon.reset_time_str);
                            }
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }

        if (parsed_success) {
            out_data->has_error = false;
            out_data->error_msg[0] = '\0';
            out_data->last_update_sec = (int64_t)time(NULL);
        } else {
            out_data->has_error = true;
            snprintf(out_data->error_msg, sizeof(out_data->error_msg), "No Quota Data");
        }
    } else {
        ESP_LOGE(TAG, "CloudCode Quota API failed: err=%s, status=%d, resp=%.200s",
                 esp_err_to_name(err), status_code, resp.buffer);
        out_data->has_error = true;
        if (status_code == 401 || status_code == 403) {
            snprintf(out_data->error_msg, sizeof(out_data->error_msg), "Auth Error / Check Token");
            s_access_token[0] = '\0';
            s_token_expires_at_uptime_sec = 0;
        } else if (status_code > 0) {
            snprintf(out_data->error_msg, sizeof(out_data->error_msg), "HTTP %d Error", status_code);
        } else {
            snprintf(out_data->error_msg, sizeof(out_data->error_msg), "Net Error (%s)", esp_err_to_name(err));
        }
    }

    free(resp_buf);
    return parsed_success;
}

static void quota_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Quota polling task started on Core %d", xPortGetCoreID());

    while (1) {
        if (!s_is_active && !s_force_refresh) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        s_force_refresh = false;

        wifi_mgr_status_t wst;
        wifi_manager_get_status(&wst);

        if (wst.state == WIFI_MGR_CONNECTED_STA) {
            google_quota_data_t next_data;
            if (s_quota_mutex) xSemaphoreTake(s_quota_mutex, portMAX_DELAY);
            s_quota_data.is_loading = true;
            next_data = s_quota_data;
            if (s_quota_mutex) xSemaphoreGive(s_quota_mutex);

            fetch_cloudcode_quota_summary(&next_data);
            next_data.is_loading = false;

            if (s_quota_mutex) xSemaphoreTake(s_quota_mutex, portMAX_DELAY);
            s_quota_data = next_data;
            if (s_quota_mutex) xSemaphoreGive(s_quota_mutex);
        } else {
            if (s_quota_mutex) xSemaphoreTake(s_quota_mutex, portMAX_DELAY);
            s_quota_data.is_loading = false;
            s_quota_data.has_error = true;
            snprintf(s_quota_data.error_msg, sizeof(s_quota_data.error_msg), "Wi-Fi Disconnected");
            if (s_quota_mutex) xSemaphoreGive(s_quota_mutex);
        }

        if (s_is_active) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(QUOTA_POLL_INTERVAL_MS));
        } else if (!s_force_refresh) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

static void init_default_window(quota_window_t *win, const char *name)
{
    win->valid = false;
    snprintf(win->name, sizeof(win->name), "%s", name);
    win->remaining_fraction = 1.0f;
    win->reset_epoch_sec = 0;
    snprintf(win->reset_time_str, sizeof(win->reset_time_str), "Waiting data...");
}

void google_quota_init(void)
{
    if (s_quota_task != NULL) return;

    s_quota_mutex = xSemaphoreCreateMutex();
    s_is_active = false;
    s_force_token_refresh = false;
    s_force_refresh = false;

    memset(&s_quota_data, 0, sizeof(s_quota_data));
    init_default_window(&s_quota_data.limit_5h, "Gemini 5h");
    init_default_window(&s_quota_data.limit_weekly, "Gemini Wk");
    init_default_window(&s_quota_data.limit_claude_5h, "Claude 5h");
    init_default_window(&s_quota_data.limit_claude_wk, "Claude Wk");
    init_default_window(&s_quota_data.limit_argon, "Argon Wk");

    BaseType_t ret = xTaskCreatePinnedToCore(
        quota_task, "quota_task", 8192, NULL, 2, &s_quota_task, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create quota_task");
        s_quota_task = NULL;
    }
}

void google_quota_set_active(bool active)
{
    bool prev = s_is_active;
    s_is_active = active;
    if (prev != active) {
        ESP_LOGI(TAG, "Quota polling active state changed: %s -> %s",
                 prev ? "ACTIVE" : "PAUSED", active ? "ACTIVE" : "PAUSED");
    }
    if (active && !prev && s_quota_task) {
        xTaskNotifyGive(s_quota_task);
    }
}

void google_quota_request_refresh(void)
{
    s_force_token_refresh = true;
    s_force_refresh = true;
    if (s_quota_task) {
        xTaskNotifyGive(s_quota_task);
    }
}

void google_quota_get_data(google_quota_data_t *out_data)
{
    if (!out_data) return;
    if (s_quota_mutex && xSemaphoreTake(s_quota_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(out_data, &s_quota_data, sizeof(google_quota_data_t));
        xSemaphoreGive(s_quota_mutex);
    } else {
        memcpy(out_data, &s_quota_data, sizeof(google_quota_data_t));
    }
}
