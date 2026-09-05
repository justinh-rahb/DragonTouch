# Panel board layer — not built in headless console mode (no display). Keeping it out
# also avoids the S3 pin asserts on a non-S3 headless build. Same mode signal as
# main/CMakeLists (env first, else default by target), readable during early expansion.
if(DEFINED ENV{DT_HEADLESS_CONSOLE})
    set(_dt_headless $ENV{DT_HEADLESS_CONSOLE})
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(_dt_headless 0)
else()
    set(_dt_headless 1)
endif()
if(_dt_headless)
    idf_component_register()   # inert in headless mode
    return()
endif()

idf_component_register(
    SRCS "dt_board.c"
    INCLUDE_DIRS "include"
)
