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

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static lv_display_t *s_lvgl_disp = NULL;

/* UI views */
typedef enum {
    VIEW_PLAYER = 0,
    VIEW_STATIONS,
    VIEW_NETWORK,
} ui_view_t;

static ui_view_t s_cur_view = VIEW_PLAYER;

typedef enum {
    GEST_NONE = 0,
    GEST_VOLUME,
    GEST_SWIPE_X,
} gesture_mode_t;

/* LVGL widgets */
static lv_obj_t *s_scr_main = NULL;

static lv_obj_t *s_panel_player = NULL;
static lv_obj_t *s_panel_stations = NULL;
static lv_obj_t *s_panel_network = NULL;

/* Player widgets */
static lv_obj_t *s_lbl_station = NULL;
static lv_obj_t *s_lbl_state_pill = NULL;
static lv_obj_t *s_img_art = NULL;
static lv_obj_t *s_lbl_track = NULL;
static lv_obj_t *s_lbl_artist = NULL;
static lv_obj_t *s_lbl_codec_info = NULL;
static lv_obj_t *s_bar_buf = NULL;

/* Floating Gesture OSD */
static lv_obj_t *s_osd_box = NULL;
static lv_obj_t *s_osd_lbl = NULL;
static lv_obj_t *s_osd_bar = NULL;
static int64_t s_osd_hide_us = 0;

/* Artwork LVGL buffer */
static uint16_t *s_art_pixels = NULL;
static lv_image_dsc_t s_art_dsc;
static uint32_t s_shown_art_ver = 0;

/* Stations list container */
static lv_obj_t *s_list_stations = NULL;
static volatile bool s_need_station_rebuild = false;

/* Network / Settings view labels */
static lv_obj_t *s_lbl_net_mode = NULL;
static lv_obj_t *s_lbl_net_detail = NULL;
static lv_obj_t *s_lbl_net_hint = NULL;

/* Player gesture state */
static gesture_mode_t s_gest_mode = GEST_NONE;
static lv_point_t s_gest_start = {0, 0};
static lv_point_t s_gest_last = {0, 0};
static int s_gest_start_vol = 0;
static int s_gest_max_dist = 0;
static bool s_gest_long_pressed = false;

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
    uint32_t duty = ((uint32_t)pct * 1023) / 100;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
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

    /* Update BGR/RGB bit in MADCTL (reg 0x36) via raw panel io if needed */
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
    display_ui_set_brightness(cfg.brightness);

    if (lvgl_port_lock(500)) {
        apply_orientation_and_color(cfg.invert_color, cfg.flip_180, cfg.swap_rb);
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_unlock();
    }
}

