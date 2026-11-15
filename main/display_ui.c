#include "display_ui.h"
#include "album_art.h"
#include "app_config.h"
#include "audio_player.h"
#include "board_config.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define VOLUME_OSD_US       1400000LL
#define SLEEP_WAKE_GUARD_US 350000LL

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_lvgl_disp = NULL;

/* UI views */
typedef enum {
    VIEW_PLAYER = 0,
    VIEW_STOPPED,
    VIEW_SLEEP,
} ui_view_t;

typedef enum {
    STOP_TAB_STATIONS = 0,
    STOP_TAB_SETTINGS,
} stopped_tab_t;

typedef enum {
    GEST_NONE = 0,
    GEST_VOLUME,
    GEST_SWIPE_X,
} gesture_mode_t;

/* 8-direction outlined label for legible overlay text on bright artwork */
#define OUTLINE_OFFSETS 8
typedef struct {
    lv_obj_t *wrap;
    lv_obj_t *bg[OUTLINE_OFFSETS];
    lv_obj_t *fg;
} outlined_label_t;

static ui_view_t s_cur_view = VIEW_PLAYER;
static stopped_tab_t s_stop_tab = STOP_TAB_STATIONS;

/* Top-level screens/panels */
static lv_obj_t *s_scr_main = NULL;
static lv_obj_t *s_panel_player = NULL;
static lv_obj_t *s_panel_stopped = NULL;
static lv_obj_t *s_panel_sleep = NULL;

/* Player widgets (zero buttons, maximized art) */
static lv_obj_t *s_img_art = NULL;
static outlined_label_t s_ol_track;
static outlined_label_t s_ol_artist;
static lv_grad_dsc_t s_scrim_grad;

/* Temporary 2-second station banner overlay */
static lv_obj_t *s_station_banner = NULL;
static lv_obj_t *s_lbl_station_banner = NULL;
static int64_t s_station_hide_us = 0;
static int s_last_station_idx = -1;

/* Temporary gesture volume OSD overlay */
static lv_obj_t *s_osd_box = NULL;
static lv_obj_t *s_osd_lbl = NULL;
static lv_obj_t *s_osd_bar = NULL;
static int64_t s_osd_hide_us = 0;

/* Artwork LVGL buffer */
static uint16_t *s_art_pixels = NULL;
static lv_image_dsc_t s_art_dsc;
static uint32_t s_shown_art_ver = 0;

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

/* Player gesture state */
static gesture_mode_t s_gest_mode = GEST_NONE;
static lv_point_t s_gest_start = {0, 0};
static lv_point_t s_gest_last = {0, 0};
static int s_gest_start_vol = 0;
static int s_gest_max_dist = 0;

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
    set_backlight_raw(pct);
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
    if (s_cur_view != VIEW_SLEEP) {
        display_ui_set_brightness(cfg.brightness);
    }

    if (lvgl_port_lock(500)) {
        apply_orientation_and_color(cfg.invert_color, cfg.flip_180, cfg.swap_rb);
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_unlock();
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
    lv_obj_set_hidden(s_panel_player, view != VIEW_PLAYER);
    lv_obj_set_hidden(s_panel_stopped, view != VIEW_STOPPED);
    lv_obj_set_hidden(s_panel_sleep, view != VIEW_SLEEP);

    if (view == VIEW_STOPPED) {
        wifi_mgr_status_t wst;
        wifi_manager_get_status(&wst);
        if (wst.state == WIFI_MGR_AP_CONFIG_MODE) {
            switch_stopped_tab(STOP_TAB_SETTINGS);
        } else {
            switch_stopped_tab(s_stop_tab);
        }
        s_need_station_rebuild = true;
    }
}

