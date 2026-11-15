#include "display_ui.h"
#include "album_art.h"
#include "app_config.h"
#include "audio_player.h"
#include "board_config.h"
#include "google_quota.h"
#include "sntp_time.h"
#include "wifi_manager.h"
#include "word_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"

static const char *TAG = "display_ui";

#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_DUTY_RES    LEDC_TIMER_10_BIT

#define STATION_BANNER_US   2000000LL
#define OSD_TIMEOUT_US      1400000LL
#define TOAST_TIMEOUT_US    1500000LL
#define WC_STATUS_HIDE_US   60000000LL
#define SLEEP_WAKE_GUARD_US 350000LL
#define LONG_PRESS_US       550000LL
#define DOUBLE_TAP_US       280000LL
#define CLOCK_POLL_US       1000000LL

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_lvgl_disp = NULL;

/* UI views */
typedef enum {
    VIEW_PLAYER = 0,
    VIEW_WORD_CLOCK,
    VIEW_AGY_DASHBOARD,
    VIEW_STOPPED,
    VIEW_SLEEP,
} ui_view_t;

typedef enum {
    STOP_TAB_STATIONS = 0,
    STOP_TAB_SETTINGS,
} stopped_tab_t;

typedef enum {
    GEST_NONE = 0,
    GEST_VERT,
    GEST_SWIPE_X,
} gesture_mode_t;

typedef enum {
    WC_FONT_MODERN_SANS = 0,
    WC_FONT_BOLD_DISPLAY,
    WC_FONT_COMPACT,
    WC_FONT_RETRO_MONO,
    WC_FONT_COUNT
} wc_font_style_t;

/* 8-direction outlined label for legible overlay text on bright artwork */
#define OUTLINE_OFFSETS 8
typedef struct {
    lv_obj_t *wrap;
    lv_obj_t *bg[OUTLINE_OFFSETS];
    lv_obj_t *fg;
} outlined_label_t;

static volatile ui_view_t s_cur_view = VIEW_PLAYER;
static volatile bool s_has_pending_view = false;
static volatile int s_pending_view = VIEW_PLAYER;
static volatile bool s_need_agy_apply = false;

static stopped_tab_t s_stop_tab = STOP_TAB_STATIONS;
static uint8_t s_cur_brightness = 80;
static app_agy_config_t s_agy_cfg;

/* Top-level screen, shared artwork backdrop, and view panels */
static lv_obj_t *s_scr_main = NULL;
static lv_obj_t *s_bg_art = NULL;
static lv_obj_t *s_panel_player = NULL;
static lv_obj_t *s_panel_wordclock = NULL;
static lv_obj_t *s_panel_agy = NULL;
static lv_obj_t *s_panel_stopped = NULL;
static lv_obj_t *s_panel_sleep = NULL;

/* Player widgets (zero buttons, maximized art) */
static outlined_label_t s_ol_track;
static outlined_label_t s_ol_artist;
static lv_grad_dsc_t s_scrim_grad;

/* Word Clock widgets (zero buttons) */
static lv_obj_t *s_wc_outline[OUTLINE_OFFSETS];
static lv_obj_t *s_wc_time_label = NULL;
static lv_obj_t *s_wc_status_shadow = NULL;
static lv_obj_t *s_wc_status_label = NULL;
static int64_t s_wc_status_hide_us = 0;
static char s_last_wc_status_str[256] = {0};
static wc_font_style_t s_wc_font_style = WC_FONT_MODERN_SANS;
static int s_last_wc_hour = -1;
static int s_last_wc_min = -1;
static int s_last_wc_block_min = -1;

/* Agy Dashboard widgets (zero buttons) */
static lv_obj_t *s_db_label_wifi = NULL;
static lv_obj_t *s_db_label_time = NULL;
static lv_obj_t *s_db_label_ampm = NULL;
static lv_obj_t *s_db_label_date = NULL;
static lv_obj_t *s_db_label_status = NULL;

typedef struct {
    lv_obj_t *title;
    lv_obj_t *val;
    lv_obj_t *bar;
    lv_obj_t *reset;
} quota_slot_ui_t;

static quota_slot_ui_t s_q_slots[5];
static int64_t s_q_last_update_sec = -1;
static bool s_q_last_loading = false;
static bool s_q_last_has_error = false;
static char s_q_last_err_msg[64] = {0};
static bool s_force_quota_redraw = true;

/* Temporary 2-second station banner overlay */
static lv_obj_t *s_station_banner = NULL;
static lv_obj_t *s_lbl_station_banner = NULL;
static int64_t s_station_hide_us = 0;
static int s_last_station_idx = -1;

/* Temporary gesture volume/brightness/stopping OSD overlay */
static lv_obj_t *s_osd_box = NULL;
static lv_obj_t *s_osd_lbl = NULL;
static lv_obj_t *s_osd_bar = NULL;
static int64_t s_osd_hide_us = 0;

/* Centered Toast overlay (1.5s auto-hide) */
static lv_obj_t *s_toast = NULL;
static lv_obj_t *s_toast_label = NULL;
static int64_t s_toast_hide_us = 0;

/* Artwork LVGL buffer */
static uint16_t *s_art_pixels = NULL;
static lv_image_dsc_t s_art_dsc;
static uint32_t s_shown_art_ver = 0;
static bool s_art_ready = false;
static bool s_has_art = false;

/* Unified Stopped screen widgets */
static lv_obj_t *s_btn_tab_stations = NULL;
static lv_obj_t *s_btn_tab_settings = NULL;
static lv_obj_t *s_tab_stations = NULL;
static lv_obj_t *s_tab_settings = NULL;
static lv_obj_t *s_list_stations = NULL;
static volatile bool s_need_station_rebuild = false;

static lv_obj_t *s_lbl_net_mode = NULL;
static lv_obj_t *s_lbl_net_detail = NULL;
static lv_obj_t *s_lbl_net_hint = NULL;

/* Unified touch gesture state (Player, Word Clock, Agy Dashboard) */
static gesture_mode_t s_gest_mode = GEST_NONE;
static lv_point_t s_gest_start = {0, 0};
static lv_point_t s_gest_last = {0, 0};
static int s_gest_start_vol = 0;
static int s_gest_start_bright = 80;
static int s_gest_max_dist = 0;
static bool s_gest_active = false;
static bool s_gest_long_fired = false;
static bool s_gest_ignore_until_release = false;
static int64_t s_gest_press_us = 0;

/* Double-tap vs single-tap disambiguation state */
static bool s_tap_pending = false;
static int64_t s_tap_first_us = 0;
static ui_view_t s_tap_view = VIEW_PLAYER;
static lv_point_t s_tap_pt = {0, 0};

/* Stopped screen horizontal swipe state */
static bool s_stop_gest_active = false;
static bool s_stop_swipe_blocked_click = false;
static lv_point_t s_stop_gest_start = {0, 0};

/* Activity & playback transition tracking */
static int64_t s_last_activity_us = 0;
static int64_t s_last_clock_poll_us = 0;
static bool s_force_clock_update = true;
static player_state_t s_prev_player_state = PLAYER_STATE_STOPPED;

/* Sleep state */
static int64_t s_sleep_entered_us = 0;
static bool s_sleep_touch_armed = false;

static void set_backlight_raw(uint8_t pct)
{
    if (pct > 100) pct = 100;
    uint32_t duty = ((uint32_t)pct * 1023U) / 100U;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

static void backlight_init(uint8_t initial_pct)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BOARD_LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
    display_ui_set_brightness(initial_pct);
}

void display_ui_set_brightness(uint8_t pct)
{
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    s_cur_brightness = pct;
    if (s_cur_view != VIEW_SLEEP) {
        set_backlight_raw(pct);
    }
}

static void apply_orientation_and_color(bool invert_color, bool flip_180, bool swap_rb)
{
    if (!s_lcd_panel || !s_lvgl_disp || !s_touch_handle) return;

    esp_lcd_panel_invert_color(s_lcd_panel, invert_color);

    /* LVGL v9 lv_indev automatically rotates pointer coords via lv_display_rotate_point(),
     * so FT6336G stays in native portrait coords (240x320) without extra swap/mirror. */
    lv_display_set_rotation(s_lvgl_disp, flip_180 ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_270);
    esp_lcd_touch_set_swap_xy(s_touch_handle, false);
    esp_lcd_touch_set_mirror_x(s_touch_handle, false);
    esp_lcd_touch_set_mirror_y(s_touch_handle, false);

    uint8_t madctl = flip_180 ? 0xE0 : 0x20;
    if (!swap_rb) {
        madctl |= 0x08; /* BGR bit */
    }
    esp_lcd_panel_io_tx_param(s_lcd_io, 0x36, &madctl, 1);
}

void display_ui_apply_settings(void)
{
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);
    s_cur_brightness = cfg.brightness;
    if (s_cur_view != VIEW_SLEEP) {
        display_ui_set_brightness(cfg.brightness);
    }

    if (lvgl_port_lock(500)) {
        apply_orientation_and_color(cfg.invert_color, cfg.flip_180, cfg.swap_rb);
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_unlock();
    }
}

void display_ui_apply_agy_settings(void)
{
    s_need_agy_apply = true;
}

/* ---------- Word Clock Fonts ---------- */
static const lv_font_t *wc_font_get(wc_font_style_t style)
{
    switch (style) {
        case WC_FONT_MODERN_SANS:
#if defined(CONFIG_LV_FONT_MONTSERRAT_28) && CONFIG_LV_FONT_MONTSERRAT_28
            return &lv_font_montserrat_28;
#else
            return &lv_font_montserrat_24;
#endif
        case WC_FONT_BOLD_DISPLAY:
#if defined(CONFIG_LV_FONT_MONTSERRAT_32) && CONFIG_LV_FONT_MONTSERRAT_32
            return &lv_font_montserrat_32;
#elif defined(CONFIG_LV_FONT_MONTSERRAT_28) && CONFIG_LV_FONT_MONTSERRAT_28
            return &lv_font_montserrat_28;
#else
            return &lv_font_montserrat_24;
#endif
        case WC_FONT_COMPACT:
#if defined(CONFIG_LV_FONT_MONTSERRAT_24) && CONFIG_LV_FONT_MONTSERRAT_24
            return &lv_font_montserrat_24;
#else
            return &lv_font_montserrat_16;
#endif
        case WC_FONT_RETRO_MONO:
#if defined(CONFIG_LV_FONT_UNSCII_16) && CONFIG_LV_FONT_UNSCII_16
            return &lv_font_unscii_16;
#else
            return &lv_font_montserrat_24;
#endif
        default:
            return &lv_font_montserrat_24;
    }
}

