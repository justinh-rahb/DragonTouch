# LVGL panel UI — not built in headless console mode. Skipping registration here keeps
# LVGL (a heavy dependency) out of the console binary entirely. Same mode signal as
# main/CMakeLists (env first, else default by target).
if(DEFINED ENV{DT_HEADLESS_CONSOLE})
    set(_dt_headless $ENV{DT_HEADLESS_CONSOLE})
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(_dt_headless 0)
else()
    set(_dt_headless 1)
endif()
if(_dt_headless)
    idf_component_register()   # inert in headless mode (no LVGL)
    return()
endif()

idf_component_register(
    SRCS "dt_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
)
