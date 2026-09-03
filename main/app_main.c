#include <inttypes.h>

#include "dt_board.h"
#include "dt_runtime.h"
#include "dt_ui.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "nvs_flash.h"

static const char *TAG = "dragon_touch";


static void init_nvs(void)
{
    esp_err_t err =
        nvs_flash_init();

    if (
        err ==
            ESP_ERR_NVS_NO_FREE_PAGES ||
        err ==
            ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ESP_ERROR_CHECK(
            nvs_flash_init()
        );

    } else {
        ESP_ERROR_CHECK(err);
    }
}


void app_main(void)
{
    esp_chip_info_t chip = {0};
    uint32_t flash_bytes = 0;

    esp_chip_info(&chip);

    ESP_ERROR_CHECK(
        esp_flash_get_size(
            NULL,
            &flash_bytes
        )
    );

    ESP_LOGI(
        TAG,
        "DragonTouch hardware UI boot"
    );

    ESP_LOGI(
        TAG,
        "chip cores=%u revision=%u "
        "flash=%" PRIu32 " bytes",
        chip.cores,
        chip.revision,
        flash_bytes
    );

    ESP_LOGI(
        TAG,
        "PSRAM=%u bytes; "
        "internal heap=%u bytes",
        (unsigned)
            esp_psram_get_size(),
        (unsigned)
            heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL
            )
    );

    init_nvs();

    lv_display_t *display = NULL;

    ESP_ERROR_CHECK(
        dt_board_lvgl_init(
            &display
        )
    );

    ESP_ERROR_CHECK(
        dt_ui_create(
            display
        )
    );

    ESP_ERROR_CHECK(
        dt_board_lvgl_start()
    );

    ESP_LOGI(
        TAG,
        "DragonTouch UI running"
    );

    /*
     * Network/runtime starts only after the local console is
     * healthy. Network failure is non-fatal.
     */
    esp_err_t runtime_err =
        dt_runtime_start();

    if (runtime_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "runtime start failed: %s; "
            "local UI remains available",
            esp_err_to_name(
                runtime_err
            )
        );
    }
}