static const lv_font_t *agy_small_font_get(void)
{
#if defined(CONFIG_LV_FONT_MONTSERRAT_10) && CONFIG_LV_FONT_MONTSERRAT_10
    return &lv_font_montserrat_10;
#else
    return &lv_font_montserrat_12;
#endif
}

static const char *wc_font_get_name(wc_font_style_t style)
{
    switch (style) {
        case WC_FONT_MODERN_SANS:  return "Modern Sans";
        case WC_FONT_BOLD_DISPLAY: return "Bold Display";
        case WC_FONT_COMPACT:      return "Compact";
        case WC_FONT_RETRO_MONO:   return "Retro Mono";
        default:                   return "Standard";
    }
}

static void wc_apply_font_style(wc_font_style_t style)
{
    s_wc_font_style = style;
    const lv_font_t *font = wc_font_get(style);
    if (s_wc_time_label) {
        lv_obj_set_style_text_font(s_wc_time_label, font, 0);
    }
    for (int i = 0; i < OUTLINE_OFFSETS; i++) {
        if (s_wc_outline[i]) {
            lv_obj_set_style_text_font(s_wc_outline[i], font, 0);
        }
    }
}

/* ---------- Outlined Label Helpers ---------- */
static void outlined_label_create(lv_obj_t *parent, outlined_label_t *ol, int32_t w,
                                  const lv_font_t *font, lv_color_t fg_color, lv_text_align_t align)
{
    static const int8_t dx[OUTLINE_OFFSETS] = { -1,  0,  1, -1, 1, -1, 0, 1 };
    static const int8_t dy[OUTLINE_OFFSETS] = { -1, -1, -1,  0, 0,  1, 1, 1 };

    ol->wrap = lv_obj_create(parent);
    lv_obj_set_size(ol->wrap, w + 4, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ol->wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ol->wrap, 0, 0);
    lv_obj_set_style_pad_all(ol->wrap, 2, 0);
    lv_obj_set_scrollable(ol->wrap, false);
    lv_obj_set_clickable(ol->wrap, false);

    for (int i = 0; i < OUTLINE_OFFSETS; i++) {
        lv_obj_t *lbl = lv_label_create(ol->wrap);
        lv_label_set_text(lbl, "");
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, w);
        lv_obj_set_style_text_align(lbl, align, 0);
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_MID, dx[i], dy[i]);
        lv_obj_set_clickable(lbl, false);
        ol->bg[i] = lbl;
    }

    ol->fg = lv_label_create(ol->wrap);
    lv_label_set_text(ol->fg, "");
    lv_label_set_long_mode(ol->fg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ol->fg, w);
    lv_obj_set_style_text_align(ol->fg, align, 0);
    lv_obj_set_style_text_font(ol->fg, font, 0);
    lv_obj_set_style_text_color(ol->fg, fg_color, 0);
    lv_obj_align(ol->fg, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(ol->fg, false);
}

static void outlined_label_set_text(outlined_label_t *ol, const char *txt)
{
    if (!ol->fg) return;
    const char *s = txt ? txt : "";
    if (strcmp(lv_label_get_text(ol->fg), s) == 0) return;

    for (int i = 0; i < OUTLINE_OFFSETS; i++) {
        lv_label_set_text(ol->bg[i], s);
    }
    lv_label_set_text(ol->fg, s);
}

static void outlined_label_set_font(outlined_label_t *ol, const lv_font_t *font)
{
    if (!ol->fg) return;
    if (lv_obj_get_style_text_font(ol->fg, 0) == font) return;

    for (int i = 0; i < OUTLINE_OFFSETS; i++) {
        lv_obj_set_style_text_font(ol->bg[i], font, 0);
    }
    lv_obj_set_style_text_font(ol->fg, font, 0);
}

static void outlined_label_set_color(outlined_label_t *ol, lv_color_t color)
{
    if (!ol->fg) return;
    lv_obj_set_style_text_color(ol->fg, color, 0);
}

static void outlined_label_set_hidden(outlined_label_t *ol, bool hidden)
{
    if (!ol->wrap) return;
    lv_obj_set_hidden(ol->wrap, hidden);
}

/* ---------- Overlay Helpers ---------- */
static void show_toast(const char *msg)
{
    if (!s_toast || !s_toast_label || !msg) return;
    lv_label_set_text(s_toast_label, msg);
    lv_obj_set_hidden(s_toast, false);
    lv_obj_move_foreground(s_toast);
    s_toast_hide_us = esp_timer_get_time() + TOAST_TIMEOUT_US;
}

static void show_station_banner(int station_idx, const char *station_name)
{
    if (!s_station_banner || !s_lbl_station_banner) return;
    char buf[80];
    snprintf(buf, sizeof(buf), "%d. %s", station_idx + 1,
             (station_name && station_name[0]) ? station_name : "Station");
    lv_label_set_text(s_lbl_station_banner, buf);
    lv_obj_set_hidden(s_station_banner, false);
    lv_obj_move_foreground(s_station_banner);
    s_station_hide_us = esp_timer_get_time() + STATION_BANNER_US;
    s_last_station_idx = station_idx;
}

static void show_station_banner_current(void)
{
    player_status_t pst;
    audio_player_get_status(&pst);
    show_station_banner(pst.station_index, pst.station_name);
}

static void show_vol_osd(int vol_pct)
{
    if (!s_osd_box) return;
    char buf[24];
    snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX "  %d%%", vol_pct);
    lv_label_set_text(s_osd_lbl, buf);
    lv_bar_set_value(s_osd_bar, vol_pct, LV_ANIM_OFF);
    lv_obj_set_hidden(s_osd_bar, false);
    lv_obj_set_hidden(s_osd_box, false);
    lv_obj_move_foreground(s_osd_box);
    s_osd_hide_us = esp_timer_get_time() + OSD_TIMEOUT_US;
}

static void show_bright_osd(int bright_pct)
{
    if (!s_osd_box) return;
    char buf[28];
    snprintf(buf, sizeof(buf), LV_SYMBOL_EYE_OPEN "  %d%%", bright_pct);
    lv_label_set_text(s_osd_lbl, buf);
    lv_bar_set_value(s_osd_bar, bright_pct, LV_ANIM_OFF);
    lv_obj_set_hidden(s_osd_bar, false);
    lv_obj_set_hidden(s_osd_box, false);
    lv_obj_move_foreground(s_osd_box);
    s_osd_hide_us = esp_timer_get_time() + OSD_TIMEOUT_US;
}

static void show_wc_status(const char *txt, int64_t now_us)
{
    if (!s_wc_status_label || !s_wc_status_shadow || !txt) return;
    lv_label_set_text(s_wc_status_shadow, txt);
    lv_label_set_text(s_wc_status_label, txt);
    lv_obj_set_hidden(s_wc_status_shadow, false);
    lv_obj_set_hidden(s_wc_status_label, false);
    s_wc_status_hide_us = now_us + WC_STATUS_HIDE_US;
}

/* ---------- Shared Album Art Backdrop State ---------- */
static void update_bg_art_visuals(player_state_t pstate)
{
    if (!s_bg_art) return;

    ui_view_t view = s_cur_view;
    bool show = false;
    if (s_art_ready) {
        if (view == VIEW_PLAYER) {
            show = true;
        } else if (view == VIEW_WORD_CLOCK || view == VIEW_AGY_DASHBOARD) {
            show = (pstate != PLAYER_STATE_STOPPED && s_has_art);
        }
    }

    if (!show) {
        lv_obj_set_hidden(s_bg_art, true);
        return;
    }

    if (view == VIEW_AGY_DASHBOARD) {
        lv_obj_set_style_image_recolor(s_bg_art, lv_color_black(), 0);
        lv_obj_set_style_image_recolor_opa(s_bg_art, LV_OPA_50, 0);
    } else {
        lv_obj_set_style_image_recolor_opa(s_bg_art, LV_OPA_TRANSP, 0);
    }
    lv_obj_set_hidden(s_bg_art, false);
}

/* ---------- View & Tab Switching ---------- */
static void switch_stopped_tab(stopped_tab_t tab)
{
    s_stop_tab = tab;
    bool st_active = (tab == STOP_TAB_STATIONS);
    lv_obj_set_hidden(s_tab_stations, !st_active);
    lv_obj_set_hidden(s_tab_settings, st_active);
    lv_obj_set_style_bg_color(s_btn_tab_stations,
                              lv_color_hex(st_active ? 0x0369A1 : 0x1E293B), 0);
    lv_obj_set_style_bg_color(s_btn_tab_settings,
                              lv_color_hex(!st_active ? 0x0369A1 : 0x1E293B), 0);
    if (st_active) {
        s_need_station_rebuild = true;
    }
}

static void switch_view(ui_view_t view)
{
    s_cur_view = view;
    if (s_gest_active) {
        s_gest_ignore_until_release = true;
    }
    s_gest_active = false;
    s_gest_mode = GEST_NONE;
    s_tap_pending = false;
    s_stop_gest_active = false;

    lv_obj_set_hidden(s_panel_player, view != VIEW_PLAYER);
    lv_obj_set_hidden(s_panel_wordclock, view != VIEW_WORD_CLOCK);
    lv_obj_set_hidden(s_panel_agy, view != VIEW_AGY_DASHBOARD);
    lv_obj_set_hidden(s_panel_stopped, view != VIEW_STOPPED);
    lv_obj_set_hidden(s_panel_sleep, view != VIEW_SLEEP);

    player_status_t pst;
    audio_player_get_status(&pst);
    update_bg_art_visuals(pst.state);

    google_quota_set_active(view == VIEW_AGY_DASHBOARD);

    if (view == VIEW_SLEEP) {
        if (s_station_banner) lv_obj_set_hidden(s_station_banner, true);
        if (s_osd_box) lv_obj_set_hidden(s_osd_box, true);
        if (s_toast) lv_obj_set_hidden(s_toast, true);
    } else if (view == VIEW_STOPPED) {
        wifi_mgr_status_t wst;
        wifi_manager_get_status(&wst);
        if (wst.state == WIFI_MGR_AP_CONFIG_MODE) {
            switch_stopped_tab(STOP_TAB_SETTINGS);
        } else {
            switch_stopped_tab(s_stop_tab);
        }
        s_need_station_rebuild = true;
    } else if (view == VIEW_WORD_CLOCK) {
        s_force_clock_update = true;
        s_last_wc_status_str[0] = '\0';
    } else if (view == VIEW_AGY_DASHBOARD) {
        s_force_clock_update = true;
        s_force_quota_redraw = true;
    }
}

