#include "wifi_manager.h"
#include "app_config.h"
#include "audio_player.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define STA_MAX_RETRY       5

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static EventGroupHandle_t s_wifi_events = NULL;
static SemaphoreHandle_t s_status_mutex = NULL;

static wifi_mgr_status_t s_status;
static int s_retry_num = 0;
static bool s_mdns_started = false;
static bool s_ap_active = false;
static bool s_auto_played_once = false;

static void start_mdns_once(void)
{
    if (s_mdns_started) return;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(APP_MDNS_HOSTNAME);
        mdns_instance_name_set("ES3C28P Internet Radio");
        mdns_service_add("ES3C28P Radio Web", "_http", "_tcp", 80, NULL, 0);
        s_mdns_started = true;
        ESP_LOGI(TAG, "mDNS started: http://%s.local/", APP_MDNS_HOSTNAME);
    }
}

/* Lightweight Captive Portal DNS server active only when SoftAP is running */
static void captive_dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < 12) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!s_ap_active) {
            continue;
        }

        /* Only reply to standard queries (QR == 0) */
        if ((buf[2] & 0x80) != 0) continue;

        /* Build minimal DNS response pointing to 192.168.4.1 */
        buf[2] |= 0x80; /* QR = 1 (Response) */
        buf[2] |= 0x04; /* AA = 1 */
        buf[3] = (buf[3] & 0xF0);
        buf[6] = 0; buf[7] = 1; /* ANCOUNT = 1 */
        buf[8] = 0; buf[9] = 0; /* NSCOUNT = 0 */
        buf[10] = 0; buf[11] = 0; /* ARCOUNT = 0 */

        /* Skip question section */
        int pos = 12;
        while (pos < len && buf[pos] != 0) {
            uint8_t lbl = buf[pos];
            if ((lbl & 0xC0) == 0xC0) {
                pos += 2;
                break;
            }
            pos += lbl + 1;
        }
        if (pos < len && buf[pos] == 0) pos++;
        pos += 4; /* QTYPE + QCLASS */
        if (pos > len || pos + 16 > (int)sizeof(buf)) continue;

        /* Answer: pointer to name at offset 12 (0xC00C), TYPE A, CLASS IN, TTL 60, RDLENGTH 4, 192.168.4.1 */
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;
        buf[pos++] = 0x00; buf[pos++] = 0x01; /* TYPE A */
        buf[pos++] = 0x00; buf[pos++] = 0x01; /* CLASS IN */
        buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 60; /* TTL 60s */
        buf[pos++] = 0x00; buf[pos++] = 0x04; /* RDLENGTH */
        buf[pos++] = 192;  buf[pos++] = 168;  buf[pos++] = 4;    buf[pos++] = 1;

        sendto(sock, buf, pos, 0, (struct sockaddr *)&client_addr, client_len);
    }
}

static void enable_ap_locked_state(void)
{
    if (!s_ap_active) {
        wifi_config_t ap_cfg = {
            .ap = {
                .ssid = APP_DEFAULT_AP_SSID,
                .ssid_len = strlen(APP_DEFAULT_AP_SSID),
                .channel = 1,
                .password = "",
                .max_connection = 4,
                .authmode = WIFI_AUTH_OPEN,
            },
        };
        wifi_mode_t cur_mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&cur_mode);
        if (cur_mode != WIFI_MODE_APSTA && cur_mode != WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_APSTA);
        }
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        s_ap_active = true;
        ESP_LOGI(TAG, "SoftAP active: SSID='%s' IP=192.168.4.1", APP_DEFAULT_AP_SSID);
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = WIFI_MGR_AP_CONFIG_MODE;
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s", APP_DEFAULT_AP_SSID);
    snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), "192.168.4.1");
    xSemaphoreGive(s_status_mutex);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        app_config_meta_t cfg;
        app_config_get_meta(&cfg);
        if (cfg.wifi_ssid[0] != '\0') {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < STA_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi connection (%d/%d)...", s_retry_num, STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
            if (!s_ap_active) {
                enable_ap_locked_state();
            }
        }
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        if (s_status.state == WIFI_MGR_CONNECTED_STA) {
            s_status.state = s_ap_active ? WIFI_MGR_AP_CONFIG_MODE : WIFI_MGR_CONNECTING;
            s_status.ip_addr[0] = '\0';
        }
        xSemaphoreGive(s_status_mutex);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_num = 0;

        wifi_ap_record_t ap_info = {0};
        esp_wifi_sta_get_ap_info(&ap_info);

        app_config_meta_t cfg;
        app_config_get_meta(&cfg);

        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
        s_status.state = WIFI_MGR_CONNECTED_STA;
        snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", cfg.wifi_ssid);
        snprintf(s_status.ip_addr, sizeof(s_status.ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        s_status.rssi = ap_info.rssi;
        xSemaphoreGive(s_status_mutex);

        ESP_LOGI(TAG, "Connected to '%s', IP: %s", cfg.wifi_ssid, s_status.ip_addr);

        /* Disable AP if it was only open for provisioning */
        if (s_ap_active) {
            s_ap_active = false;
            esp_wifi_set_mode(WIFI_MODE_STA);
        }

        start_mdns_once();
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);

        if (cfg.autoplay && !s_auto_played_once) {
            s_auto_played_once = true;
            audio_player_play_station(cfg.current_station);
        }
    }
}

