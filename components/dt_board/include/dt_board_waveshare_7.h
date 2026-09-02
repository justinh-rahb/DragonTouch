#pragma once

/*
 * Waveshare ESP32-S3-Touch-LCD-7
 *
 * ESP32-S3
 * 16 MB flash
 * 8 MB octal PSRAM
 * 800x480 RGB565 ST7262 panel
 * GT911 capacitive touch
 * CH422G I/O expander
 */

/* -------------------------------------------------------------------------- */
/* Display geometry                                                           */
/* -------------------------------------------------------------------------- */

#define DT_LCD_H_RES 800
#define DT_LCD_V_RES 480

/* -------------------------------------------------------------------------- */
/* RGB timing                                                                 */
/* -------------------------------------------------------------------------- */

#define DT_LCD_PCLK_HZ 16000000

#define DT_LCD_HSYNC_PULSE_WIDTH 4
#define DT_LCD_HSYNC_BACK_PORCH  8
#define DT_LCD_HSYNC_FRONT_PORCH 8

#define DT_LCD_VSYNC_PULSE_WIDTH 4
#define DT_LCD_VSYNC_BACK_PORCH  8
#define DT_LCD_VSYNC_FRONT_PORCH 8

#define DT_LCD_HSYNC_POLARITY    0
#define DT_LCD_VSYNC_POLARITY    0
#define DT_LCD_PCLK_ACTIVE_NEG   1
#define DT_LCD_PCLK_IDLE_HIGH    1

/* -------------------------------------------------------------------------- */
/* RGB control signals                                                        */
/* -------------------------------------------------------------------------- */

#define DT_LCD_DE_GPIO    5
#define DT_LCD_VSYNC_GPIO 3
#define DT_LCD_HSYNC_GPIO 46
#define DT_LCD_PCLK_GPIO  7

/* -------------------------------------------------------------------------- */
/* RGB565 data bus                                                            */
/*                                                                            */
/* ESP-IDF data_gpio_nums[] order is D0..D15:                                 */
/* B0..B4, G0..G5, R0..R4                                                     */
/*                                                                            */
/* Waveshare schematic labels these B3..B7, G2..G7, R3..R7.                  */
/* -------------------------------------------------------------------------- */

/* Blue: D0..D4 */
#define DT_LCD_B3_GPIO 14
#define DT_LCD_B4_GPIO 38
#define DT_LCD_B5_GPIO 18
#define DT_LCD_B6_GPIO 17
#define DT_LCD_B7_GPIO 10

/* Green: D5..D10 */
#define DT_LCD_G2_GPIO 39
#define DT_LCD_G3_GPIO 0
#define DT_LCD_G4_GPIO 45
#define DT_LCD_G5_GPIO 48
#define DT_LCD_G6_GPIO 47
#define DT_LCD_G7_GPIO 21

/* Red: D11..D15 */
#define DT_LCD_R3_GPIO 1
#define DT_LCD_R4_GPIO 2
#define DT_LCD_R5_GPIO 42
#define DT_LCD_R6_GPIO 41
#define DT_LCD_R7_GPIO 40

/*
 * LCD reset and backlight are controlled through CH422G rather than
 * directly by ESP32 GPIO.
 */
#define DT_LCD_RESET_GPIO     (-1)
#define DT_LCD_BACKLIGHT_GPIO (-1)

/* -------------------------------------------------------------------------- */
/* Shared I2C bus                                                             */
/* -------------------------------------------------------------------------- */

#define DT_TOUCH_I2C_PORT     0
#define DT_TOUCH_I2C_SDA_GPIO 8
#define DT_TOUCH_I2C_SCL_GPIO 9
#define DT_TOUCH_I2C_HZ       400000

/* -------------------------------------------------------------------------- */
/* GT911                                                                      */
/* -------------------------------------------------------------------------- */

#define DT_TOUCH_INTERRUPT_GPIO 4
#define DT_TOUCH_RESET_GPIO     (-1)

/* -------------------------------------------------------------------------- */
/* CH422G                                                                     */
/*                                                                            */
/* The chip uses fixed command addresses rather than a conventional           */
/* address + register protocol.                                               */
/* -------------------------------------------------------------------------- */

#define DT_CH422G_ADDR_CONFIG 0x24
#define DT_CH422G_ADDR_OUTPUT 0x38

#define DT_IOEXP_TOUCH_RESET   1
#define DT_IOEXP_LCD_BACKLIGHT 2
#define DT_IOEXP_LCD_RESET     3
#define DT_IOEXP_SD_CS         4
#define DT_IOEXP_USB_CAN_SEL   5
#define DT_IOEXP_LCD_VDD_EN    6

/*
 * Backlight is binary through CH422G on this board.
 */
#define DT_BACKLIGHT_PWM_HZ 0