static ui_view_t parse_view_name(const char *name, bool *valid)
{
    if (valid) *valid = false;
    if (!name) return VIEW_STOPPED;
    if (strcmp(name, "player") == 0) {
        if (valid) *valid = true;
        return VIEW_PLAYER;
    }
    if (strcmp(name, "wordclock") == 0) {
        if (valid) *valid = true;
        return VIEW_WORD_CLOCK;
    }
    if (strcmp(name, "agy") == 0) {
        if (valid) *valid = true;
        return VIEW_AGY_DASHBOARD;
    }
    if (strcmp(name, "stopped") == 0) {
        if (valid) *valid = true;
        return VIEW_STOPPED;
    }
    if (strcmp(name, "sleep") == 0) {
        if (valid) *valid = true;
        return VIEW_SLEEP;
    }
    return VIEW_STOPPED;
}

const char *display_ui_get_view_name(void)
{
    switch (s_cur_view) {
        case VIEW_PLAYER:        return "player";
        case VIEW_WORD_CLOCK:    return "wordclock";
        case VIEW_AGY_DASHBOARD: return "agy";
        case VIEW_STOPPED:       return "stopped";
        case VIEW_SLEEP:         return "sleep";
        default:                 return "stopped";
    }
}

/* ---------- Sleep & Wake ---------- */
static void enter_sleep_mode(void)
{
    audio_player_stop();
    s_sleep_entered_us = esp_timer_get_time();
    s_sleep_touch_armed = false;
    switch_view(VIEW_SLEEP);

    set_backlight_raw(0);
    if (s_lcd_panel) {
        esp_lcd_panel_disp_on_off(s_lcd_panel, false);
    }
    ESP_LOGI(TAG, "Entered sleep mode");
}

static void wake_from_sleep(ui_view_t target_view)
{
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);

    s_sleep_touch_armed = false;
    s_last_activity_us = esp_timer_get_time();
    switch_view(target_view);
    if (s_lcd_panel) {
        esp_lcd_panel_disp_on_off(s_lcd_panel, true);
    }
    display_ui_set_brightness(cfg.brightness);
    ESP_LOGI(TAG, "Woke from sleep mode");
}

static void apply_requested_view(ui_view_t req)
{
    player_status_t pst;
    audio_player_get_status(&pst);
    s_last_activity_us = esp_timer_get_time();

    if (req == VIEW_SLEEP) {
        enter_sleep_mode();
        return;
    }
    if (req == VIEW_PLAYER && pst.state == PLAYER_STATE_STOPPED) {
        req = VIEW_STOPPED;
    }
    if (s_cur_view == VIEW_SLEEP) {
        wake_from_sleep(req);
    } else {
        switch_view(req);
    }
    if (req == VIEW_PLAYER && pst.state != PLAYER_STATE_STOPPED) {
        show_station_banner(pst.station_index, pst.station_name);
    }
}

void display_ui_switch_view_by_name(const char *name)
{
    bool valid = false;
    ui_view_t target = parse_view_name(name, &valid);
    if (!valid) return;

    if (s_scr_main && lvgl_port_lock(300)) {
        apply_requested_view(target);
        lvgl_port_unlock();
    } else {
        s_pending_view = (int)target;
        s_has_pending_view = true;
    }
}

static void on_sleep_screen_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        if ((esp_timer_get_time() - s_sleep_entered_us) >= SLEEP_WAKE_GUARD_US) {
            s_sleep_touch_armed = true;
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_sleep_touch_armed) {
            wake_from_sleep(VIEW_STOPPED);
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        s_sleep_touch_armed = false;
    }
}

/* ---------- Gesture Actions (Player / Word Clock / Agy Dashboard) ---------- */
static void cycle_view_horizontal(int dir)
{
    player_status_t pst;
    audio_player_get_status(&pst);
    bool is_playing = (pst.state != PLAYER_STATE_STOPPED);
    ui_view_t base = is_playing ? VIEW_PLAYER : VIEW_STOPPED;

    ui_view_t ring[3] = { base, VIEW_WORD_CLOCK, VIEW_AGY_DASHBOARD };
    int idx = 0;
    for (int i = 0; i < 3; i++) {
        if (s_cur_view == ring[i]) {
            idx = i;
            break;
        }
    }
    /* dir < 0: swipe left (forward); dir > 0: swipe right (backward) */
    int next_idx = (dir < 0) ? ((idx + 1) % 3) : ((idx + 2) % 3);
    switch_view(ring[next_idx]);
}

static void handle_long_press(void)
{
    player_status_t pst;
    audio_player_get_status(&pst);
    if (pst.state != PLAYER_STATE_STOPPED) {
        audio_player_stop();
        show_toast("Stopped");
    }
    switch_view(VIEW_STOPPED);
}

static void handle_double_tap(void)
{
    if (s_cur_view == VIEW_PLAYER) {
        audio_player_next();
        show_station_banner_current();
    } else if (s_cur_view == VIEW_WORD_CLOCK) {
        app_agy_config_t agy;
        app_config_get_agy(&agy);
        uint8_t next_mode = (uint8_t)((agy.wc_font_mode + 1) % 5);
        app_config_set_wc_font(next_mode);
        s_agy_cfg.wc_font_mode = next_mode;

        if (next_mode == 0) {
            wc_font_style_t next_style = (wc_font_style_t)((s_wc_font_style + 1) % WC_FONT_COUNT);
            wc_apply_font_style(next_style);
            show_toast("Font: Auto (5m)");
        } else {
            wc_font_style_t style = (wc_font_style_t)(next_mode - 1);
            wc_apply_font_style(style);
            char msg[48];
            snprintf(msg, sizeof(msg), "Font: %s", wc_font_get_name(style));
            show_toast(msg);
        }
    } else if (s_cur_view == VIEW_AGY_DASHBOARD) {
        google_quota_request_refresh();
        show_toast("Refreshing quota...");
    }
}

static void handle_single_tap(void)
{
    int64_t now_us = esp_timer_get_time();
    if (s_cur_view == VIEW_PLAYER) {
        show_station_banner_current();
        clock_display_time_t ct;
        sntp_time_get_current(&ct);
        if (ct.is_synced) {
            char msg[48];
            snprintf(msg, sizeof(msg), "%s %s  |  Hold: Stop", ct.time_str, ct.am_pm);
            show_toast(msg);
        } else {
            show_toast("Hold: Stop  |  2xTap: Next");
        }
    } else if (s_cur_view == VIEW_WORD_CLOCK) {
        if (s_last_wc_status_str[0] != '\0') {
            show_wc_status(s_last_wc_status_str, now_us);
        }
        clock_display_time_t ct;
        sntp_time_get_current(&ct);
        if (ct.is_synced) {
            char msg[48];
            snprintf(msg, sizeof(msg), "%s %s  |  2xTap: Font", ct.time_str, ct.am_pm);
            show_toast(msg);
        } else {
            show_toast("2xTap: Font  |  Hold: Menu");
        }
    } else if (s_cur_view == VIEW_AGY_DASHBOARD) {
        show_toast("2xTap: Refresh  |  Hold: Menu");
    }
}

static void on_unified_gesture_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt = s_gest_last;
    if (indev) {
        lv_indev_get_point(indev, &pt);
    }
    int64_t now_us = esp_timer_get_time();

    if (code == LV_EVENT_PRESSED) {
        s_last_activity_us = now_us;
        s_gest_active = true;
        s_gest_long_fired = false;
        s_gest_ignore_until_release = false;
        s_gest_press_us = now_us;
        s_gest_start = pt;
        s_gest_last = pt;
        s_gest_start_vol = (int)audio_player_get_volume();
        s_gest_start_bright = (int)s_cur_brightness;
        s_gest_mode = GEST_NONE;
        s_gest_max_dist = 0;
    } else if (code == LV_EVENT_PRESSING) {
        s_last_activity_us = now_us;
        if (s_gest_long_fired || s_gest_ignore_until_release || !s_gest_active) return;

        s_gest_last = pt;
        int dx = (int)pt.x - (int)s_gest_start.x;
        int dy = (int)pt.y - (int)s_gest_start.y;
        int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (dist > s_gest_max_dist) s_gest_max_dist = dist;

        if (s_gest_mode == GEST_NONE) {
            if (s_gest_max_dist < 16 && (now_us - s_gest_press_us) >= LONG_PRESS_US) {
                s_gest_long_fired = true;
                s_gest_ignore_until_release = true;
                s_tap_pending = false;
                handle_long_press();
                return;
            }
            if (abs(dy) >= 14 && abs(dy) > (abs(dx) * 6) / 5) {
                s_gest_mode = GEST_VERT;
                s_tap_pending = false;
            } else if (abs(dx) >= 24 && abs(dx) > (abs(dy) * 6) / 5) {
                s_gest_mode = GEST_SWIPE_X;
                s_tap_pending = false;
            }
        }

        if (s_gest_mode == GEST_VERT) {
            if (s_cur_view == VIEW_PLAYER) {
                int target = s_gest_start_vol + (-dy * 100) / 150;
                if (target < 0) target = 0;
                if (target > 100) target = 100;
                audio_player_set_volume_live((uint8_t)target, false);
                show_vol_osd(target);
            } else if (s_cur_view == VIEW_WORD_CLOCK || s_cur_view == VIEW_AGY_DASHBOARD) {
                int target = s_gest_start_bright + (-dy * 90) / 150;
                if (target < 10) target = 10;
                if (target > 100) target = 100;
                display_ui_set_brightness((uint8_t)target);
                show_bright_osd(target);
            }
        }
    } else if (code == LV_EVENT_RELEASED) {
        s_last_activity_us = now_us;
        s_gest_last = pt;
        int dx = (int)pt.x - (int)s_gest_start.x;

        if (s_gest_long_fired || s_gest_ignore_until_release || !s_gest_active) {
            s_gest_long_fired = false;
            s_gest_ignore_until_release = false;
            s_gest_active = false;
            s_gest_mode = GEST_NONE;
            return;
        }
        s_gest_active = false;

        if (s_gest_mode == GEST_VERT) {
            if (s_cur_view == VIEW_PLAYER) {
                uint8_t cur_vol = audio_player_get_volume();
                audio_player_set_volume_live(cur_vol, true);
                show_vol_osd((int)cur_vol);
            } else if (s_cur_view == VIEW_WORD_CLOCK || s_cur_view == VIEW_AGY_DASHBOARD) {
                app_config_set_brightness(s_cur_brightness);
                show_bright_osd((int)s_cur_brightness);
            }
        } else if (s_gest_mode == GEST_SWIPE_X) {
            if (abs(dx) >= 35) {
                player_status_t pst;
                audio_player_get_status(&pst);
                bool is_playing = (pst.state != PLAYER_STATE_STOPPED);
                if (s_gest_start.y < 44 && is_playing) {
                    if (dx <= -35) {
                        audio_player_next();
                    } else {
                        audio_player_prev();
                    }
                    show_station_banner_current();
                } else {
                    cycle_view_horizontal(dx < 0 ? -1 : 1);
                }
            }
        } else if (s_gest_mode == GEST_NONE && s_gest_max_dist < 16) {
            if (s_tap_pending && s_tap_view == s_cur_view &&
                (s_gest_press_us - s_tap_first_us) <= DOUBLE_TAP_US &&
                abs((int)s_gest_start.x - (int)s_tap_pt.x) < 28 &&
                abs((int)s_gest_start.y - (int)s_tap_pt.y) < 28) {
                s_tap_pending = false;
                handle_double_tap();
            } else {
                s_tap_pending = true;
                s_tap_first_us = now_us;
                s_tap_view = s_cur_view;
                s_tap_pt = s_gest_start;
            }
        }
        s_gest_mode = GEST_NONE;
    } else if (code == LV_EVENT_PRESS_LOST) {
        if (s_gest_long_fired || s_gest_ignore_until_release || !s_gest_active) {
            s_gest_long_fired = false;
            s_gest_ignore_until_release = false;
            s_gest_active = false;
            s_gest_mode = GEST_NONE;
            return;
        }
        s_gest_active = false;
        if (s_gest_mode == GEST_VERT) {
            if (s_cur_view == VIEW_PLAYER) {
                audio_player_set_volume_live(audio_player_get_volume(), true);
            } else if (s_cur_view == VIEW_WORD_CLOCK || s_cur_view == VIEW_AGY_DASHBOARD) {
                app_config_set_brightness(s_cur_brightness);
            }
        }
        s_gest_mode = GEST_NONE;
        s_gest_long_fired = false;
        s_gest_ignore_until_release = false;
    }
}