static void switch_view(ui_view_t view)
{
    s_cur_view = view;
    if (view == VIEW_PLAYER) {
        lv_obj_clear_flag(s_panel_player, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_panel_stations, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_panel_network, LV_OBJ_FLAG_HIDDEN);
    } else if (view == VIEW_STATIONS) {
        lv_obj_add_flag(s_panel_player, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_panel_stations, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_panel_network, LV_OBJ_FLAG_HIDDEN);
        s_need_station_rebuild = true;
    } else {
        lv_obj_add_flag(s_panel_player, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_panel_stations, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_panel_network, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_osd_toast(const char *text, bool show_vol_bar, int vol_pct)
{
    if (!s_osd_box) return;
    lv_label_set_text(s_osd_lbl, text);
    if (show_vol_bar) {
        lv_obj_clear_flag(s_osd_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_osd_bar, vol_pct, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(s_osd_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_osd_box, LV_OBJ_FLAG_HIDDEN);
    s_osd_hide_us = esp_timer_get_time() + 1400000LL;
}

static void on_back_to_player_clicked(lv_event_t *e)
{
    (void)e;
    switch_view(VIEW_PLAYER);
}

static void on_nav_stations_clicked(lv_event_t *e)
{
    (void)e;
    switch_view(VIEW_STATIONS);
}

static void on_nav_net_clicked(lv_event_t *e)
{
    (void)e;
    switch_view(VIEW_NETWORK);
}

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
        s_gest_long_pressed = false;
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
            /* Swipe up (dy < 0) increases volume; ~150px maps 100% range */
            int target = s_gest_start_vol + (-dy * 100) / 150;
            if (target < 0) target = 0;
            if (target > 100) target = 100;
            audio_player_set_volume_live((uint8_t)target, false);
            char buf[24];
            snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX "  %d%%", target);
            show_osd_toast(buf, true, target);
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (s_gest_mode == GEST_NONE && s_gest_max_dist < 14) {
            s_gest_long_pressed = true;
            switch_view(VIEW_STATIONS);
        }
    } else if (code == LV_EVENT_RELEASED) {
        s_gest_last = pt;
        int dx = (int)pt.x - (int)s_gest_start.x;

        if (s_gest_mode == GEST_VOLUME) {
            uint8_t cur_vol = audio_player_get_volume();
            audio_player_set_volume_live(cur_vol, true);
            char buf[24];
            snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX "  %d%%", (int)cur_vol);
            show_osd_toast(buf, true, (int)cur_vol);
        } else if (s_gest_mode == GEST_SWIPE_X) {
            if (dx <= -35) {
                audio_player_next();
                show_osd_toast(LV_SYMBOL_NEXT "  Next Station", false, 0);
            } else if (dx >= 35) {
                audio_player_prev();
                show_osd_toast(LV_SYMBOL_PREV "  Prev Station", false, 0);
            }
        } else if (s_gest_mode == GEST_NONE && !s_gest_long_pressed && s_gest_max_dist < 16) {
            player_status_t pst;
            audio_player_get_status(&pst);
            bool stopping = (pst.state != PLAYER_STATE_STOPPED);
            audio_player_toggle_play_pause();
            show_osd_toast(stopping ? LV_SYMBOL_STOP "  Stopped" : LV_SYMBOL_PLAY "  Playing", false, 0);
        }
        s_gest_mode = GEST_NONE;
    } else if (code == LV_EVENT_PRESS_LOST) {
        if (s_gest_mode == GEST_VOLUME) {
            audio_player_set_volume_live(audio_player_get_volume(), true);
        }
        s_gest_mode = GEST_NONE;
    }
}

static void on_station_item_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    audio_player_play_station(idx);
    switch_view(VIEW_PLAYER);
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
        lv_obj_set_height(btn, 40);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, active ? lv_color_hex(0x0369A1) : lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_border_width(btn, active ? 1 : 0, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x38BDF8), 0);
        lv_obj_set_style_pad_hor(btn, 10, 0);
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