static void show_station_banner(int station_idx, const char *station_name)
{
    if (!s_station_banner || !s_lbl_station_banner) return;
    char buf[80];
    snprintf(buf, sizeof(buf), "%d. %s", station_idx + 1,
             (station_name && station_name[0]) ? station_name : "Station");
    lv_label_set_text(s_lbl_station_banner, buf);
    lv_obj_set_hidden(s_station_banner, false);
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
    lv_obj_set_hidden(s_osd_box, false);
    s_osd_hide_us = esp_timer_get_time() + VOLUME_OSD_US;
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

    if (s_lcd_panel) {
        esp_lcd_panel_disp_on_off(s_lcd_panel, true);
    }
    display_ui_set_brightness(cfg.brightness);
    s_sleep_touch_armed = false;
    switch_view(target_view);
    ESP_LOGI(TAG, "Woke from sleep mode");
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

/* ---------- Player Gesture Events ---------- */
static void on_player_gesture_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt = s_gest_last;
    if (indev) {
        lv_indev_get_point(indev, &pt);
    }

    if (code == LV_EVENT_PRESSED) {
        s_gest_start = pt;
        s_gest_last = pt;
        s_gest_start_vol = (int)audio_player_get_volume();
        s_gest_mode = GEST_NONE;
        s_gest_max_dist = 0;
    } else if (code == LV_EVENT_PRESSING) {
        s_gest_last = pt;
        int dx = (int)pt.x - (int)s_gest_start.x;
        int dy = (int)pt.y - (int)s_gest_start.y;
        int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (dist > s_gest_max_dist) s_gest_max_dist = dist;

        if (s_gest_mode == GEST_NONE) {
            if (abs(dy) >= 14 && abs(dy) > (abs(dx) * 6) / 5) {
                s_gest_mode = GEST_VOLUME;
            } else if (abs(dx) >= 24 && abs(dx) > (abs(dy) * 6) / 5) {
                s_gest_mode = GEST_SWIPE_X;
            }
        }

        if (s_gest_mode == GEST_VOLUME) {
            int target = s_gest_start_vol + (-dy * 100) / 150;
            if (target < 0) target = 0;
            if (target > 100) target = 100;
            audio_player_set_volume_live((uint8_t)target, false);
            show_vol_osd(target);
        }
    } else if (code == LV_EVENT_RELEASED) {
        s_gest_last = pt;
        int dx = (int)pt.x - (int)s_gest_start.x;

        if (s_gest_mode == GEST_VOLUME) {
            uint8_t cur_vol = audio_player_get_volume();
            audio_player_set_volume_live(cur_vol, true);
            show_vol_osd((int)cur_vol);
        } else if (s_gest_mode == GEST_SWIPE_X) {
            if (dx <= -35) {
                audio_player_next();
                show_station_banner_current();
            } else if (dx >= 35) {
                audio_player_prev();
                show_station_banner_current();
            }
        } else if (s_gest_mode == GEST_NONE && s_gest_max_dist < 16) {
            /* Tap on Player screen -> Stop and open Unified Stopped screen */
            audio_player_stop();
            switch_view(VIEW_STOPPED);
        }
        s_gest_mode = GEST_NONE;
    } else if (code == LV_EVENT_PRESS_LOST) {
        if (s_gest_mode == GEST_VOLUME) {
            audio_player_set_volume_live(audio_player_get_volume(), true);
        }
        s_gest_mode = GEST_NONE;
    }
}

/* ---------- Stopped Screen Button Handlers ---------- */
static void on_play_resume_clicked(lv_event_t *e)
{
    (void)e;
    app_config_meta_t cfg;
    app_config_get_meta(&cfg);
    audio_player_play_station(cfg.current_station);
    switch_view(VIEW_PLAYER);
    show_station_banner_current();
}

static void on_tab_stations_clicked(lv_event_t *e)
{
    (void)e;
    switch_stopped_tab(STOP_TAB_STATIONS);
}

static void on_tab_settings_clicked(lv_event_t *e)
{
    (void)e;
    switch_stopped_tab(STOP_TAB_SETTINGS);
}

static void on_sleep_clicked(lv_event_t *e)
{
    (void)e;
    enter_sleep_mode();
}

static void on_station_item_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    audio_player_play_station(idx);
    switch_view(VIEW_PLAYER);
    show_station_banner_current();
}