/* ---------- Stopped Screen Swipe & Button Handlers ---------- */
static void on_stopped_swipe_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt = s_stop_gest_start;
    if (indev) {
        lv_indev_get_point(indev, &pt);
    }
    int64_t now_us = esp_timer_get_time();

    if (code == LV_EVENT_PRESSED) {
        s_last_activity_us = now_us;
        s_stop_gest_active = true;
        s_stop_swipe_blocked_click = false;
        s_stop_gest_start = pt;
    } else if (code == LV_EVENT_PRESSING) {
        s_last_activity_us = now_us;
        if (s_stop_gest_active) {
            int dx = abs((int)pt.x - (int)s_stop_gest_start.x);
            int dy = abs((int)pt.y - (int)s_stop_gest_start.y);
            if (dx >= 24 && (dx * 2) > (dy * 3)) {
                s_stop_swipe_blocked_click = true;
            }
        }
    } else if (code == LV_EVENT_RELEASED) {
        s_last_activity_us = now_us;
        if (s_stop_gest_active) {
            s_stop_gest_active = false;
            int dx = (int)pt.x - (int)s_stop_gest_start.x;
            int dy = (int)pt.y - (int)s_stop_gest_start.y;
            if (abs(dx) >= 45 && (abs(dx) * 2) > (abs(dy) * 3)) {
                s_stop_swipe_blocked_click = true;
                cycle_view_horizontal(dx < 0 ? -1 : 1);
            }
        }
    } else if (code == LV_EVENT_PRESS_LOST) {
        s_stop_gest_active = false;
    }
}

static void on_play_resume_clicked(lv_event_t *e)
{
    (void)e;
    if (s_stop_swipe_blocked_click) return;
    s_last_activity_us = esp_timer_get_time();
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);
    audio_player_play_station(cfg.current_station);
    switch_view(VIEW_PLAYER);
    show_station_banner_current();
}

static void on_tab_stations_clicked(lv_event_t *e)
{
    (void)e;
    if (s_stop_swipe_blocked_click) return;
    s_last_activity_us = esp_timer_get_time();
    switch_stopped_tab(STOP_TAB_STATIONS);
}

static void on_tab_settings_clicked(lv_event_t *e)
{
    (void)e;
    if (s_stop_swipe_blocked_click) return;
    s_last_activity_us = esp_timer_get_time();
    switch_stopped_tab(STOP_TAB_SETTINGS);
}

static void on_sleep_clicked(lv_event_t *e)
{
    (void)e;
    if (s_stop_swipe_blocked_click) return;
    s_last_activity_us = esp_timer_get_time();
    enter_sleep_mode();
}

static void on_station_item_clicked(lv_event_t *e)
{
    if (s_stop_swipe_blocked_click) return;
    s_last_activity_us = esp_timer_get_time();
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    audio_player_play_station(idx);
    switch_view(VIEW_PLAYER);
    show_station_banner_current();
}

static void on_start_ap_clicked(lv_event_t *e)
{
    (void)e;
    if (s_stop_swipe_blocked_click) return;
    s_last_activity_us = esp_timer_get_time();
    wifi_manager_start_ap_mode();
}

static void rebuild_station_list(void)
{
    if (!s_list_stations) return;
    lv_obj_clean(s_list_stations);

    int count = app_config_get_station_count();
    player_status_t pst;
    audio_player_get_status(&pst);
    radio_station_t st;

    for (int i = 0; i < count; i++) {
        if (!app_config_get_station(i, &st)) continue;
        bool active = (i == pst.station_index);

        lv_obj_t *btn = lv_button_create(s_list_stations);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 38);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, active ? lv_color_hex(0x0369A1) : lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_border_width(btn, active ? 1 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x38BDF8), 0);
        lv_obj_set_style_pad_hor(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_event_bubble(btn, true);
        lv_obj_add_event_cb(btn, on_station_item_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        char line[96];
        snprintf(line, sizeof(line), "%d. %s", i + 1, st.name);
        lv_label_set_text(lbl, line);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xF8FAFC), 0);
        lv_obj_center(lbl);
        lv_obj_set_clickable(lbl, false);
    }
}

void display_ui_refresh_stations(void)
{
    s_need_station_rebuild = true;
}

static lv_obj_t *create_hdr_btn(lv_obj_t *parent, const char *txt, int w, int h,
                                uint32_t bg_hex, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 7, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_event_bubble(btn, true);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
    lv_obj_set_clickable(lbl, false);
    return btn;
}

/* ---------- Agy Dashboard Quota Slot Helpers (LVGL v9) ---------- */
static lv_color_t get_quota_color_by_val(int val)
{
    if (val < 10) {
        return lv_color_hex(0xef4444);
    } else if (val < 30) {
        return lv_color_hex(0xf97316);
    } else if (val < 50) {
        return lv_color_hex(0xeab308);
    } else {
        return lv_color_hex(0x22c55e);
    }
}

static void set_bar_color_by_val(lv_obj_t *bar, int val)
{
    lv_color_t color = get_quota_color_by_val(val);
    lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_NONE, LV_PART_INDICATOR);
}

