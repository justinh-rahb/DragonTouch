idf_component_register(
    SRCS
        "dt_board.c"
        "dt_board_waveshare_7.c"

    INCLUDE_DIRS
        "include"

    REQUIRES esp_lvgl_port
        esp_lcd
        driver
        esp_timer
        heap
        lvgl
)