static void on_start_ap_clicked(lv_event_t *e)
{
    (void)e;
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
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
    return btn;
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_need_station_rebuild) {
        s_need_station_rebuild = false;
        rebuild_station_list();
    }

    player_status_t pst;
    audio_player_get_status(&pst);
    wifi_mgr_status_t wst = {0};
    static uint32_t s_wifi_poll_div = 0;
    bool poll_wifi = false;
    if (s_cur_view == VIEW_PLAYER) {
        s_wifi_poll_div = 0;
    } else if ((s_wifi_poll_div++ % 10U) == 0U) {
        poll_wifi = true;
        wifi_manager_get_status(&wst);
    }

    /* Auto-sync view with playback state (e.g. Web UI Play/Stop or startup autoplay) */
    static bool s_ap_tab_auto_shown = false;
    if (poll_wifi && wst.state == WIFI_MGR_AP_CONFIG_MODE) {
        if (!s_ap_tab_auto_shown && s_cur_view == VIEW_STOPPED) {
            s_ap_tab_auto_shown = true;
            switch_stopped_tab(STOP_TAB_SETTINGS);
        }
    } else if (poll_wifi) {
        s_ap_tab_auto_shown = false;
    }

    if (s_cur_view == VIEW_SLEEP) {
        if (pst.state != PLAYER_STATE_STOPPED) {
            wake_from_sleep(VIEW_PLAYER);
            show_station_banner(pst.station_index, pst.station_name);
        } else {
            return;
        }
    } else if (s_cur_view == VIEW_STOPPED && pst.state != PLAYER_STATE_STOPPED) {
        switch_view(VIEW_PLAYER);
        show_station_banner(pst.station_index, pst.station_name);
    } else if (s_cur_view == VIEW_PLAYER && pst.state == PLAYER_STATE_STOPPED) {
        switch_view(VIEW_STOPPED);
    }

    int64_t now_us = esp_timer_get_time();

    /* Auto-hide temporary 2-second station banner */
    if (s_station_banner && !lv_obj_is_hidden(s_station_banner)) {
        if (now_us >= s_station_hide_us) {
            lv_obj_set_hidden(s_station_banner, true);
        }
    }

    /* Auto-hide temporary volume OSD */
    if (s_osd_box && !lv_obj_is_hidden(s_osd_box)) {
        if (s_gest_mode != GEST_VOLUME && now_us >= s_osd_hide_us) {
            lv_obj_set_hidden(s_osd_box, true);
        }
    }

    if (s_cur_view == VIEW_PLAYER) {
        /* Detect station switches initiated externally (e.g. Web UI) */
        if (pst.station_index != s_last_station_idx) {
            show_station_banner(pst.station_index, pst.station_name);
        }

        /* Refresh maximized artwork image if version changed (driven in sync by audio_player) */
        if (s_art_pixels && s_img_art) {
            uint32_t cur_ver = album_art_get_version();
            if (cur_ver != s_shown_art_ver) {
                bool is_cover = false;
                if (album_art_copy_rgb565(s_art_pixels, ALBUM_ART_BYTES, &is_cover, &s_shown_art_ver)) {
                    lv_image_cache_drop(&s_art_dsc);
                    lv_obj_invalidate(s_img_art);
                }
            }
        }

        /* Minimalist track & artist display */
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
    } else if (poll_wifi && s_cur_view == VIEW_STOPPED && s_stop_tab == STOP_TAB_SETTINGS) {
        if (wst.state == WIFI_MGR_CONNECTED_STA) {
            lv_label_set_text(s_lbl_net_mode, "Wi-Fi Station (Connected)");
            lv_label_set_text_fmt(s_lbl_net_detail,
                                  "SSID: %s\nIP: %s\nSignal: %d dBm",
                                  wst.ssid, wst.ip_addr, wst.rssi);
            lv_label_set_text_fmt(s_lbl_net_hint,
                                  "Web Config:\nhttp://%s/\nhttp://%s.local/",
                                  wst.ip_addr, APP_MDNS_HOSTNAME);
        } else if (wst.state == WIFI_MGR_AP_CONFIG_MODE) {
            lv_label_set_text(s_lbl_net_mode, "SoftAP Provisioning");
            lv_label_set_text_fmt(s_lbl_net_detail,
                                  "AP SSID: %s (Open)\nAP IP: %s",
                                  wst.ap_ssid, wst.ap_ip);
            lv_label_set_text(s_lbl_net_hint,
                              "1. Connect phone/PC to 'ES3C28P-Radio'\n2. Open browser: http://192.168.4.1/");
        } else {
            lv_label_set_text(s_lbl_net_mode, "Connecting Wi-Fi...");
            lv_label_set_text_fmt(s_lbl_net_detail, "Target SSID: %s", wst.ssid);
            lv_label_set_text(s_lbl_net_hint, "Please wait...");
        }
    }
}

