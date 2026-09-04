# The ESP32-C3 build is the headless console bring-up board: it pulls in the
# dragon-core networking + discovery stack and the emulated-screen web console.
# Every other target keeps the S3 panel groundwork (dt_board only).
if(IDF_TARGET STREQUAL "esp32c3")
    set(main_reqs nvs_flash esp_hw_support dc_wifi dc_peer dc_registry dc_portal dt_console)
else()
    set(main_reqs dt_board esp_psram spi_flash heap)
endif()

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES ${main_reqs}
)
