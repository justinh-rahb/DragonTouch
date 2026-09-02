#include "dt_board.h"

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

esp_err_t dt_board_waveshare_7_display_test_init(void);

#endif


esp_err_t dt_board_display_test_init(void)
{
#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

    return dt_board_waveshare_7_display_test_init();

#else

    /*
     * K-Touch hardware bring-up remains intentionally unchanged.
     */
    return ESP_ERR_NOT_SUPPORTED;

#endif
}