static void build_ui(void)
{
    s_scr_main = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr_main, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(s_scr_main, 0, 0);
    lv_obj_set_scrollable(s_scr_main, false);

    /* Allocate full 320x240 RGB565 artwork pixel buffer in PSRAM */
    s_art_pixels = heap_caps_malloc(ALBUM_ART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_art_pixels) {
        s_art_pixels = malloc(ALBUM_ART_BYTES);
    }
    if (s_art_pixels) {
        bool dummy = false;
        album_art_copy_rgb565(s_art_pixels, ALBUM_ART_BYTES, &dummy, &s_shown_art_ver);
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

    /* ---------- View 1: Minimalist Full-Bleed Artwork Player (320x240, ZERO buttons) ---------- */
    s_panel_player = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_player, 320, 240);
    lv_obj_align(s_panel_player, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel_player, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_panel_player, 0, 0);
    lv_obj_set_style_pad_all(s_panel_player, 0, 0);
    lv_obj_set_scrollable(s_panel_player, false);
    lv_obj_set_clickable(s_panel_player, true);
    lv_obj_add_event_cb(s_panel_player, on_player_gesture_event, LV_EVENT_ALL, NULL);

    /* Maximized 320x240 artwork canvas (240x240 cover + ambient wings) */
    s_img_art = lv_image_create(s_panel_player);
    if (s_art_pixels) {
        lv_image_set_src(s_img_art, &s_art_dsc);
    }
    lv_obj_set_size(s_img_art, ALBUM_ART_WIDTH, ALBUM_ART_HEIGHT);
    lv_obj_align(s_img_art, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_clickable(s_img_art, false);

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

    /* Temporary 2-second Station Name Banner (hidden except for 2s on station switch) */
    s_station_banner = lv_obj_create(s_panel_player);
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

    /* Temporary Volume Gesture OSD Overlay */
    s_osd_box = lv_obj_create(s_panel_player);
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

    s_osd_bar = lv_bar_create(s_osd_box);
    lv_obj_set_size(s_osd_bar, 168, 6);
    lv_bar_set_range(s_osd_bar, 0, 100);
    lv_bar_set_value(s_osd_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_osd_bar, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_osd_bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_align(s_osd_bar, LV_ALIGN_BOTTOM_MID, 0, -2);

    /* ---------- View 2: Unified Stopped Screen (Stations + Settings + Sleep) ---------- */
    s_panel_stopped = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_stopped, 320, 240);
    lv_obj_align(s_panel_stopped, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_panel_stopped, lv_color_hex(0x0B1120), 0);
    lv_obj_set_style_border_width(s_panel_stopped, 0, 0);
    lv_obj_set_style_pad_all(s_panel_stopped, 0, 0);
    lv_obj_set_scrollable(s_panel_stopped, false);
    lv_obj_set_hidden(s_panel_stopped, true);

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

    s_list_stations = lv_obj_create(s_tab_stations);
    lv_obj_set_size(s_list_stations, 310, 190);
    lv_obj_center(s_list_stations);
    lv_obj_set_style_bg_opa(s_list_stations, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_stations, 0, 0);
    lv_obj_set_style_pad_all(s_list_stations, 2, 0);
    lv_obj_set_flex_flow(s_list_stations, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_stations, 6, 0);

    /* Tab B: Settings & Wi-Fi Info */
    s_tab_settings = lv_obj_create(s_panel_stopped);
    lv_obj_set_size(s_tab_settings, 320, 198);
    lv_obj_align(s_tab_settings, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_tab_settings, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tab_settings, 0, 0);
    lv_obj_set_style_pad_all(s_tab_settings, 10, 0);
    lv_obj_set_scrollable(s_tab_settings, false);
    lv_obj_set_hidden(s_tab_settings, true);

    s_lbl_net_mode = lv_label_create(s_tab_settings);
    lv_label_set_text(s_lbl_net_mode, "Network Info");
    lv_obj_set_style_text_font(s_lbl_net_mode, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_net_mode, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(s_lbl_net_mode, 2, 0);

    s_lbl_net_detail = lv_label_create(s_tab_settings);
    lv_label_set_text(s_lbl_net_detail, "...");
    lv_obj_set_style_text_font(s_lbl_net_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_net_detail, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(s_lbl_net_detail, 2, 24);

    s_lbl_net_hint = lv_label_create(s_tab_settings);
    lv_label_set_text(s_lbl_net_hint, "...");
    lv_obj_set_style_text_font(s_lbl_net_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_net_hint, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_pos(s_lbl_net_hint, 2, 86);

    lv_obj_t *btn_ap = lv_button_create(s_tab_settings);
    lv_obj_set_size(btn_ap, 220, 36);
    lv_obj_align(btn_ap, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_radius(btn_ap, 8, 0);
    lv_obj_set_style_bg_color(btn_ap, lv_color_hex(0x0369A1), 0);
    lv_obj_set_style_shadow_width(btn_ap, 0, 0);
    lv_obj_add_event_cb(btn_ap, on_start_ap_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ap = lv_label_create(btn_ap);
    lv_label_set_text(lbl_ap, "Enable AP Config Mode");
    lv_obj_set_style_text_font(lbl_ap, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ap, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl_ap);

    /* ---------- View 3: Full-Screen Sleep Overlay (Tap anywhere to wake) ---------- */
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

    /* Initial view matches current playback state */
    player_status_t pst;
    audio_player_get_status(&pst);
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
