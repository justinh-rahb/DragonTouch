idf_component_register(
    SRCS "app_main.c" "dt_runtime.c" "dt_portal.c"
    INCLUDE_DIRS "."
    REQUIRES
        dt_board
        dt_ui
        esp_psram
        spi_flash
        heap
 esp_lvgl_port nvs_flash esp_wifi dc_wifi dc_moonraker esp_http_client json dc_portal esp_http_server)
