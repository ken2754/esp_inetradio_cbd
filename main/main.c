#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "album_art.h"
#include "app_config.h"
#include "audio_player.h"
#include "board_config.h"
#include "display_ui.h"
#include "google_quota.h"
#include "sntp_time.h"
#include "web_server.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static i2c_master_bus_handle_t init_shared_i2c(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_PIN_SDA,
        .scl_io_num = BOARD_I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &bus));
    return bus;
}

static void boot_button_task(void *arg)
{
    (void)arg;
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BOOT_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_cfg);

    int held_ms = 0;
    while (1) {
        if (gpio_get_level(BOARD_PIN_BOOT_BTN) == 0) {
            held_ms += 100;
            if (held_ms == 2500) {
                ESP_LOGI(TAG, "BOOT button held: starting SoftAP config mode");
                wifi_manager_start_ap_mode();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting ES3C28P Internet Radio...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(app_config_init());

    i2c_master_bus_handle_t i2c_bus = init_shared_i2c();

    ESP_ERROR_CHECK(album_art_init());
    ESP_ERROR_CHECK(audio_player_init(i2c_bus));
    ESP_ERROR_CHECK(display_ui_init(i2c_bus));
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(sntp_time_init(NULL));
    google_quota_init();
    ESP_ERROR_CHECK(web_server_start());

    xTaskCreate(boot_button_task, "boot_btn", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "Initialization complete (free internal=%u, spiram=%u).",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}
