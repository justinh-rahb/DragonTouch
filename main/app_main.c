#include <inttypes.h>

#include "dt_board.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"


static const char *TAG = "dragon_touch";


void app_main(void)
{
    esp_chip_info_t chip = {0};
    uint32_t flash_bytes = 0;

    esp_chip_info(&chip);

    ESP_ERROR_CHECK(
        esp_flash_get_size(NULL, &flash_bytes)
    );

    ESP_LOGI(TAG, "DragonTouch groundwork boot");

    ESP_LOGI(
        TAG,
        "chip cores=%u revision=%u flash=%" PRIu32 " bytes",
        chip.cores,
        chip.revision,
        flash_bytes
    );

    ESP_LOGI(
        TAG,
        "PSRAM detected=%u bytes; internal heap=%u bytes",
        (unsigned)esp_psram_get_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
    );

    ESP_LOGI(
        TAG,
        "board: %s",
        DT_BOARD_NAME
    );

    ESP_LOGI(
        TAG,
        "board contract: %ux%u RGB panel, GT911 touch",
        DT_LCD_H_RES,
        DT_LCD_V_RES
    );

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

    ESP_LOGI(TAG, "starting Waveshare RGB display test");

    ESP_ERROR_CHECK(
        dt_board_display_test_init()
    );

#else

    ESP_LOGW(
        TAG,
        "display and backlight intentionally not initialized on this target"
    );

#endif
}
