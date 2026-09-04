# DragonTouch's product target is the ESP32-S3 panel hardware (K-Touch / PandaTouch).
#
# The "headless console" is a build MODE, not a chip: it builds the dragon-core
# networking + discovery stack with the web "emulated screen" and no panel, so the
# product can be brought up on a screenless board (we test it on an ESP32-C3). It is
# deliberately independent of the target — it defaults ON for any non-S3 board (there
# is no panel to drive) and OFF on the S3 product build, and can be forced either way
# with `-DDT_HEADLESS_CONSOLE=<0|1>` (e.g. to run headless on an S3 without a panel).
#
# NOTE: component REQUIRES are expanded before Kconfig is available, so this mode is a
# CMake variable (keyed off IDF_TARGET), not a CONFIG_ option.
# Headless console vs. S3 panel. This must be decided from values visible during IDF's
# early component-requirement expansion, where only IDF_TARGET and the environment are
# available — NOT -D cache vars or CONFIG_ options (they'd desync REQUIRES from the
# compile define below). So: an env override first, else default by target (on for any
# non-S3 board, off on the S3 product). Force with `DT_HEADLESS_CONSOLE=1 idf.py build`.
if(DEFINED ENV{DT_HEADLESS_CONSOLE})
    set(DT_HEADLESS_CONSOLE $ENV{DT_HEADLESS_CONSOLE})
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(DT_HEADLESS_CONSOLE 0)
else()
    set(DT_HEADLESS_CONSOLE 1)
endif()

if(DT_HEADLESS_CONSOLE)
    set(main_reqs nvs_flash esp_hw_support dc_wifi dc_peer dc_registry dc_portal dt_console)
else()
    set(main_reqs dt_board esp_psram spi_flash heap)
endif()

idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES ${main_reqs}
)

# Expose the mode to the C code as a compile definition so app_main branches on the
# mode, not on the chip.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DT_HEADLESS_CONSOLE=${DT_HEADLESS_CONSOLE})
