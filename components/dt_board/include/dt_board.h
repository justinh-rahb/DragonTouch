#pragma once

#include "sdkconfig.h"
#include "esp_err.h"

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

#include "dt_board_waveshare_7.h"
#define DT_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-7"

#else

#include "dt_board_ktouch.h"
#define DT_BOARD_NAME "BIGTREETECH K-Touch / PandaTouch"

#endif

/*
 * Hardware bring-up API.
 *
 * This remains deliberately small while the board implementations are
 * established and validated on hardware.
 */
esp_err_t dt_board_display_test_init(void);