static void create_quota_slot(lv_obj_t *parent, int y_slot,
                              lv_color_t title_color, const char *default_title,
                              quota_slot_ui_t *slot, bool add_divider)
{
    slot->title = lv_label_create(parent);
    lv_label_set_text(slot->title, default_title);
    lv_obj_set_style_text_color(slot->title, title_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(slot->title, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(slot->title, LV_ALIGN_TOP_LEFT, 2, y_slot);
    lv_obj_set_clickable(slot->title, false);

    slot->val = lv_label_create(parent);
    lv_label_set_text(slot->val, "--%");
    lv_obj_set_style_text_color(slot->val, lv_color_hex(0x94a3b8), LV_PART_MAIN);
    lv_obj_set_style_text_font(slot->val, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(slot->val, LV_ALIGN_TOP_RIGHT, -2, y_slot);
    lv_obj_set_clickable(slot->val, false);

    slot->bar = lv_bar_create(parent);
    lv_obj_set_size(slot->bar, 150, 7);
    lv_obj_align(slot->bar, LV_ALIGN_TOP_LEFT, 2, y_slot + 16);
    lv_bar_set_range(slot->bar, 0, 100);
    lv_bar_set_value(slot->bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(slot->bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(slot->bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slot->bar, lv_color_hex(0x27272a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slot->bar, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slot->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slot->bar, lv_color_hex(0x3f3f46), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(slot->bar, LV_GRAD_DIR_NONE, LV_PART_INDICATOR);
    lv_obj_set_clickable(slot->bar, false);

    slot->reset = lv_label_create(parent);
    lv_label_set_text(slot->reset, "Waiting data...");
    lv_obj_set_width(slot->reset, 150);
    lv_label_set_long_mode(slot->reset, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(slot->reset, lv_color_hex(0xd4d4d8), LV_PART_MAIN);
    lv_obj_set_style_text_font(slot->reset, agy_small_font_get(), LV_PART_MAIN);
    lv_obj_align(slot->reset, LV_ALIGN_TOP_LEFT, 2, y_slot + 26);
    lv_obj_set_clickable(slot->reset, false);

    if (add_divider) {
        lv_obj_t *div = lv_obj_create(parent);
        lv_obj_set_size(div, 150, 1);
        lv_obj_set_style_bg_color(div, lv_color_hex(0x3f3f46), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(div, LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_border_width(div, 0, LV_PART_MAIN);
        lv_obj_align(div, LV_ALIGN_TOP_LEFT, 2, y_slot + 41);
        lv_obj_set_clickable(div, false);
    }
}

static void update_quota_slot(quota_slot_ui_t *slot, const quota_window_t *window)
{
    if (!slot || !window || !slot->title || !slot->bar || !slot->val || !slot->reset) return;

    if (window->name[0] != '\0' && strcmp(lv_label_get_text(slot->title), window->name) != 0) {
        lv_label_set_text(slot->title, window->name);
    }

    if (!window->valid) {
        lv_bar_set_value(slot->bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slot->bar, lv_color_hex(0x3f3f46), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(slot->bar, LV_GRAD_DIR_NONE, LV_PART_INDICATOR);
        lv_label_set_text(slot->val, "--%");
        lv_obj_set_style_text_color(slot->val, lv_color_hex(0x94a3b8), LV_PART_MAIN);
    } else {
        int p = (int)(window->remaining_fraction * 100.0f);
        if (p < 0) p = 0;
        if (p > 100) p = 100;

        lv_bar_set_value(slot->bar, p, LV_ANIM_OFF);
        set_bar_color_by_val(slot->bar, p);
        lv_label_set_text_fmt(slot->val, "%d%%", p);
        lv_obj_set_style_text_color(slot->val, get_quota_color_by_val(p), LV_PART_MAIN);
    }

    lv_obj_set_style_text_color(slot->reset, lv_color_hex(0xd4d4d8), LV_PART_MAIN);
    if (strcmp(lv_label_get_text(slot->reset), window->reset_time_str) != 0) {
        lv_label_set_text(slot->reset, window->reset_time_str);
    }
}

/* ---------- Periodic Clock & Quota Sync Helpers ---------- */
static void update_word_clock_view(const player_status_t *pst, const wifi_mgr_status_t *wst, int64_t now_us)
{
    struct tm tm_local;
    if (sntp_time_get_local(&tm_local)) {
        if (s_last_wc_hour != tm_local.tm_hour || s_last_wc_min != tm_local.tm_min || s_force_clock_update) {
            s_last_wc_hour = tm_local.tm_hour;
            s_last_wc_min = tm_local.tm_min;

            char buf[64];
            word_clock_get_text(tm_local.tm_hour, tm_local.tm_min, buf, sizeof(buf));
            if (strcmp(lv_label_get_text(s_wc_time_label), buf) != 0) {
                lv_label_set_text(s_wc_time_label, buf);
                for (int i = 0; i < OUTLINE_OFFSETS; i++) {
                    lv_label_set_text(s_wc_outline[i], buf);
                }
            }

            int block_min = (tm_local.tm_min / 5) * 5;
            if (s_last_wc_block_min != block_min) {
                if (s_last_wc_block_min != -1 && s_agy_cfg.wc_font_mode == 0) {
                    wc_apply_font_style((wc_font_style_t)((s_wc_font_style + 1) % WC_FONT_COUNT));
                }
                s_last_wc_block_min = block_min;
            }
        }
    } else if (s_last_wc_hour != -2) {
        s_last_wc_hour = -2;
        const char *wait_txt = "Syncing\nNTP...";
        lv_label_set_text(s_wc_time_label, wait_txt);
        for (int i = 0; i < OUTLINE_OFFSETS; i++) {
            lv_label_set_text(s_wc_outline[i], wait_txt);
        }
    }

    char status_buf[256];
    if (pst->state != PLAYER_STATE_STOPPED) {
        if (pst->stream_track[0] && pst->stream_artist[0]) {
            snprintf(status_buf, sizeof(status_buf), "%s - %s", pst->stream_artist, pst->stream_track);
        } else if (pst->stream_title[0]) {
            snprintf(status_buf, sizeof(status_buf), "%s", pst->stream_title);
        } else if (pst->station_name[0]) {
            snprintf(status_buf, sizeof(status_buf), "%s", pst->station_name);
        } else {
            snprintf(status_buf, sizeof(status_buf), "Playing...");
        }
    } else {
        if (wst->state == WIFI_MGR_CONNECTED_STA) {
            snprintf(status_buf, sizeof(status_buf), "IP: %s", wst->ip_addr);
        } else if (wst->state == WIFI_MGR_AP_CONFIG_MODE) {
            snprintf(status_buf, sizeof(status_buf), "AP: %s (%s)", wst->ap_ssid, wst->ap_ip);
        } else {
            snprintf(status_buf, sizeof(status_buf), "Wi-Fi: Connecting...");
        }
    }

    if (strcmp(s_last_wc_status_str, status_buf) != 0) {
        snprintf(s_last_wc_status_str, sizeof(s_last_wc_status_str), "%s", status_buf);
        show_wc_status(status_buf, now_us);
    }
}

static void update_agy_dashboard_view(const player_status_t *pst, const wifi_mgr_status_t *wst)
{
    clock_display_time_t ct;
    sntp_time_get_current(&ct);

    if (ct.is_synced) {
        if (strcmp(lv_label_get_text(s_db_label_time), ct.time_str) != 0) {
            lv_label_set_text(s_db_label_time, ct.time_str);
            lv_obj_align_to(s_db_label_ampm, s_db_label_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);
        }
        if (strcmp(lv_label_get_text(s_db_label_ampm), ct.am_pm) != 0) {
            lv_label_set_text(s_db_label_ampm, ct.am_pm);
            lv_obj_align_to(s_db_label_ampm, s_db_label_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);
        }
        if (strcmp(lv_label_get_text(s_db_label_date), ct.date_str) != 0) {
            lv_label_set_text(s_db_label_date, ct.date_str);
        }
    } else {
        if (strcmp(lv_label_get_text(s_db_label_time), "--:--") != 0) {
            lv_label_set_text(s_db_label_time, "--:--");
            lv_label_set_text(s_db_label_ampm, "");
            lv_label_set_text(s_db_label_date, "Syncing NTP...");
        }
    }

    char wifi_buf[48];
    lv_color_t wifi_col;
    if (wst->state == WIFI_MGR_CONNECTED_STA) {
        snprintf(wifi_buf, sizeof(wifi_buf), "IP: %s", wst->ip_addr);
        wifi_col = lv_color_hex(0x4ade80);
    } else if (wst->state == WIFI_MGR_AP_CONFIG_MODE) {
        snprintf(wifi_buf, sizeof(wifi_buf), "AP: %s", wst->ap_ip);
        wifi_col = lv_color_hex(0xf59e0b);
    } else {
        snprintf(wifi_buf, sizeof(wifi_buf), "Wi-Fi: Offline");
        wifi_col = lv_color_hex(0xf87171);
    }
    if (strcmp(lv_label_get_text(s_db_label_wifi), wifi_buf) != 0) {
        lv_label_set_text(s_db_label_wifi, wifi_buf);
    }
    lv_obj_set_style_text_color(s_db_label_wifi, wifi_col, LV_PART_MAIN);

    char st_buf[192];
    lv_color_t st_col;
    if (pst->state != PLAYER_STATE_STOPPED) {
        const char *txt = pst->stream_track[0] ? pst->stream_track
                          : (pst->stream_title[0] ? pst->stream_title : pst->station_name);
        snprintf(st_buf, sizeof(st_buf), LV_SYMBOL_AUDIO " %s", txt[0] ? txt : "Playing");
        st_col = lv_color_hex(0x38bdf8);
    } else if (wst->state == WIFI_MGR_AP_CONFIG_MODE) {
        snprintf(st_buf, sizeof(st_buf), "AP: %s", wst->ap_ssid);
        st_col = lv_color_hex(0xf59e0b);
    } else {
        snprintf(st_buf, sizeof(st_buf), "Stopped");
        st_col = lv_color_hex(0x94a3b8);
    }
    if (strcmp(lv_label_get_text(s_db_label_status), st_buf) != 0) {
        lv_label_set_text(s_db_label_status, st_buf);
    }
    lv_obj_set_style_text_color(s_db_label_status, st_col, LV_PART_MAIN);

    google_quota_data_t qd;
    google_quota_get_data(&qd);
    bool changed = s_force_quota_redraw ||
                   (qd.last_update_sec != s_q_last_update_sec) ||
                   (qd.is_loading != s_q_last_loading) ||
                   (qd.has_error != s_q_last_has_error) ||
                   (strcmp(qd.error_msg, s_q_last_err_msg) != 0);

    if (changed) {
        s_force_quota_redraw = false;
        s_q_last_update_sec = qd.last_update_sec;
        s_q_last_loading = qd.is_loading;
        s_q_last_has_error = qd.has_error;
        snprintf(s_q_last_err_msg, sizeof(s_q_last_err_msg), "%s", qd.error_msg);

        update_quota_slot(&s_q_slots[0], &qd.limit_5h);
        update_quota_slot(&s_q_slots[1], &qd.limit_weekly);
        update_quota_slot(&s_q_slots[2], &qd.limit_claude_5h);
        update_quota_slot(&s_q_slots[3], &qd.limit_claude_wk);
        update_quota_slot(&s_q_slots[4], &qd.limit_argon);

        if (qd.has_error) {
            for (int i = 0; i < 5; i++) {
                lv_obj_set_style_text_color(s_q_slots[i].reset, lv_color_hex(0xf87171), LV_PART_MAIN);
                lv_label_set_text(s_q_slots[i].reset, qd.error_msg);
            }
        } else if (qd.is_loading && qd.last_update_sec == 0) {
            for (int i = 0; i < 5; i++) {
                lv_obj_set_style_text_color(s_q_slots[i].reset, lv_color_hex(0xd4d4d8), LV_PART_MAIN);
                lv_label_set_text(s_q_slots[i].reset, "Fetching quota...");
            }
        }
    }
}

/* ---------- 50ms Main UI Timer ---------- */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int64_t now_us = esp_timer_get_time();

    if (s_need_agy_apply) {
        s_need_agy_apply = false;
        app_config_get_agy(&s_agy_cfg);
        if (s_agy_cfg.wc_font_mode >= 1 && s_agy_cfg.wc_font_mode <= WC_FONT_COUNT) {
            wc_apply_font_style((wc_font_style_t)(s_agy_cfg.wc_font_mode - 1));
        }
        s_force_clock_update = true;
        s_force_quota_redraw = true;
        s_last_activity_us = now_us;
    }

    if (s_has_pending_view) {
        ui_view_t req = (ui_view_t)s_pending_view;
        s_has_pending_view = false;
        apply_requested_view(req);
    }

    if (s_need_station_rebuild) {
        s_need_station_rebuild = false;
        rebuild_station_list();
    }

    player_status_t pst;
    audio_player_get_status(&pst);

    static wifi_mgr_status_t s_cached_wst = {0};
    static uint32_t s_wifi_poll_div = 0;
    bool poll_wifi = false;
    if (s_cur_view == VIEW_PLAYER || s_cur_view == VIEW_SLEEP) {
        s_wifi_poll_div = 0;
    } else if ((s_wifi_poll_div++ % 10U) == 0U || s_force_clock_update) {
        poll_wifi = true;
        wifi_manager_get_status(&s_cached_wst);
    }

    static bool s_ap_tab_auto_shown = false;
    if (poll_wifi && s_cached_wst.state == WIFI_MGR_AP_CONFIG_MODE) {
        if (!s_ap_tab_auto_shown && s_cur_view == VIEW_STOPPED) {
            s_ap_tab_auto_shown = true;
            switch_stopped_tab(STOP_TAB_SETTINGS);
        }
    } else if (poll_wifi) {
        s_ap_tab_auto_shown = false;
    }

    /* Playback state transition sync */
    bool was_stopped = (s_prev_player_state == PLAYER_STATE_STOPPED);
    bool is_stopped = (pst.state == PLAYER_STATE_STOPPED);
    if (s_prev_player_state != pst.state) {
        s_last_activity_us = now_us;
    }

    if (s_cur_view == VIEW_SLEEP) {
        if (!is_stopped) {
            s_prev_player_state = pst.state;
            wake_from_sleep(VIEW_PLAYER);
            show_station_banner(pst.station_index, pst.station_name);
        } else {
            s_prev_player_state = pst.state;
            return;
        }
    } else if (was_stopped && !is_stopped) {
        if (s_cur_view == VIEW_STOPPED) {
            switch_view(VIEW_PLAYER);
            show_station_banner(pst.station_index, pst.station_name);
        } else if (s_cur_view == VIEW_WORD_CLOCK || s_cur_view == VIEW_AGY_DASHBOARD) {
            update_bg_art_visuals(pst.state);
            show_station_banner(pst.station_index, pst.station_name);
        }
    } else if (!was_stopped && is_stopped) {
        if (s_cur_view == VIEW_PLAYER) {
            switch_view(VIEW_STOPPED);
        } else if (s_cur_view == VIEW_WORD_CLOCK || s_cur_view == VIEW_AGY_DASHBOARD) {
            update_bg_art_visuals(pst.state);
        }
    }
    s_prev_player_state = pst.state;

    /* Long-press check while finger is stationary */
    if (s_gest_active && !s_gest_long_fired && !s_gest_ignore_until_release &&
        s_gest_mode == GEST_NONE && s_gest_max_dist < 16) {
        if ((now_us - s_gest_press_us) >= LONG_PRESS_US) {
            s_gest_long_fired = true;
            s_gest_ignore_until_release = true;
            s_tap_pending = false;
            handle_long_press();
        }
    }

    /* Single-tap resolution after 280ms double-tap window expires */
    if (s_tap_pending && !s_gest_active && (now_us - s_tap_first_us) > DOUBLE_TAP_US) {
        s_tap_pending = false;
        if (s_tap_view == s_cur_view) {
            handle_single_tap();
        }
    }

    /* Idle auto-clock transition from VIEW_STOPPED */
    if (s_cur_view == VIEW_STOPPED && s_agy_cfg.idle_clock_sec > 0) {
        int64_t idle_us = now_us - s_last_activity_us;
        if (idle_us >= (int64_t)s_agy_cfg.idle_clock_sec * 1000000LL) {
            switch_view(s_agy_cfg.idle_clock_view == 1 ? VIEW_AGY_DASHBOARD : VIEW_WORD_CLOCK);
        }
    }

    /* Auto-hide temporary overlays */
    if (s_station_banner && !lv_obj_is_hidden(s_station_banner) && now_us >= s_station_hide_us) {
        lv_obj_set_hidden(s_station_banner, true);
    }
    if (s_osd_box && !lv_obj_is_hidden(s_osd_box) &&
        s_gest_mode != GEST_VERT && now_us >= s_osd_hide_us) {
        lv_obj_set_hidden(s_osd_box, true);
    }
    if (s_toast && !lv_obj_is_hidden(s_toast) && now_us >= s_toast_hide_us) {
        lv_obj_set_hidden(s_toast, true);
    }
    if (s_wc_status_label && !lv_obj_is_hidden(s_wc_status_label) && now_us >= s_wc_status_hide_us) {
        lv_obj_set_hidden(s_wc_status_label, true);
        if (s_wc_status_shadow) lv_obj_set_hidden(s_wc_status_shadow, true);
    }

    /* Detect station switches initiated externally (e.g. Web UI) while playing */
    if (!is_stopped && pst.station_index != s_last_station_idx) {
        s_last_activity_us = now_us;
        show_station_banner(pst.station_index, pst.station_name);
    }

    /* Refresh shared artwork image if version changed */
    if (s_art_pixels && s_bg_art &&
        (s_cur_view == VIEW_PLAYER || s_cur_view == VIEW_WORD_CLOCK || s_cur_view == VIEW_AGY_DASHBOARD)) {
        uint32_t cur_ver = album_art_get_version();
        if (cur_ver != s_shown_art_ver) {
            bool is_cover = false;
            if (album_art_copy_rgb565(s_art_pixels, ALBUM_ART_BYTES, &is_cover, &s_shown_art_ver)) {
                s_art_ready = true;
                s_has_art = is_cover;
                lv_image_cache_drop(&s_art_dsc);
                update_bg_art_visuals(pst.state);
                lv_obj_invalidate(s_bg_art);
            }
        }
    }

    /* Per-view content updates */
    if (s_cur_view == VIEW_PLAYER) {
        const char *track_txt = NULL;
        const char *artist_txt = "";
        const lv_font_t *track_font = &lv_font_montserrat_20;

        if (pst.stream_track[0] != '\0') {
            track_txt = pst.stream_track;
            artist_txt = pst.stream_artist;
            if (strlen(track_txt) > 36) {
                track_font = &lv_font_montserrat_16;
            }
        } else if (pst.stream_title[0] != '\0') {
            track_txt = pst.stream_title;
            if (strlen(track_txt) > 36) {
                track_font = &lv_font_montserrat_16;
            }
        } else {
            track_txt = pst.status_msg[0] ? pst.status_msg : "Connecting...";
            track_font = &lv_font_montserrat_16;
        }

        outlined_label_set_font(&s_ol_track, track_font);
        outlined_label_set_text(&s_ol_track, track_txt);

        if (artist_txt[0] != '\0') {
            outlined_label_set_hidden(&s_ol_artist, false);
            outlined_label_set_color(&s_ol_artist, lv_color_hex(0x7DD3FC));
            outlined_label_set_text(&s_ol_artist, artist_txt);
        } else {
            outlined_label_set_hidden(&s_ol_artist, true);
        }
    } else if (s_cur_view == VIEW_WORD_CLOCK || s_cur_view == VIEW_AGY_DASHBOARD) {
        if (s_force_clock_update || (now_us - s_last_clock_poll_us) >= CLOCK_POLL_US) {
            s_last_clock_poll_us = now_us;
            app_config_get_agy(&s_agy_cfg);
            if (s_cur_view == VIEW_WORD_CLOCK) {
                update_word_clock_view(&pst, &s_cached_wst, now_us);
            } else {
                update_agy_dashboard_view(&pst, &s_cached_wst);
            }
            s_force_clock_update = false;
        }
    } else if (poll_wifi && s_cur_view == VIEW_STOPPED && s_stop_tab == STOP_TAB_SETTINGS) {
        if (s_cached_wst.state == WIFI_MGR_CONNECTED_STA) {
            lv_label_set_text(s_lbl_net_mode, "Wi-Fi Station (Connected)");
            lv_label_set_text_fmt(s_lbl_net_detail,
                                  "SSID: %s\nIP: %s\nSignal: %d dBm",
                                  s_cached_wst.ssid, s_cached_wst.ip_addr, s_cached_wst.rssi);
            lv_label_set_text_fmt(s_lbl_net_hint,
                                  "Web Config:\nhttp://%s/\nhttp://%s.local/",
                                  s_cached_wst.ip_addr, APP_MDNS_HOSTNAME);
        } else if (s_cached_wst.state == WIFI_MGR_AP_CONFIG_MODE) {
            lv_label_set_text(s_lbl_net_mode, "SoftAP Provisioning");
            lv_label_set_text_fmt(s_lbl_net_detail,
                                  "AP SSID: %s (Open)\nAP IP: %s",
                                  s_cached_wst.ap_ssid, s_cached_wst.ap_ip);
            lv_label_set_text(s_lbl_net_hint,
                              "1. Connect phone/PC to 'ES3C28P-Radio'\n2. Open browser: http://192.168.4.1/");
        } else {
            lv_label_set_text(s_lbl_net_mode, "Connecting Wi-Fi...");
            lv_label_set_text_fmt(s_lbl_net_detail, "Target SSID: %s", s_cached_wst.ssid);
            lv_label_set_text(s_lbl_net_hint, "Please wait...");
        }
    }
}

/* ---------- UI Construction ---------- */
static void build_ui(void)
{
    app_config_get_agy(&s_agy_cfg);
    s_last_activity_us = esp_timer_get_time();

    s_scr_main = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr_main, lv_color_hex(0x09090B), 0);
    lv_obj_set_style_pad_all(s_scr_main, 0, 0);
    lv_obj_set_scrollable(s_scr_main, false);

    /* Allocate full 320x240 RGB565 artwork pixel buffer in PSRAM */
    s_art_pixels = heap_caps_malloc(ALBUM_ART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_art_pixels) {
        s_art_pixels = malloc(ALBUM_ART_BYTES);
    }
    if (s_art_pixels) {
        bool is_cover = false;
        if (album_art_copy_rgb565(s_art_pixels, ALBUM_ART_BYTES, &is_cover, &s_shown_art_ver)) {
            s_art_ready = true;
            s_has_art = is_cover;
        }
        s_art_dsc = (lv_image_dsc_t){
            .header = {
                .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0,
                .w = ALBUM_ART_WIDTH,
                .h = ALBUM_ART_HEIGHT,
                .stride = ALBUM_ART_WIDTH * sizeof(uint16_t),
            },
            .data_size = ALBUM_ART_BYTES,
            .data = (const uint8_t *)s_art_pixels,
        };
    }

    /* Shared 320x240 album art backdrop (behind Player, Word Clock, and Agy Dashboard) */
    s_bg_art = lv_image_create(s_scr_main);
    if (s_art_pixels) {
        lv_image_set_src(s_bg_art, &s_art_dsc);
    }
    lv_obj_set_size(s_bg_art, ALBUM_ART_WIDTH, ALBUM_ART_HEIGHT);
    lv_obj_align(s_bg_art, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_image_recolor(s_bg_art, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(s_bg_art, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(s_bg_art, false);

    /* ---------- View 1: Minimalist Full-Bleed Artwork Player (320x240, ZERO buttons) ---------- */
    s_panel_player = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_player, 320, 240);
    lv_obj_align(s_panel_player, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_panel_player, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel_player, 0, 0);
    lv_obj_set_style_pad_all(s_panel_player, 0, 0);
    lv_obj_set_scrollable(s_panel_player, false);
    lv_obj_set_clickable(s_panel_player, true);
    lv_obj_add_event_cb(s_panel_player, on_unified_gesture_event, LV_EVENT_ALL, NULL);

    /* Smooth bottom dark scrim so overlapping track/artist remains crisp over bright art */
    static const lv_color_t scrim_colors[2] = {
        LV_COLOR_MAKE(0, 0, 0),
        LV_COLOR_MAKE(0, 0, 0),
    };
    static const lv_opa_t scrim_opa[2] = { LV_OPA_TRANSP, LV_OPA_80 };
    lv_grad_init_stops(&s_scrim_grad, scrim_colors, scrim_opa, NULL, 2);
    lv_grad_vertical_init(&s_scrim_grad);

    lv_obj_t *scrim = lv_obj_create(s_panel_player);
    lv_obj_set_size(scrim, 320, 110);
    lv_obj_align(scrim, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_set_style_bg_grad(scrim, &s_scrim_grad, 0);
    lv_obj_set_scrollable(scrim, false);
    lv_obj_set_clickable(scrim, false);

    /* Outlined Track Title & Artist Name near bottom */
    lv_obj_t *meta_box = lv_obj_create(s_panel_player);
    lv_obj_set_size(meta_box, 304, LV_SIZE_CONTENT);
    lv_obj_align(meta_box, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(meta_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta_box, 0, 0);
    lv_obj_set_style_pad_all(meta_box, 0, 0);
    lv_obj_set_flex_flow(meta_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(meta_box, 2, 0);
    lv_obj_set_scrollable(meta_box, false);
    lv_obj_set_clickable(meta_box, false);

    outlined_label_create(meta_box, &s_ol_track, 296, &lv_font_montserrat_20,
                          lv_color_hex(0xFFFFFF), LV_TEXT_ALIGN_CENTER);
    outlined_label_create(meta_box, &s_ol_artist, 296, &lv_font_montserrat_16,
                          lv_color_hex(0x7DD3FC), LV_TEXT_ALIGN_CENTER);

    /* ---------- View 2: Word Clock (320x240, ZERO buttons) ---------- */
    s_panel_wordclock = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_wordclock, 320, 240);
    lv_obj_align(s_panel_wordclock, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_panel_wordclock, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel_wordclock, 0, 0);
    lv_obj_set_style_pad_all(s_panel_wordclock, 0, 0);
    lv_obj_set_scrollable(s_panel_wordclock, false);
    lv_obj_set_clickable(s_panel_wordclock, true);
    lv_obj_set_hidden(s_panel_wordclock, true);
    lv_obj_add_event_cb(s_panel_wordclock, on_unified_gesture_event, LV_EVENT_ALL, NULL);

    if (s_agy_cfg.wc_font_mode >= 1 && s_agy_cfg.wc_font_mode <= WC_FONT_COUNT) {
        s_wc_font_style = (wc_font_style_t)(s_agy_cfg.wc_font_mode - 1);
    } else {
        s_wc_font_style = WC_FONT_MODERN_SANS;
    }
    const lv_font_t *wc_font = wc_font_get(s_wc_font_style);
    static const int8_t wc_offsets[OUTLINE_OFFSETS][2] = {
        {-2, -2}, { 0, -2}, { 2, -2},
        {-2,  0},           { 2,  0},
        {-2,  2}, { 0,  2}, { 2,  2}
    };

    for (int i = 0; i < OUTLINE_OFFSETS; i++) {
        s_wc_outline[i] = lv_label_create(s_panel_wordclock);
        lv_obj_set_width(s_wc_outline[i], 320 - 24);
        lv_obj_align(s_wc_outline[i], LV_ALIGN_CENTER, wc_offsets[i][0], wc_offsets[i][1]);
        lv_obj_set_style_text_align(s_wc_outline[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_wc_outline[i], lv_color_black(), 0);
        lv_obj_set_style_text_font(s_wc_outline[i], wc_font, 0);
        lv_label_set_text(s_wc_outline[i], "Syncing\nNTP...");
        lv_obj_set_clickable(s_wc_outline[i], false);
    }

    s_wc_time_label = lv_label_create(s_panel_wordclock);
    lv_obj_set_width(s_wc_time_label, 320 - 24);
    lv_obj_align(s_wc_time_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(s_wc_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_wc_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_wc_time_label, wc_font, 0);
    lv_label_set_text(s_wc_time_label, "Syncing\nNTP...");
    lv_obj_set_clickable(s_wc_time_label, false);

    s_wc_status_shadow = lv_label_create(s_panel_wordclock);
    lv_obj_set_width(s_wc_status_shadow, 304);
    lv_label_set_long_mode(s_wc_status_shadow, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wc_status_shadow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wc_status_shadow, LV_ALIGN_BOTTOM_MID, 1, -3);
    lv_obj_set_style_text_color(s_wc_status_shadow, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_wc_status_shadow, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_wc_status_shadow, "");
    lv_obj_set_clickable(s_wc_status_shadow, false);
    lv_obj_set_hidden(s_wc_status_shadow, true);

    s_wc_status_label = lv_label_create(s_panel_wordclock);
    lv_obj_set_width(s_wc_status_label, 304);
    lv_label_set_long_mode(s_wc_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wc_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wc_status_label, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_color(s_wc_status_label, lv_color_hex(0xf1f5f9), 0);
    lv_obj_set_style_text_font(s_wc_status_label, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_wc_status_label, "");
    lv_obj_set_clickable(s_wc_status_label, false);
    lv_obj_set_hidden(s_wc_status_label, true);

    /* ---------- View 3: Agy Dashboard (320x240, ZERO buttons) ---------- */
    s_panel_agy = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_agy, 320, 240);
    lv_obj_align(s_panel_agy, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_panel_agy, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel_agy, 0, 0);
    lv_obj_set_style_pad_all(s_panel_agy, 0, 0);
    lv_obj_set_scrollable(s_panel_agy, false);
    lv_obj_set_clickable(s_panel_agy, true);
    lv_obj_set_hidden(s_panel_agy, true);
    lv_obj_add_event_cb(s_panel_agy, on_unified_gesture_event, LV_EVENT_ALL, NULL);

    /* Left Card: Digital Clock & Status */
    lv_obj_t *clock_card = lv_obj_create(s_panel_agy);
    lv_obj_set_size(clock_card, 144, 232);
    lv_obj_set_pos(clock_card, 4, 4);
    lv_obj_set_style_bg_color(clock_card, lv_color_hex(0x09090B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clock_card, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_color(clock_card, lv_color_hex(0x3F3F46), LV_PART_MAIN);
    lv_obj_set_style_border_opa(clock_card, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(clock_card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(clock_card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(clock_card, 6, LV_PART_MAIN);
    lv_obj_set_scrollable(clock_card, false);
    lv_obj_set_clickable(clock_card, false);

    s_db_label_wifi = lv_label_create(clock_card);
    lv_label_set_text(s_db_label_wifi, "Wi-Fi: ...");
    lv_obj_set_width(s_db_label_wifi, 130);
    lv_label_set_long_mode(s_db_label_wifi, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_db_label_wifi, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_db_label_wifi, lv_color_hex(0xd4d4d8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_db_label_wifi, agy_small_font_get(), LV_PART_MAIN);
    lv_obj_align(s_db_label_wifi, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_clickable(s_db_label_wifi, false);

    s_db_label_time = lv_label_create(clock_card);
    lv_label_set_text(s_db_label_time, "--:--");
    lv_obj_set_style_text_color(s_db_label_time, lv_color_hex(0xffffff), LV_PART_MAIN);
#if defined(CONFIG_LV_FONT_MONTSERRAT_36) && CONFIG_LV_FONT_MONTSERRAT_36
    lv_obj_set_style_text_font(s_db_label_time, &lv_font_montserrat_36, LV_PART_MAIN);
#else
    lv_obj_set_style_text_font(s_db_label_time, &lv_font_montserrat_24, LV_PART_MAIN);
#endif
    lv_obj_align(s_db_label_time, LV_ALIGN_CENTER, -12, -22);
    lv_obj_set_clickable(s_db_label_time, false);

    s_db_label_ampm = lv_label_create(clock_card);
    lv_label_set_text(s_db_label_ampm, "");
    lv_obj_set_style_text_color(s_db_label_ampm, lv_color_hex(0x38BDF8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_db_label_ampm, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align_to(s_db_label_ampm, s_db_label_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -4);
    lv_obj_set_clickable(s_db_label_ampm, false);

    s_db_label_date = lv_label_create(clock_card);
    lv_label_set_text(s_db_label_date, "Syncing NTP...");
    lv_obj_set_style_text_color(s_db_label_date, lv_color_hex(0xe4e4e7), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_db_label_date, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_db_label_date, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_clickable(s_db_label_date, false);

    s_db_label_status = lv_label_create(clock_card);
    lv_label_set_text(s_db_label_status, "Stopped");
    lv_obj_set_width(s_db_label_status, 130);
    lv_label_set_long_mode(s_db_label_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_db_label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_db_label_status, lv_color_hex(0x94a3b8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_db_label_status, agy_small_font_get(), LV_PART_MAIN);
    lv_obj_align(s_db_label_status, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_clickable(s_db_label_status, false);

    /* Right Card: 5 Quota Bars */
    lv_obj_t *quota_card = lv_obj_create(s_panel_agy);
    lv_obj_set_size(quota_card, 164, 232);
    lv_obj_set_pos(quota_card, 152, 4);
    lv_obj_set_style_bg_color(quota_card, lv_color_hex(0x09090B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(quota_card, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_color(quota_card, lv_color_hex(0x3F3F46), LV_PART_MAIN);
    lv_obj_set_style_border_opa(quota_card, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(quota_card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(quota_card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(quota_card, 6, LV_PART_MAIN);
    lv_obj_set_scrollable(quota_card, false);
    lv_obj_set_clickable(quota_card, false);

    lv_color_t col_gemini = lv_color_hex(0x93c5fd);
    lv_color_t col_claude = lv_color_hex(0xfba75c);
    lv_color_t col_argon  = lv_color_hex(0xc084fc);

    create_quota_slot(quota_card,   2, col_gemini, "Gemini 5h", &s_q_slots[0], true);
    create_quota_slot(quota_card,  46, col_gemini, "Gemini Wk", &s_q_slots[1], true);
    create_quota_slot(quota_card,  90, col_claude, "Claude 5h", &s_q_slots[2], true);
    create_quota_slot(quota_card, 134, col_claude, "Claude Wk", &s_q_slots[3], true);
    create_quota_slot(quota_card, 178, col_argon,  "Argon Wk",  &s_q_slots[4], false);

    /* ---------- View 4: Unified Stopped Screen (Stations + Settings + Sleep) ---------- */
    s_panel_stopped = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_stopped, 320, 240);
    lv_obj_align(s_panel_stopped, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel_stopped, lv_color_hex(0x0B1120), 0);
    lv_obj_set_style_border_width(s_panel_stopped, 0, 0);
    lv_obj_set_style_pad_all(s_panel_stopped, 0, 0);
    lv_obj_set_scrollable(s_panel_stopped, false);
    lv_obj_set_hidden(s_panel_stopped, true);
    lv_obj_add_event_cb(s_panel_stopped, on_stopped_swipe_event, LV_EVENT_ALL, NULL);

    /* Top Control Strip (320x42) */
    lv_obj_t *stop_hdr = lv_obj_create(s_panel_stopped);
    lv_obj_set_size(stop_hdr, 320, 42);
    lv_obj_align(stop_hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(stop_hdr, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_width(stop_hdr, 0, 0);
    lv_obj_set_style_radius(stop_hdr, 0, 0);
    lv_obj_set_style_pad_hor(stop_hdr, 6, 0);
    lv_obj_set_style_pad_ver(stop_hdr, 6, 0);
    lv_obj_set_flex_flow(stop_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stop_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(stop_hdr, false);
    lv_obj_set_event_bubble(stop_hdr, true);

    create_hdr_btn(stop_hdr, LV_SYMBOL_PLAY " Play", 70, 30, 0x0EA5E9, on_play_resume_clicked);
    s_btn_tab_stations = create_hdr_btn(stop_hdr, LV_SYMBOL_LIST " Stations", 82, 30, 0x0369A1, on_tab_stations_clicked);
    s_btn_tab_settings = create_hdr_btn(stop_hdr, LV_SYMBOL_SETTINGS " Settings", 82, 30, 0x1E293B, on_tab_settings_clicked);
    create_hdr_btn(stop_hdr, LV_SYMBOL_POWER " Sleep", 66, 30, 0x334155, on_sleep_clicked);

    /* Tab A: Station List */
    s_tab_stations = lv_obj_create(s_panel_stopped);
    lv_obj_set_size(s_tab_stations, 320, 198);
    lv_obj_align(s_tab_stations, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_tab_stations, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tab_stations, 0, 0);
    lv_obj_set_style_pad_all(s_tab_stations, 4, 0);
    lv_obj_set_scrollable(s_tab_stations, false);
    lv_obj_set_event_bubble(s_tab_stations, true);

    s_list_stations = lv_obj_create(s_tab_stations);
    lv_obj_set_size(s_list_stations, 310, 190);
    lv_obj_center(s_list_stations);
    lv_obj_set_style_bg_opa(s_list_stations, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_stations, 0, 0);
    lv_obj_set_style_pad_all(s_list_stations, 2, 0);
    lv_obj_set_flex_flow(s_list_stations, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_stations, 6, 0);
    lv_obj_set_scroll_dir(s_list_stations, LV_DIR_VER);
    lv_obj_set_event_bubble(s_list_stations, true);

    /* Tab B: Settings & Wi-Fi Info */
    s_tab_settings = lv_obj_create(s_panel_stopped);
    lv_obj_set_size(s_tab_settings, 320, 198);
    lv_obj_align(s_tab_settings, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_tab_settings, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tab_settings, 0, 0);
    lv_obj_set_style_pad_all(s_tab_settings, 10, 0);
    lv_obj_set_scrollable(s_tab_settings, false);
    lv_obj_set_hidden(s_tab_settings, true);
    lv_obj_set_event_bubble(s_tab_settings, true);

    s_lbl_net_mode = lv_label_create(s_tab_settings);
    lv_label_set_text(s_lbl_net_mode, "Network Info");
    lv_obj_set_style_text_font(s_lbl_net_mode, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_net_mode, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(s_lbl_net_mode, 2, 0);
    lv_obj_set_clickable(s_lbl_net_mode, false);

    s_lbl_net_detail = lv_label_create(s_tab_settings);
    lv_label_set_text(s_lbl_net_detail, "...");
    lv_obj_set_style_text_font(s_lbl_net_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_net_detail, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(s_lbl_net_detail, 2, 24);
    lv_obj_set_clickable(s_lbl_net_detail, false);

    s_lbl_net_hint = lv_label_create(s_tab_settings);
    lv_label_set_text(s_lbl_net_hint, "...");
    lv_obj_set_style_text_font(s_lbl_net_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_net_hint, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_pos(s_lbl_net_hint, 2, 86);
    lv_obj_set_clickable(s_lbl_net_hint, false);

    lv_obj_t *btn_ap = lv_button_create(s_tab_settings);
    lv_obj_set_size(btn_ap, 220, 36);
    lv_obj_align(btn_ap, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(btn_ap, 8, 0);
    lv_obj_set_style_bg_color(btn_ap, lv_color_hex(0x0369A1), 0);
    lv_obj_set_style_shadow_width(btn_ap, 0, 0);
    lv_obj_set_event_bubble(btn_ap, true);
    lv_obj_add_event_cb(btn_ap, on_start_ap_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ap = lv_label_create(btn_ap);
    lv_label_set_text(lbl_ap, "Enable AP Config Mode");
    lv_obj_set_style_text_font(lbl_ap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ap, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_ap);
    lv_obj_set_clickable(lbl_ap, false);

    /* ---------- View 5: Full-Screen Sleep Overlay (Tap anywhere to wake) ---------- */
    s_panel_sleep = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_sleep, 320, 240);
    lv_obj_align(s_panel_sleep, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel_sleep, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_panel_sleep, 0, 0);
    lv_obj_set_style_radius(s_panel_sleep, 0, 0);
    lv_obj_set_scrollable(s_panel_sleep, false);
    lv_obj_set_clickable(s_panel_sleep, true);
    lv_obj_add_event_cb(s_panel_sleep, on_sleep_screen_event, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_panel_sleep, true);

    /* ---------- Shared Floating Overlays on Root Screen ---------- */
    /* Temporary 2-second Station Name Banner */
    s_station_banner = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_station_banner, LV_SIZE_CONTENT, 32);
    lv_obj_align(s_station_banner, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(s_station_banner, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_station_banner, 220, 0);
    lv_obj_set_style_border_color(s_station_banner, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(s_station_banner, 1, 0);
    lv_obj_set_style_radius(s_station_banner, 16, 0);
    lv_obj_set_style_pad_hor(s_station_banner, 14, 0);
    lv_obj_set_style_pad_ver(s_station_banner, 5, 0);
    lv_obj_set_scrollable(s_station_banner, false);
    lv_obj_set_clickable(s_station_banner, false);
    lv_obj_set_hidden(s_station_banner, true);

    s_lbl_station_banner = lv_label_create(s_station_banner);
    lv_label_set_text(s_lbl_station_banner, "");
    lv_obj_set_style_text_font(s_lbl_station_banner, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_station_banner, lv_color_hex(0xF8FAFC), 0);
    lv_obj_center(s_lbl_station_banner);
    lv_obj_set_clickable(s_lbl_station_banner, false);

    /* Temporary Volume / Brightness Gesture OSD Overlay */
    s_osd_box = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_osd_box, 200, 44);
    lv_obj_align(s_osd_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_osd_box, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_osd_box, 230, 0);
    lv_obj_set_style_border_color(s_osd_box, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(s_osd_box, 1, 0);
    lv_obj_set_style_radius(s_osd_box, 12, 0);
    lv_obj_set_style_pad_all(s_osd_box, 6, 0);
    lv_obj_set_scrollable(s_osd_box, false);
    lv_obj_set_clickable(s_osd_box, false);
    lv_obj_set_hidden(s_osd_box, true);

    s_osd_lbl = lv_label_create(s_osd_box);
    lv_label_set_text(s_osd_lbl, "");
    lv_obj_set_style_text_font(s_osd_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_osd_lbl, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(s_osd_lbl, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(s_osd_lbl, false);

    s_osd_bar = lv_bar_create(s_osd_box);
    lv_obj_set_size(s_osd_bar, 168, 6);
    lv_bar_set_range(s_osd_bar, 0, 100);
    lv_bar_set_value(s_osd_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_osd_bar, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_osd_bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_align(s_osd_bar, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_clickable(s_osd_bar, false);

    /* Centered Toast Overlay (1.5s auto-hide) */
    s_toast = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_toast, 220, 42);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x18181B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x38BDF8), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toast, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_toast, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_toast, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_toast, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_toast, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_scrollable(s_toast, false);
    lv_obj_set_clickable(s_toast, false);
    lv_obj_set_hidden(s_toast, true);

    s_toast_label = lv_label_create(s_toast);
    lv_label_set_text(s_toast_label, "");
    lv_obj_set_width(s_toast_label, 208);
    lv_label_set_long_mode(s_toast_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_toast_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(s_toast_label);
    lv_obj_set_clickable(s_toast_label, false);

    /* Initial view matches current playback state */
    player_status_t pst;
    audio_player_get_status(&pst);
    s_prev_player_state = pst.state;
    if (pst.state == PLAYER_STATE_STOPPED) {
        switch_view(VIEW_STOPPED);
    } else {
        switch_view(VIEW_PLAYER);
        show_station_banner(pst.station_index, pst.station_name);
    }

    rebuild_station_list();
    lv_timer_create(ui_timer_cb, 50, NULL);
}

esp_err_t display_ui_init(i2c_master_bus_handle_t i2c_bus)
{
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);

    backlight_init(cfg.brightness);

    /* 1. SPI Bus & ILI9341 LCD */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_LCD_PIN_MOSI,
        .miso_io_num = BOARD_LCD_PIN_MISO,
        .sclk_io_num = BOARD_LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * 20 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = ILI9341_PANEL_IO_SPI_CONFIG(
        BOARD_LCD_PIN_CS, BOARD_LCD_PIN_DC, NULL, NULL);
    io_cfg.pclk_hz = BOARD_LCD_PIXEL_CLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_lcd_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_PIN_RST,
        .rgb_ele_order = cfg.swap_rb ? LCD_RGB_ELEMENT_ORDER_RGB : LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(s_lcd_io, &panel_cfg, &s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_lcd_panel, cfg.invert_color));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_lcd_panel, true));

    /* 2. FT6336G Capacitive Touch over shared I2C */
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = BOARD_I2C_CLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(i2c_bus, &tp_io_cfg, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_NATIVE_W,
        .y_max = BOARD_LCD_NATIVE_H,
        .rst_gpio_num = BOARD_TP_PIN_RST,
        .int_gpio_num = BOARD_TP_PIN_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &s_touch_handle));

    /* 3. LVGL Port: pin to Core 0 so Core 1 stays dedicated to audio_decode */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = 0;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = 320 * 20,
        .double_buffer = true,
        .hres = BOARD_LCD_NATIVE_W,
        .vres = BOARD_LCD_NATIVE_H,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,
        },
    };
    s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_lvgl_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_lvgl_disp,
        .handle = s_touch_handle,
    };
    lvgl_port_add_touch(&touch_cfg);

    if (lvgl_port_lock(0)) {
        apply_orientation_and_color(cfg.invert_color, cfg.flip_180, cfg.swap_rb);
        build_ui();
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Display & FT6336G touch UI initialized");
    return ESP_OK;
}