static lv_obj_t *create_ctrl_btn(lv_obj_t *parent, const char *txt, int w, int h,
                                 uint32_t bg_hex, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
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

    if (s_osd_box && !lv_obj_has_flag(s_osd_box, LV_OBJ_FLAG_HIDDEN)) {
        if (s_gest_mode != GEST_VOLUME && esp_timer_get_time() >= s_osd_hide_us) {
            lv_obj_add_flag(s_osd_box, LV_OBJ_FLAG_HIDDEN);
        }
    }

    player_status_t pst;
    audio_player_get_status(&pst);
    wifi_mgr_status_t wst;
    wifi_manager_get_status(&wst);

    /* Trigger background iTunes artwork fetch (deduplicated inside album_art) */
    if (pst.state == PLAYER_STATE_STOPPED) {
        album_art_clear(pst.station_index);
    } else {
        album_art_request(pst.station_index, pst.stream_artist, pst.stream_track);
    }

    /* Refresh artwork LVGL image buffer when version changes */
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

    /* Station name */
    char st_hdr[80];
    snprintf(st_hdr, sizeof(st_hdr), "%d. %s", pst.station_index + 1,
             pst.station_name[0] ? pst.station_name : "No Station");
    if (strcmp(lv_label_get_text(s_lbl_station), st_hdr) != 0) {
        lv_label_set_text(s_lbl_station, st_hdr);
    }

    /* Track title & Artist name */
    const char *track_txt = NULL;
    const char *artist_txt = "";
    const lv_font_t *track_font = &lv_font_montserrat_20;
    lv_color_t artist_col = lv_color_hex(0x38BDF8);

    if (pst.stream_track[0] != '\0') {
        track_txt = pst.stream_track;
        artist_txt = pst.stream_artist;
        if (strlen(track_txt) > 34) {
            track_font = &lv_font_montserrat_16;
        }
    } else if (pst.stream_title[0] != '\0') {
        track_txt = pst.stream_title;
        if (strlen(track_txt) > 34) {
            track_font = &lv_font_montserrat_16;
        }
    } else if (wst.state == WIFI_MGR_AP_CONFIG_MODE && pst.state == PLAYER_STATE_STOPPED) {
        track_txt = "Wi-Fi Setup";
        artist_txt = "AP: ES3C28P-Radio\nhttp://192.168.4.1";
        track_font = &lv_font_montserrat_16;
        artist_col = lv_color_hex(0xFBBF24);
    } else if (pst.state == PLAYER_STATE_STOPPED) {
        track_txt = "Stopped";
        artist_txt = "Tap: Play / Stop\nSwipe L/R: Station\nSwipe U/D: Volume";
        artist_col = lv_color_hex(0x64748B);
    } else {
        track_txt = pst.status_msg[0] ? pst.status_msg : "Connecting...";
        track_font = &lv_font_montserrat_16;
    }

    if (lv_obj_get_style_text_font(s_lbl_track, 0) != track_font) {
        lv_obj_set_style_text_font(s_lbl_track, track_font, 0);
    }
    if (strcmp(lv_label_get_text(s_lbl_track), track_txt) != 0) {
        lv_label_set_text(s_lbl_track, track_txt);
    }

    if (artist_txt[0] != '\0') {
        lv_obj_clear_flag(s_lbl_artist, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_lbl_artist, artist_col, 0);
        if (strcmp(lv_label_get_text(s_lbl_artist), artist_txt) != 0) {
            lv_label_set_text(s_lbl_artist, artist_txt);
        }
    } else {
        lv_obj_add_flag(s_lbl_artist, LV_OBJ_FLAG_HIDDEN);
    }

    /* State pill */
    const char *state_txt = "STOP";
    uint32_t pill_col = 0x475569;
    switch (pst.state) {
    case PLAYER_STATE_CONNECTING:
        state_txt = "CONN";
        pill_col = 0xD97706;
        break;
    case PLAYER_STATE_BUFFERING:
        state_txt = "BUF";
        pill_col = 0x0284C7;
        break;
    case PLAYER_STATE_PLAYING:
        state_txt = "LIVE";
        pill_col = 0x059669;
        break;
    case PLAYER_STATE_ERROR:
        state_txt = "ERR";
        pill_col = 0xDC2626;
        break;
    default:
        break;
    }
    if (strcmp(lv_label_get_text(s_lbl_state_pill), state_txt) != 0) {
        lv_label_set_text(s_lbl_state_pill, state_txt);
    }
    lv_obj_set_style_bg_color(s_lbl_state_pill, lv_color_hex(pill_col), 0);

    /* Codec info (integer formatting; LVGL printf does not enable %f) */
    if (pst.state == PLAYER_STATE_PLAYING || pst.state == PLAYER_STATE_BUFFERING) {
        uint32_t sr_khz = pst.sample_rate / 1000;
        uint32_t sr_tenths = (pst.sample_rate % 1000) / 100;
        if (pst.sample_rate > 0) {
            lv_label_set_text_fmt(s_lbl_codec_info, "%s %luk  %lu.%lukHz",
                                  pst.codec_name[0] ? pst.codec_name : "MP3",
                                  (unsigned long)pst.bitrate_kbps,
                                  (unsigned long)sr_khz,
                                  (unsigned long)sr_tenths);
        } else {
            lv_label_set_text_fmt(s_lbl_codec_info, "%s %luk",
                                  pst.codec_name[0] ? pst.codec_name : "MP3",
                                  (unsigned long)pst.bitrate_kbps);
        }
    } else {
        lv_label_set_text(s_lbl_codec_info, pst.status_msg);
    }

    lv_bar_set_value(s_bar_buf, pst.buffer_pct, LV_ANIM_OFF);

    /* Network / Settings view */
    if (s_cur_view == VIEW_NETWORK) {
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

static lv_obj_t *create_subview_header(lv_obj_t *parent, const char *title,
                                       const char *alt_btn_txt, lv_event_cb_t alt_cb)
{
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, 320, 36);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x111827), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 8, 0);
    lv_obj_set_style_pad_ver(hdr, 4, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_back = lv_button_create(hdr);
    lv_obj_set_size(btn_back, 64, 26);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x243447), 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, on_back_to_player_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_b = lv_label_create(btn_back);
    lv_label_set_text(lbl_b, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(lbl_b, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl_b);

    lv_obj_t *lbl_t = lv_label_create(hdr);
    lv_label_set_text(lbl_t, title);
    lv_obj_set_style_text_font(lbl_t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_t, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(lbl_t, LV_ALIGN_CENTER, -6, 0);

    if (alt_btn_txt && alt_cb) {
        lv_obj_t *btn_alt = lv_button_create(hdr);
        lv_obj_set_size(btn_alt, 64, 26);
        lv_obj_align(btn_alt, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(btn_alt, 6, 0);
        lv_obj_set_style_bg_color(btn_alt, lv_color_hex(0x243447), 0);
        lv_obj_set_style_shadow_width(btn_alt, 0, 0);
        lv_obj_add_event_cb(btn_alt, alt_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl_a = lv_label_create(btn_alt);
        lv_label_set_text(lbl_a, alt_btn_txt);
        lv_obj_set_style_text_font(lbl_a, &lv_font_montserrat_12, 0);
        lv_obj_center(lbl_a);
    }

    return hdr;
}

static void build_ui(void)
{
    s_scr_main = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr_main, lv_color_hex(0x0B1120), 0);
    lv_obj_set_style_pad_all(s_scr_main, 0, 0);
    lv_obj_clear_flag(s_scr_main, LV_OBJ_FLAG_SCROLLABLE);

    /* Allocate RGB565 artwork pixel buffer in PSRAM */
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

    /* ---------- View 1: Full-screen Gesture Player (320x240) ---------- */
    s_panel_player = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_player, 320, 240);
    lv_obj_align(s_panel_player, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_panel_player, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel_player, 0, 0);
    lv_obj_set_style_pad_all(s_panel_player, 0, 0);
    lv_obj_clear_flag(s_panel_player, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel_player, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_panel_player, on_player_gesture_event, LV_EVENT_ALL, NULL);

    /* Top Header: Station Name + State Pill + Subtle Corner Icons */
    s_lbl_station = lv_label_create(s_panel_player);
    lv_label_set_text(s_lbl_station, "1. SomaFM");
    lv_label_set_long_mode(s_lbl_station, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_lbl_station, 194);
    lv_obj_set_style_text_font(s_lbl_station, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_station, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(s_lbl_station, 10, 8);

    s_lbl_state_pill = lv_label_create(s_panel_player);
    lv_label_set_text(s_lbl_state_pill, "STOP");
    lv_obj_set_style_text_font(s_lbl_state_pill, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_state_pill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_lbl_state_pill, lv_color_hex(0x475569), 0);
    lv_obj_set_style_bg_opa(s_lbl_state_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_lbl_state_pill, 5, 0);
    lv_obj_set_style_pad_hor(s_lbl_state_pill, 6, 0);
    lv_obj_set_style_pad_ver(s_lbl_state_pill, 2, 0);
    lv_obj_align(s_lbl_state_pill, LV_ALIGN_TOP_RIGHT, -66, 8);

    lv_obj_t *btn_list = lv_button_create(s_panel_player);
    lv_obj_set_size(btn_list, 26, 24);
    lv_obj_align(btn_list, LV_ALIGN_TOP_RIGHT, -35, 6);
    lv_obj_set_style_radius(btn_list, 6, 0);
    lv_obj_set_style_bg_color(btn_list, lv_color_hex(0x162032), 0);
    lv_obj_set_style_border_color(btn_list, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(btn_list, 1, 0);
    lv_obj_set_style_shadow_width(btn_list, 0, 0);
    lv_obj_set_ext_click_area(btn_list, 6);
    lv_obj_add_event_cb(btn_list, on_nav_stations_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_list = lv_label_create(btn_list);
    lv_label_set_text(lbl_list, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(lbl_list, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_list, lv_color_hex(0x94A3B8), 0);
    lv_obj_center(lbl_list);

    lv_obj_t *btn_net = lv_button_create(s_panel_player);
    lv_obj_set_size(btn_net, 26, 24);
    lv_obj_align(btn_net, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_radius(btn_net, 6, 0);
    lv_obj_set_style_bg_color(btn_net, lv_color_hex(0x162032), 0);
    lv_obj_set_style_border_color(btn_net, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(btn_net, 1, 0);
    lv_obj_set_style_shadow_width(btn_net, 0, 0);
    lv_obj_set_ext_click_area(btn_net, 6);
    lv_obj_add_event_cb(btn_net, on_nav_net_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_net = lv_label_create(btn_net);
    lv_label_set_text(lbl_net, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(lbl_net, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_net, lv_color_hex(0x94A3B8), 0);
    lv_obj_center(lbl_net);

    /* Subtle Divider */
    lv_obj_t *divider = lv_obj_create(s_panel_player);
    lv_obj_set_size(divider, 300, 1);
    lv_obj_set_pos(divider, 10, 36);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x1E293B), 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_CLICKABLE);

    /* Left: Album Artwork Card (132x132 containing 128x128 RGB565 image) */
    lv_obj_t *art_frame = lv_obj_create(s_panel_player);
    lv_obj_set_size(art_frame, 132, 132);
    lv_obj_set_pos(art_frame, 10, 46);
    lv_obj_set_style_bg_color(art_frame, lv_color_hex(0x162032), 0);
    lv_obj_set_style_border_color(art_frame, lv_color_hex(0x1E3A5F), 0);
    lv_obj_set_style_border_width(art_frame, 1, 0);
    lv_obj_set_style_radius(art_frame, 10, 0);
    lv_obj_set_style_pad_all(art_frame, 0, 0);
    lv_obj_set_style_clip_corner(art_frame, true, 0);
    lv_obj_clear_flag(art_frame, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_img_art = lv_image_create(art_frame);
    if (s_art_pixels) {
        lv_image_set_src(s_img_art, &s_art_dsc);
    }
    lv_obj_set_size(s_img_art, ALBUM_ART_WIDTH, ALBUM_ART_HEIGHT);
    lv_obj_set_style_radius(s_img_art, 8, 0);
    lv_obj_set_style_clip_corner(s_img_art, true, 0);
    lv_obj_center(s_img_art);

    /* Codec & buffer indicator cleanly tucked below artwork */
    s_lbl_codec_info = lv_label_create(s_panel_player);
    lv_label_set_text(s_lbl_codec_info, "");
    lv_obj_set_width(s_lbl_codec_info, 132);
    lv_obj_set_style_text_align(s_lbl_codec_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_lbl_codec_info, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_codec_info, lv_color_hex(0x64748B), 0);
    lv_obj_set_pos(s_lbl_codec_info, 10, 186);

    s_bar_buf = lv_bar_create(s_panel_player);
    lv_obj_set_size(s_bar_buf, 132, 4);
    lv_bar_set_range(s_bar_buf, 0, 100);
    lv_bar_set_value(s_bar_buf, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_buf, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_buf, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_pos(s_bar_buf, 10, 206);

    /* Right: Track Title & Artist Name Flex Column */
    lv_obj_t *meta_box = lv_obj_create(s_panel_player);
    lv_obj_set_size(meta_box, 160, 178);
    lv_obj_set_pos(meta_box, 152, 44);
    lv_obj_set_style_bg_opa(meta_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meta_box, 0, 0);
    lv_obj_set_style_pad_all(meta_box, 2, 0);
    lv_obj_set_flex_flow(meta_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(meta_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(meta_box, 8, 0);
    lv_obj_clear_flag(meta_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_lbl_track = lv_label_create(meta_box);
    lv_label_set_text(s_lbl_track, "Ready");
    lv_label_set_long_mode(s_lbl_track, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_track, 156);
    lv_obj_set_style_text_font(s_lbl_track, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lbl_track, lv_color_hex(0xF8FAFC), 0);

    s_lbl_artist = lv_label_create(meta_box);
    lv_label_set_text(s_lbl_artist, "");
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lbl_artist, 156);
    lv_obj_set_style_text_font(s_lbl_artist, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_artist, lv_color_hex(0x38BDF8), 0);

    /* Floating Gesture OSD Overlay (hidden unless gesturing) */
    s_osd_box = lv_obj_create(s_panel_player);
    lv_obj_set_size(s_osd_box, 210, 46);
    lv_obj_align(s_osd_box, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(s_osd_box, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(s_osd_box, 235, 0);
    lv_obj_set_style_border_color(s_osd_box, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(s_osd_box, 1, 0);
    lv_obj_set_style_radius(s_osd_box, 12, 0);
    lv_obj_set_style_pad_all(s_osd_box, 6, 0);
    lv_obj_clear_flag(s_osd_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_osd_box, LV_OBJ_FLAG_HIDDEN);

    s_osd_lbl = lv_label_create(s_osd_box);
    lv_label_set_text(s_osd_lbl, "");
    lv_obj_set_style_text_font(s_osd_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_osd_lbl, lv_color_hex(0xF8FAFC), 0);
    lv_obj_align(s_osd_lbl, LV_ALIGN_TOP_MID, 0, 0);

    s_osd_bar = lv_bar_create(s_osd_box);
    lv_obj_set_size(s_osd_bar, 176, 6);
    lv_bar_set_range(s_osd_bar, 0, 100);
    lv_bar_set_value(s_osd_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_osd_bar, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_osd_bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_align(s_osd_bar, LV_ALIGN_BOTTOM_MID, 0, -2);

    /* ---------- View 2: Stations List (320x240) ---------- */
    s_panel_stations = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_stations, 320, 240);
    lv_obj_align(s_panel_stations, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_panel_stations, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel_stations, 0, 0);
    lv_obj_set_style_pad_all(s_panel_stations, 0, 0);
    lv_obj_clear_flag(s_panel_stations, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel_stations, LV_OBJ_FLAG_HIDDEN);

    create_subview_header(s_panel_stations, "Stations", LV_SYMBOL_SETTINGS " Net", on_nav_net_clicked);

    s_list_stations = lv_obj_create(s_panel_stations);
    lv_obj_set_size(s_list_stations, 308, 194);
    lv_obj_align(s_list_stations, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(s_list_stations, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_stations, 0, 0);
    lv_obj_set_style_pad_all(s_list_stations, 2, 0);
    lv_obj_set_flex_flow(s_list_stations, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_stations, 6, 0);

    /* ---------- View 3: Settings & Network Info (320x240) ---------- */
    s_panel_network = lv_obj_create(s_scr_main);
    lv_obj_set_size(s_panel_network, 320, 240);
    lv_obj_align(s_panel_network, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_panel_network, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel_network, 0, 0);
    lv_obj_set_style_pad_all(s_panel_network, 0, 0);
    lv_obj_clear_flag(s_panel_network, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel_network, LV_OBJ_FLAG_HIDDEN);

    create_subview_header(s_panel_network, "Settings & Wi-Fi", LV_SYMBOL_LIST " List", on_nav_stations_clicked);

    s_lbl_net_mode = lv_label_create(s_panel_network);
    lv_label_set_text(s_lbl_net_mode, "Network Info");
    lv_obj_set_style_text_font(s_lbl_net_mode, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lbl_net_mode, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(s_lbl_net_mode, 12, 44);

    s_lbl_net_detail = lv_label_create(s_panel_network);
    lv_label_set_text(s_lbl_net_detail, "...");
    lv_obj_set_style_text_font(s_lbl_net_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_net_detail, lv_color_hex(0xE2E8F0), 0);
    lv_obj_set_pos(s_lbl_net_detail, 12, 70);

    s_lbl_net_hint = lv_label_create(s_panel_network);
    lv_label_set_text(s_lbl_net_hint, "...");
    lv_obj_set_style_text_font(s_lbl_net_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_net_hint, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_pos(s_lbl_net_hint, 12, 134);

    create_ctrl_btn(s_panel_network, "Enable AP Config Mode", 220, 36, 0x0369A1, on_start_ap_clicked);
    lv_obj_t *btn_ap = lv_obj_get_child(s_panel_network, -1);
    lv_obj_align(btn_ap, LV_ALIGN_BOTTOM_MID, 0, -10);

    rebuild_station_list();
    lv_timer_create(ui_timer_cb, 250, NULL);
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
        .max_transfer_sz = 320 * 40 * sizeof(uint16_t),
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

    /* 3. LVGL Port */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = 320 * 40,
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