static void connect_task(void *arg)
{
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);

    if (cfg.wifi_ssid[0] == '\0') {
        enable_ap_locked_state();
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.state = WIFI_MGR_CONNECTING;
    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", cfg.wifi_ssid);
    s_status.ip_addr[0] = '\0';
    xSemaphoreGive(s_status_mutex);

    s_retry_num = 0;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t sta_cfg = {0};
    size_t ssid_len = strnlen(cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
    size_t pass_len = strnlen(cfg.wifi_pass, sizeof(sta_cfg.sta.password));
    memcpy(sta_cfg.sta.ssid, cfg.wifi_ssid, ssid_len);
    memcpy(sta_cfg.sta.password, cfg.wifi_pass, pass_len);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur_mode);
    wifi_mode_t target_mode = s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    if (cur_mode != target_mode) {
        esp_wifi_set_mode(target_mode);
    }
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "STA connection timed out/failed; enabling SoftAP config mode");
        enable_ap_locked_state();
    }

    vTaskDelete(NULL);
}

esp_err_t wifi_manager_init(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    s_wifi_events = xEventGroupCreate();
    memset(&s_status, 0, sizeof(s_status));
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "%s", APP_DEFAULT_AP_SSID);
    snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), "192.168.4.1");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    (void)s_sta_netif;
    (void)s_ap_netif;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    app_config_meta_t cfg;
    app_config_get_meta(&cfg);

    if (cfg.wifi_ssid[0] != '\0') {
        wifi_config_t sta_cfg = {0};
        size_t ssid_len = strnlen(cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
        size_t pass_len = strnlen(cfg.wifi_pass, sizeof(sta_cfg.sta.password));
        memcpy(sta_cfg.sta.ssid, cfg.wifi_ssid, ssid_len);
        memcpy(sta_cfg.sta.password, cfg.wifi_pass, pass_len);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
        enable_ap_locked_state();
    }

    /* Disable Wi-Fi power save for smooth low-jitter audio streaming */
    esp_wifi_set_ps(WIFI_PS_NONE);

    xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 3, NULL);

    if (cfg.wifi_ssid[0] != '\0') {
        wifi_manager_connect_sta_async();
    }

    return ESP_OK;
}

void wifi_manager_get_status(wifi_mgr_status_t *out_status)
{
    if (!out_status) return;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    if (s_status.state == WIFI_MGR_CONNECTED_STA) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_status.rssi = ap_info.rssi;
        }
    }
    memcpy(out_status, &s_status, sizeof(wifi_mgr_status_t));
    xSemaphoreGive(s_status_mutex);
}

void wifi_manager_connect_sta_async(void)
{
    xTaskCreate(connect_task, "wifi_conn", 4096, NULL, 4, NULL);
}

void wifi_manager_start_ap_mode(void)
{
    enable_ap_locked_state();
}

int wifi_manager_scan(wifi_scan_entry_t *out_list, int max_entries)
{
    if (!out_list || max_entries <= 0) return 0;

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) return 0;

    uint16_t fetch = ap_count > 20 ? 20 : ap_count;
    wifi_ap_record_t *recs = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!recs) return 0;

    if (esp_wifi_scan_get_ap_records(&fetch, recs) != ESP_OK) {
        free(recs);
        return 0;
    }

    int added = 0;
    for (int i = 0; i < fetch && added < max_entries; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        /* Deduplicate SSIDs */
        bool dup = false;
        for (int j = 0; j < added; j++) {
            if (strcmp(out_list[j].ssid, (const char *)recs[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        snprintf(out_list[added].ssid, sizeof(out_list[added].ssid), "%s", (const char *)recs[i].ssid);
        out_list[added].rssi = recs[i].rssi;
        out_list[added].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
        added++;
    }

    free(recs);
    return added;
}
