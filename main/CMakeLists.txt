idf_component_register(
    SRCS "app_main.c" "dt_runtime.c" "dt_portal.c" "dt_printers.c"
    INCLUDE_DIRS "."
    REQUIRES
        dt_board
        dt_ui
        dt_config
        esp_psram
        spi_flash
        heap
 esp_lvgl_port nvs_flash esp_wifi dc_wifi dc_moonraker esp_http_client json dc_portal esp_http_server)

# DT_LVGL_MEM_POOL_PSRAM
#
# Redirect the vendored lvgl__lvgl component's built-in allocator pool
# (normally a fixed 64KB static array in internal RAM -- see
# dt_lv_mem_pool.c for the full story) into PSRAM, without editing any
# vendored files.
#
# dt_lv_mem_pool.c is added as a SOURCE of the lvgl__lvgl target itself
# (via target_sources below), NOT of this "main" component. It must live
# in the SAME static archive as lv_mem_core_builtin.c, its only caller --
# ESP-IDF links each component as a lazily-included static archive, and an
# object file compiled into main's own archive is only pulled in if
# something else in main references it. Nothing in main does; only
# lv_mem_core_builtin.c (inside liblvgl__lvgl.a) does. Compiling it
# straight into lvgl__lvgl's own archive sidesteps that entirely.
#
# idf_component_get_property()/target_sources()/target_compile_definitions()
# on another component's already-registered target is the standard
# ESP-IDF pattern for reaching into a component from outside it; it works
# regardless of the order components' CMakeLists.txt files happen to be
# processed in, since component target names are registered in an early
# pass before any component's build settings are finalized.
idf_component_get_property(dt_lvgl_component_lib lvgl__lvgl COMPONENT_LIB)
target_sources(${dt_lvgl_component_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/dt_lv_mem_pool.c")
target_include_directories(${dt_lvgl_component_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(${dt_lvgl_component_lib} PRIVATE
    "LV_MEM_POOL_INCLUDE=\"dt_lv_mem_pool.h\""
    "LV_MEM_POOL_ALLOC=dt_lv_mem_pool_alloc"
)

# dt_lv_mem_pool.c calls heap_caps_malloc()/MALLOC_CAP_SPIRAM from
# esp_heap_caps.h -- give lvgl__lvgl's target the "heap" component's
# public include dir explicitly (idf::<component> is the alias target
# ESP-IDF creates for every component precisely for this kind of
# cross-component linking) rather than assuming it's already reachable.
target_link_libraries(${dt_lvgl_component_lib} PRIVATE idf::heap)
