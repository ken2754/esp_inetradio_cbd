#include "web_server.h"
#include "app_config.h"
#include "audio_player.h"
#include "display_ui.h"
#include "google_quota.h"
#include "sntp_time.h"
#include "web_ui.h"
#include "wifi_manager.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "web_srv";

static httpd_handle_t s_server = NULL;

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    free(json_str);
    return ret;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static cJSON *recv_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        return NULL;
    }
    char *buf = malloc((size_t)total + 1);
    if (!buf) return NULL;

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[total] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

static esp_err_t handle_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

static const char *state_to_str(player_state_t st)
{
    switch (st) {
    case PLAYER_STATE_CONNECTING: return "CONNECTING";
    case PLAYER_STATE_BUFFERING:  return "BUFFERING";
    case PLAYER_STATE_PLAYING:    return "PLAYING";
    case PLAYER_STATE_ERROR:      return "ERROR";
    default:                      return "STOPPED";
    }
}

static esp_err_t handle_status_get(httpd_req_t *req)
{
    player_status_t pst;
    audio_player_get_status(&pst);
    wifi_mgr_status_t wst;
    wifi_manager_get_status(&wst);
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);
    clock_display_time_t clk;
    sntp_time_get_current(&clk);
    const char *view_name = display_ui_get_view_name();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state_text", state_to_str(pst.state));
    cJSON_AddNumberToObject(root, "station_index", pst.station_index);
    cJSON_AddStringToObject(root, "station_name", pst.station_name);
    cJSON_AddStringToObject(root, "stream_title", pst.stream_title);
    cJSON_AddStringToObject(root, "stream_artist", pst.stream_artist);
    cJSON_AddStringToObject(root, "stream_track", pst.stream_track);
    cJSON_AddStringToObject(root, "codec", pst.codec_name);
    cJSON_AddStringToObject(root, "status_msg", pst.status_msg);
    cJSON_AddNumberToObject(root, "sample_rate", pst.sample_rate);
    cJSON_AddNumberToObject(root, "bitrate_kbps", pst.bitrate_kbps);
    cJSON_AddNumberToObject(root, "volume", pst.volume);
    cJSON_AddBoolToObject(root, "muted", pst.muted);
    cJSON_AddNumberToObject(root, "buffer_pct", pst.buffer_pct);

    const char *wstate = "CONNECTING";
    if (wst.state == WIFI_MGR_CONNECTED_STA) wstate = "STA";
    else if (wst.state == WIFI_MGR_AP_CONFIG_MODE) wstate = "AP";
    cJSON_AddStringToObject(root, "wifi_state", wstate);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddStringToObject(root, "ip_addr", wst.ip_addr);
    cJSON_AddStringToObject(root, "ap_ssid", wst.ap_ssid);
    cJSON_AddStringToObject(root, "ap_ip", wst.ap_ip);
    cJSON_AddNumberToObject(root, "rssi", wst.rssi);

    cJSON_AddNumberToObject(root, "brightness", cfg.brightness);
    cJSON_AddBoolToObject(root, "autoplay", cfg.autoplay);
    cJSON_AddBoolToObject(root, "invert_color", cfg.invert_color);
    cJSON_AddBoolToObject(root, "flip_180", cfg.flip_180);
    cJSON_AddBoolToObject(root, "swap_rb", cfg.swap_rb);

    cJSON_AddStringToObject(root, "view", view_name ? view_name : "player");
    cJSON_AddStringToObject(root, "time_str", clk.time_str);
    cJSON_AddBoolToObject(root, "time_synced", clk.is_synced);

    return send_json(req, root);
}

