#pragma once

/*
 * Waveshare ESP32-S3-Touch-LCD-7
 * ESP32-S3 / 16MB Flash / 8MB PSRAM
 * 800x480 RGB565 + GT911
 */

#define DT_LCD_H_RES 800
#define DT_LCD_V_RES 480

/* RGB timing */
#define DT_LCD_PCLK_HZ 16000000

#define DT_LCD_HSYNC_PULSE_WIDTH 4
#define DT_LCD_HSYNC_BACK_PORCH 8
#define DT_LCD_HSYNC_FRONT_PORCH 8

#define DT_LCD_VSYNC_PULSE_WIDTH 4
#define DT_LCD_VSYNC_BACK_PORCH 8
#define DT_LCD_VSYNC_FRONT_PORCH 8

#define DT_LCD_HSYNC_POLARITY 0
#define DT_LCD_VSYNC_POLARITY 0
#define DT_LCD_PCLK_ACTIVE_NEG 1

/* RGB control */
#define DT_LCD_DE_GPIO 5
#define DT_LCD_VSYNC_GPIO 3
#define DT_LCD_HSYNC_GPIO 46
#define DT_LCD_PCLK_GPIO 7

/* RGB565 red */
#define DT_LCD_R3_GPIO 1
#define DT_LCD_R4_GPIO 2
#define DT_LCD_R5_GPIO 42
#define DT_LCD_R6_GPIO 41
#define DT_LCD_R7_GPIO 40

/* RGB565 green */
#define DT_LCD_G2_GPIO 39
#define DT_LCD_G3_GPIO 0
#define DT_LCD_G4_GPIO 45
#define DT_LCD_G5_GPIO 48
#define DT_LCD_G6_GPIO 47
#define DT_LCD_G7_GPIO 21

/* RGB565 blue */
#define DT_LCD_B3_GPIO 14
#define DT_LCD_B4_GPIO 38
#define DT_LCD_B5_GPIO 18
#define DT_LCD_B6_GPIO 17
#define DT_LCD_B7_GPIO 10

/*
 * These are controlled by the onboard I/O expander,
 * not directly from ESP32 GPIOs.
 */
#define DT_LCD_RESET_GPIO (-1)
#define DT_LCD_BACKLIGHT_GPIO (-1)

/* GT911 touch controller */
#define DT_TOUCH_I2C_PORT 0
#define DT_TOUCH_I2C_SDA_GPIO 8
#define DT_TOUCH_I2C_SCL_GPIO 9
#define DT_TOUCH_INTERRUPT_GPIO 4
#define DT_TOUCH_RESET_GPIO (-1)
#define DT_TOUCH_I2C_HZ 400000

#define DT_BACKLIGHT_PWM_HZ 0
