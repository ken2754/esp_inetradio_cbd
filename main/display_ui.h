#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_ui_init(i2c_master_bus_handle_t i2c_bus);

/* Apply brightness, invert_color, flip_180, swap_rb from app_config */
void display_ui_apply_settings(void);

/* Set backlight brightness 10..100 */
void display_ui_set_brightness(uint8_t pct);

/* Rebuild station list UI after web UI changes */
void display_ui_refresh_stations(void);

#ifdef __cplusplus
}
#endif
