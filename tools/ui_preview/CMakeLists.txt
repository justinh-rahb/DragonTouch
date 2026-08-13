cmake_minimum_required(VERSION 3.16)
project(dragon_touch_ui_preview LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/lvgl__lvgl"
    "${CMAKE_CURRENT_BINARY_DIR}/lvgl"
)
target_link_libraries(lvgl PUBLIC PkgConfig::SDL2)

add_executable(dragon_touch_ui_preview
    preview_main.c
    ../../components/dt_ui/dt_ui.c
)
target_include_directories(dragon_touch_ui_preview PRIVATE
    host_stubs
    ../../components/dt_ui/include
)
target_link_libraries(dragon_touch_ui_preview PRIVATE lvgl PkgConfig::SDL2)
