#pragma once

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

/* =========================================================================
 * ES3C28P (2.8" ESP32-S3 N16R8 Cheap Black Display) Hardware Pinout
 * ========================================================================= */

/* LCD Display: ILI9341V (240x320 IPS, 4-wire SPI) */
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_PIN_MOSI      GPIO_NUM_11
#define BOARD_LCD_PIN_SCLK      GPIO_NUM_12
#define BOARD_LCD_PIN_MISO      GPIO_NUM_13
#define BOARD_LCD_PIN_CS        GPIO_NUM_10
#define BOARD_LCD_PIN_DC        GPIO_NUM_46
#define BOARD_LCD_PIN_RST       GPIO_NUM_NC   /* Tied to CHIP_PU */
#define BOARD_LCD_PIN_BL        GPIO_NUM_45   /* Active High PWM backlight */
#define BOARD_LCD_PIXEL_CLK_HZ  (40 * 1000 * 1000)

/* Native panel resolution (portrait). Landscape UI uses 320x240 */
#define BOARD_LCD_NATIVE_W      240
#define BOARD_LCD_NATIVE_H      320

/* Shared I2C Bus: FT6336G Touch (0x38) & ES8311 Codec (7-bit 0x18 / 8-bit 0x30) */
#define BOARD_I2C_PORT          I2C_NUM_0
#define BOARD_I2C_PIN_SDA       GPIO_NUM_16
#define BOARD_I2C_PIN_SCL       GPIO_NUM_15
#define BOARD_I2C_CLK_HZ        400000

/* Capacitive Touch: FT6336G (FT5x06 compatible) */
#define BOARD_TP_PIN_INT        GPIO_NUM_17
#define BOARD_TP_PIN_RST        GPIO_NUM_18

/* Audio Subsystem: ES8311 Codec + FM8002E Speaker Amplifier */
#define BOARD_AUDIO_I2S_PORT    I2S_NUM_0
#define BOARD_AUDIO_PIN_PA_EN   GPIO_NUM_1    /* FM8002E SHUTDOWN: LOW = Enabled, HIGH = Muted */
#define BOARD_AUDIO_PIN_MCLK    GPIO_NUM_4
#define BOARD_AUDIO_PIN_BCLK    GPIO_NUM_5
#define BOARD_AUDIO_PIN_DOUT    GPIO_NUM_8    /* ESP32 DOUT -> ES8311 DSDIN (Speaker) */
#define BOARD_AUDIO_PIN_LRCK    GPIO_NUM_7
#define BOARD_AUDIO_PIN_DIN     GPIO_NUM_6    /* ES8311 ASDOUT -> ESP32 DIN (Mic) */

/* Misc */
#define BOARD_PIN_BOOT_BTN      GPIO_NUM_0
#define BOARD_PIN_RGB_LED       GPIO_NUM_42