static esp_err_t handle_control_post(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *act = cJSON_GetObjectItem(body, "action");
    if (cJSON_IsString(act)) {
        const char *a = act->valuestring;
        if (strcmp(a, "play") == 0) {
            cJSON *st = cJSON_GetObjectItem(body, "station");
            int idx = cJSON_IsNumber(st) ? st->valueint : -1;
            if (idx >= 0) {
                audio_player_play_station(idx);
            } else {
                app_config_meta_t cfg;
                app_config_get_meta(&cfg);
                audio_player_play_station(cfg.current_station);
            }
        } else if (strcmp(a, "stop") == 0) {
            audio_player_stop();
        } else if (strcmp(a, "toggle") == 0) {
            audio_player_toggle_play_pause();
        } else if (strcmp(a, "next") == 0) {
            audio_player_next();
        } else if (strcmp(a, "prev") == 0) {
            audio_player_prev();
        } else if (strcmp(a, "mute") == 0) {
            cJSON *m = cJSON_GetObjectItem(body, "muted");
            audio_player_set_mute(cJSON_IsBool(m) ? cJSON_IsTrue(m) : !audio_player_is_muted());
        } else if (strcmp(a, "refresh_quota") == 0) {
            google_quota_request_refresh();
        }
    }

    cJSON *vw = cJSON_GetObjectItem(body, "view");
    if (cJSON_IsString(vw) && vw->valuestring[0] != '\0') {
        display_ui_switch_view_by_name(vw->valuestring);
    }

    cJSON *vol = cJSON_GetObjectItem(body, "volume");
    if (cJSON_IsNumber(vol)) {
        int v = vol->valueint;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        audio_player_set_volume((uint8_t)v);
    }

    cJSON_Delete(body);
    return send_ok(req);
}

static esp_err_t handle_stations_get(httpd_req_t *req)
{
    int count = app_config_get_station_count();
    radio_station_t st;

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (!app_config_get_station(i, &st)) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", st.name);
        cJSON_AddStringToObject(item, "url", st.url);
        cJSON_AddStringToObject(item, "genre", st.genre);
        cJSON_AddItemToArray(arr, item);
    }
    return send_json(req, arr);
}

static esp_err_t handle_stations_post(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *name = cJSON_GetObjectItem(body, "name");
    cJSON *url = cJSON_GetObjectItem(body, "url");
    cJSON *genre = cJSON_GetObjectItem(body, "genre");

    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(name) && cJSON_IsString(url)) {
        err = app_config_add_station(name->valuestring, url->valuestring,
                                     cJSON_IsString(genre) ? genre->valuestring : "");
    }
    cJSON_Delete(body);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot add station");
        return ESP_FAIL;
    }

    audio_player_notify_stations_changed();
    display_ui_refresh_stations();
    return send_ok(req);
}

static esp_err_t handle_stations_put(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    cJSON *move_from = cJSON_GetObjectItem(body, "move_from");
    cJSON *move_to = cJSON_GetObjectItem(body, "move_to");

    if (cJSON_IsNumber(move_from) && cJSON_IsNumber(move_to)) {
        err = app_config_move_station(move_from->valueint, move_to->valueint);
    } else {
        cJSON *idx = cJSON_GetObjectItem(body, "index");
        cJSON *name = cJSON_GetObjectItem(body, "name");
        cJSON *url = cJSON_GetObjectItem(body, "url");
        cJSON *genre = cJSON_GetObjectItem(body, "genre");
        if (cJSON_IsNumber(idx) && cJSON_IsString(name) && cJSON_IsString(url)) {
            err = app_config_update_station(idx->valueint, name->valuestring, url->valuestring,
                                            cJSON_IsString(genre) ? genre->valuestring : "");
        }
    }
    cJSON_Delete(body);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot update station");
        return ESP_FAIL;
    }

    audio_player_notify_stations_changed();
    display_ui_refresh_stations();
    return send_ok(req);
}

static esp_err_t handle_stations_delete(httpd_req_t *req)
{
    char query[64] = {0};
    char idx_str[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "index", idx_str, sizeof(idx_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing index");
        return ESP_FAIL;
    }

    int idx = atoi(idx_str);
    if (app_config_delete_station(idx) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Delete failed");
        return ESP_FAIL;
    }

    audio_player_notify_stations_changed();
    display_ui_refresh_stations();
    return send_ok(req);
}

static esp_err_t handle_stations_reset_post(httpd_req_t *req)
{
    app_config_reset_default_stations();
    audio_player_notify_stations_changed();
    display_ui_refresh_stations();
    return send_ok(req);
}

static esp_err_t handle_wifi_scan_get(httpd_req_t *req)
{
    wifi_scan_entry_t list[16];
    int count = wifi_manager_scan(list, 16);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi", list[i].rssi);
        cJSON_AddBoolToObject(obj, "secure", list[i].secure);
        cJSON_AddItemToArray(arr, obj);
    }
    return send_json(req, arr);
}

