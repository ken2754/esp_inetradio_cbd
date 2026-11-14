#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_IDLE = 0,
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED_STA,
    WIFI_MGR_AP_CONFIG_MODE,
} wifi_mgr_state_t;

typedef struct {
    wifi_mgr_state_t state;
    char ssid[33];
    char ip_addr[16];
    char ap_ssid[33];
    char ap_ip[16];
    int8_t rssi;
} wifi_mgr_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_entry_t;

esp_err_t wifi_manager_init(void);
void wifi_manager_get_status(wifi_mgr_status_t *out_status);

/* Trigger STA connect with current NVS credentials (non-blocking) */
void wifi_manager_connect_sta_async(void);

/* Start SoftAP configuration mode explicitly */
void wifi_manager_start_ap_mode(void);

/* Synchronous scan (up to max_entries) */
int wifi_manager_scan(wifi_scan_entry_t *out_list, int max_entries);

#ifdef __cplusplus
}
#endif
