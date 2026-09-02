idf_component_register(
    SRCS
        "dt_board.c"
        "dt_board_waveshare_7.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_lcd
        driver
)
