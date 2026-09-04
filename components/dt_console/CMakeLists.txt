idf_component_register(
    SRCS "dt_console.c"
    INCLUDE_DIRS "include"
    REQUIRES dc_registry dc_peer esp_http_server esp_timer json
)
