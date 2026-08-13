idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES dt_board esp_psram spi_flash heap
)