static esp_err_t handle_wifi_post(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(body, "ssid");
    cJSON *pass = cJSON_GetObjectItem(body, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    esp_err_t err = app_config_set_wifi(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(body);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Wi-Fi config");
        return ESP_FAIL;
    }

    send_ok(req);
    wifi_manager_connect_sta_async();
    return ESP_OK;
}

static esp_err_t handle_display_post(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    app_config_meta_t cfg;
    app_config_get_meta(&cfg);

    uint8_t brightness = cfg.brightness;
    bool invert_color = cfg.invert_color;
    bool flip_180 = cfg.flip_180;
    bool swap_rb = cfg.swap_rb;
    bool autoplay = cfg.autoplay;

    cJSON *j;
    if ((j = cJSON_GetObjectItem(body, "brightness")) && cJSON_IsNumber(j)) {
        int b = j->valueint;
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        brightness = (uint8_t)b;
    }
    if ((j = cJSON_GetObjectItem(body, "invert_color")) && cJSON_IsBool(j)) {
        invert_color = cJSON_IsTrue(j);
    }
    if ((j = cJSON_GetObjectItem(body, "flip_180")) && cJSON_IsBool(j)) {
        flip_180 = cJSON_IsTrue(j);
    }
    if ((j = cJSON_GetObjectItem(body, "swap_rb")) && cJSON_IsBool(j)) {
        swap_rb = cJSON_IsTrue(j);
    }
    if ((j = cJSON_GetObjectItem(body, "autoplay")) && cJSON_IsBool(j)) {
        autoplay = cJSON_IsTrue(j);
    }

    cJSON_Delete(body);

    app_config_set_display(brightness, invert_color, flip_180, swap_rb);
    app_config_set_autoplay(autoplay);
    display_ui_apply_settings();

    return send_ok(req);
}

static bool copy_trimmed_if_nonempty(char *dst, size_t dst_len, const char *src, bool *changed)
{
    if (!src || !dst || dst_len == 0) return false;
    const char *start = src;
    while (*start && isspace((unsigned char)*start)) start++;
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    size_t len = (size_t)(end - start);
    if (len == 0) return false;
    if (len >= dst_len) len = dst_len - 1;
    if (changed && (strlen(dst) != len || strncmp(dst, start, len) != 0)) {
        *changed = true;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
    return true;
}

static void add_quota_window(cJSON *arr, const quota_window_t *w)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddBoolToObject(item, "valid", w->valid);
    cJSON_AddStringToObject(item, "name", w->name);
    cJSON_AddNumberToObject(item, "remaining_fraction", (double)w->remaining_fraction);
    cJSON_AddStringToObject(item, "reset_time_str", w->reset_time_str);
    cJSON_AddItemToArray(arr, item);
}

static esp_err_t handle_agy_get(httpd_req_t *req)
{
    app_agy_config_t agy;
    app_config_get_agy(&agy);
    google_quota_data_t qd;
    google_quota_get_data(&qd);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "timezone", agy.timezone);
    cJSON_AddNumberToObject(root, "wc_font_mode", agy.wc_font_mode);
    cJSON_AddNumberToObject(root, "idle_clock_sec", agy.idle_clock_sec);
    cJSON_AddNumberToObject(root, "idle_clock_view", agy.idle_clock_view);
    cJSON_AddBoolToObject(root, "has_cid", agy.oauth_client_id[0] != '\0');
    cJSON_AddBoolToObject(root, "has_sec", agy.oauth_client_secret[0] != '\0');
    cJSON_AddBoolToObject(root, "has_refr", agy.oauth_refresh_token[0] != '\0');
    cJSON_AddStringToObject(root, "oauth_client_id", agy.oauth_client_id);

    cJSON *quota = cJSON_CreateObject();
    cJSON_AddBoolToObject(quota, "is_loading", qd.is_loading);
    cJSON_AddBoolToObject(quota, "has_error", qd.has_error);
    cJSON_AddStringToObject(quota, "error_msg", qd.error_msg);
    cJSON_AddNumberToObject(quota, "last_update_sec", (double)qd.last_update_sec);

    cJSON *windows = cJSON_CreateArray();
    add_quota_window(windows, &qd.limit_5h);
    add_quota_window(windows, &qd.limit_weekly);
    add_quota_window(windows, &qd.limit_claude_5h);
    add_quota_window(windows, &qd.limit_claude_wk);
    add_quota_window(windows, &qd.limit_argon);
    cJSON_AddItemToObject(quota, "windows", windows);

    cJSON_AddItemToObject(root, "quota", quota);
    return send_json(req, root);
}

