#include "dt_board.h"

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display);
esp_err_t dt_board_waveshare_7_lvgl_start(void);

#endif


esp_err_t dt_board_lvgl_init(lv_display_t **display)
{
#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)
    return dt_board_waveshare_7_lvgl_init(display);
#else
    (void)display;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


esp_err_t dt_board_lvgl_start(void)
{
#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)
    return dt_board_waveshare_7_lvgl_start();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
