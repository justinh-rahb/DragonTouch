#include <inttypes.h>
#include <stdio.h>

#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32C3
// Headless console bring-up board (ESP32-C3 super-mini, no panel). Brings up the
// dragon-core networking + discovery stack and an "emulated screen" (dt_console),
// proving family discovery + status end to end without the display driver.
#include "nvs_flash.h"
#include "esp_mac.h"
#include "dc_wifi.h"
#include "dc_peer.h"
#include "dc_registry.h"
#include "dt_console.h"
#else
// S3 panel groundwork (unchanged): identify the board, do not touch the display yet.
#include "dt_board.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#endif

static const char *TAG = "dragon_touch";

#if CONFIG_IDF_TARGET_ESP32C3
static void headless_console_main(void)
{
    ESP_LOGI(TAG, "DragonTouch headless console (C3 bring-up board)");

    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    const dc_wifi_identity_t id = {
        .hostname = "dragontouch",
        .instance_name = "DragonTouch",
        .ap_ssid_prefix = "DragonTouch_",
        .ap_password = DC_WIFI_DEFAULT_AP_PASSWORD,
    };
    ESP_ERROR_CHECK(dc_wifi_set_identity(&id));
    ESP_ERROR_CHECK(dc_wifi_start());

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char self_id[DC_PEER_ID_MAX];
    snprintf(self_id, sizeof self_id, "dragontouch-%02x%02x", mac[4], mac[5]);
    esp_err_t pe = dc_peer_start(self_id);
    if (pe != ESP_OK) ESP_LOGW(TAG, "dc_peer_start: %s (continuing)", esp_err_to_name(pe));

    ESP_ERROR_CHECK(dc_registry_start());
    ESP_ERROR_CHECK(dt_console_start());
    ESP_LOGI(TAG, "console up as '%s' — discovering family devices", self_id);
}
#else
static void groundwork_main(void)
{
    esp_chip_info_t chip = {0};
    uint32_t flash_bytes = 0;

    esp_chip_info(&chip);
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_bytes));

    ESP_LOGI(TAG, "DragonTouch groundwork boot");
    ESP_LOGI(TAG, "chip cores=%u revision=%u flash=%" PRIu32 " bytes",
             chip.cores, chip.revision, flash_bytes);
    ESP_LOGI(TAG, "PSRAM detected=%u bytes; internal heap=%u bytes",
             (unsigned)esp_psram_get_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGW(TAG, "display and backlight are intentionally not initialized yet");
    ESP_LOGI(TAG, "board contract: %ux%u RGB panel, GT911 touch",
             DT_LCD_H_RES, DT_LCD_V_RES);
}
#endif

void app_main(void)
{
#if CONFIG_IDF_TARGET_ESP32C3
    headless_console_main();
#else
    groundwork_main();
#endif
}