static esp_err_t handle_agy_post(httpd_req_t *req)
{
    cJSON *body = recv_json_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    app_agy_config_t agy;
    app_config_get_agy(&agy);

    bool creds_changed = false;
    bool refresh_requested = false;
    cJSON *j;

    if ((j = cJSON_GetObjectItem(body, "timezone")) && cJSON_IsString(j)) {
        copy_trimmed_if_nonempty(agy.timezone, sizeof(agy.timezone), j->valuestring, NULL);
    }
    if ((j = cJSON_GetObjectItem(body, "wc_font_mode")) && cJSON_IsNumber(j)) {
        int fm = j->valueint;
        if (fm < 0) fm = 0;
        if (fm > 4) fm = 4;
        agy.wc_font_mode = (uint8_t)fm;
    }
    if ((j = cJSON_GetObjectItem(body, "idle_clock_sec")) && cJSON_IsNumber(j)) {
        int sec = j->valueint;
        if (sec < 0) sec = 0;
        if (sec > 65535) sec = 65535;
        agy.idle_clock_sec = (uint16_t)sec;
    }
    if ((j = cJSON_GetObjectItem(body, "idle_clock_view")) && cJSON_IsNumber(j)) {
        agy.idle_clock_view = (j->valueint == 1) ? 1 : 0;
    }

    if ((j = cJSON_GetObjectItem(body, "cid")) && cJSON_IsString(j)) {
        copy_trimmed_if_nonempty(agy.oauth_client_id, sizeof(agy.oauth_client_id), j->valuestring, &creds_changed);
    } else if ((j = cJSON_GetObjectItem(body, "oauth_client_id")) && cJSON_IsString(j)) {
        copy_trimmed_if_nonempty(agy.oauth_client_id, sizeof(agy.oauth_client_id), j->valuestring, &creds_changed);
    }

    if ((j = cJSON_GetObjectItem(body, "sec")) && cJSON_IsString(j)) {
        copy_trimmed_if_nonempty(agy.oauth_client_secret, sizeof(agy.oauth_client_secret), j->valuestring, &creds_changed);
    } else if ((j = cJSON_GetObjectItem(body, "oauth_client_secret")) && cJSON_IsString(j)) {
        copy_trimmed_if_nonempty(agy.oauth_client_secret, sizeof(agy.oauth_client_secret), j->valuestring, &creds_changed);
    }

    if ((j = cJSON_GetObjectItem(body, "refr")) && cJSON_IsString(j)) {
        copy_trimmed_if_nonempty(agy.oauth_refresh_token, sizeof(agy.oauth_refresh_token), j->valuestring, &creds_changed);
    } else if ((j = cJSON_GetObjectItem(body, "oauth_refresh_token")) && cJSON_IsString(j)) {
        copy_trimmed_if_nonempty(agy.oauth_refresh_token, sizeof(agy.oauth_refresh_token), j->valuestring, &creds_changed);
    }

    if ((j = cJSON_GetObjectItem(body, "refresh")) && cJSON_IsBool(j) && cJSON_IsTrue(j)) {
        refresh_requested = true;
    }

    cJSON_Delete(body);

    esp_err_t err = app_config_set_agy(&agy);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Agy config");
        return ESP_FAIL;
    }

    sntp_time_update_tz(agy.timezone);
    display_ui_apply_agy_settings();
    if (creds_changed || refresh_requested) {
        google_quota_request_refresh();
    }

    return send_ok(req);
}

static void url_decode(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    size_t s = 0;
    while (s < src_len && src[s] != '\0' && i < dst_size - 1) {
        if (src[s] == '%' && s + 2 < src_len &&
            isxdigit((unsigned char)src[s + 1]) &&
            isxdigit((unsigned char)src[s + 2])) {
            char hex_buf[3] = { src[s + 1], src[s + 2], '\0' };
            dst[i++] = (char)strtol(hex_buf, NULL, 16);
            s += 3;
        } else if (src[s] == '+') {
            dst[i++] = ' ';
            s++;
        } else {
            dst[i++] = src[s++];
        }
    }
    dst[i] = '\0';
}

static bool extract_form_param(const char *post_data, const char *key, char *out, size_t out_len)
{
    if (!post_data || !key || !out || out_len == 0) return false;
    size_t key_len = strlen(key);
    const char *p = post_data;
    while (*p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        size_t seg_end = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            const char *val = eq + 1;
            size_t val_len = seg_end - (size_t)(val - p);
            url_decode(out, out_len, val, val_len);
            return true;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static esp_err_t handle_save_post(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload size");
        return ESP_FAIL;
    }

    char *buf = malloc((size_t)total + 1);
    char *val = malloc(640);
    if (!buf || !val) {
        free(buf);
        free(val);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            free(val);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    app_agy_config_t agy;
    app_config_get_agy(&agy);

    bool creds_changed = false;
    bool oauth_provided = false;

    if (extract_form_param(buf, "cid", val, 640)) {
        if (copy_trimmed_if_nonempty(agy.oauth_client_id, sizeof(agy.oauth_client_id), val, &creds_changed)) {
            oauth_provided = true;
        }
    }
    if (extract_form_param(buf, "sec", val, 640)) {
        if (copy_trimmed_if_nonempty(agy.oauth_client_secret, sizeof(agy.oauth_client_secret), val, &creds_changed)) {
            oauth_provided = true;
        }
    }
    if (extract_form_param(buf, "refr", val, 640)) {
        if (copy_trimmed_if_nonempty(agy.oauth_refresh_token, sizeof(agy.oauth_refresh_token), val, &creds_changed)) {
            oauth_provided = true;
        }
    }
    if (extract_form_param(buf, "tz", val, 640)) {
        copy_trimmed_if_nonempty(agy.timezone, sizeof(agy.timezone), val, NULL);
    }
    if (extract_form_param(buf, "font", val, 640) && val[0] != '\0') {
        int fm = atoi(val);
        if (fm < 0) fm = 0;
        if (fm > 4) fm = 4;
        agy.wc_font_mode = (uint8_t)fm;
    }
    if (extract_form_param(buf, "bright", val, 640) && val[0] != '\0') {
        int b = atoi(val);
        if (b < 10) b = 10;
        if (b > 100) b = 100;
        app_config_set_brightness((uint8_t)b);
        display_ui_set_brightness((uint8_t)b);
    }

    free(buf);
    free(val);

    esp_err_t err = app_config_set_agy(&agy);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Agy config");
        return ESP_FAIL;
    }

    sntp_time_update_tz(agy.timezone);
    display_ui_apply_agy_settings();
    if (creds_changed || oauth_provided) {
        google_quota_request_refresh();
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req,
        "<html><body style='background:#0b1120;color:#f8fafc;font-family:sans-serif;padding:24px;text-align:center;'>"
        "<h2 style='color:#38bdf8;'>Settings Saved</h2>"
        "<p>Configuration applied.</p>"
        "<p><a href='/' style='color:#38bdf8;'>&larr; Back</a></p>"
        "</body></html>",
        HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_404_captive(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    /* Redirect captive portal probes (Android/iOS/Windows) to root page */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, "Redirecting...", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 6144;
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    const httpd_uri_t uris[] = {
        { .uri = "/",                   .method = HTTP_GET,    .handler = handle_root_get },
        { .uri = "/api/status",         .method = HTTP_GET,    .handler = handle_status_get },
        { .uri = "/api/control",        .method = HTTP_POST,   .handler = handle_control_post },
        { .uri = "/api/stations",       .method = HTTP_GET,    .handler = handle_stations_get },
        { .uri = "/api/stations",       .method = HTTP_POST,   .handler = handle_stations_post },
        { .uri = "/api/stations",       .method = HTTP_PUT,    .handler = handle_stations_put },
        { .uri = "/api/stations",       .method = HTTP_DELETE, .handler = handle_stations_delete },
        { .uri = "/api/stations/reset", .method = HTTP_POST,   .handler = handle_stations_reset_post },
        { .uri = "/api/wifi/scan",      .method = HTTP_GET,    .handler = handle_wifi_scan_get },
        { .uri = "/api/wifi",           .method = HTTP_POST,   .handler = handle_wifi_post },
        { .uri = "/api/display",        .method = HTTP_POST,   .handler = handle_display_post },
        { .uri = "/api/agy",            .method = HTTP_GET,    .handler = handle_agy_get },
        { .uri = "/api/agy",            .method = HTTP_POST,   .handler = handle_agy_post },
        { .uri = "/save",               .method = HTTP_POST,   .handler = handle_save_post },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, handle_404_captive);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}
