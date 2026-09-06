#pragma once

/*
 * DT_LVGL_MEM_POOL_PSRAM
 *
 * See dt_lv_mem_pool.c for the full story. This header is included from
 * inside the vendored lvgl__lvgl component itself (via the LV_MEM_POOL_
 * INCLUDE macro wired up in this component's CMakeLists.txt), not from
 * our own application code -- keep it dependency-free.
 */

#include <stddef.h>

void *dt_lv_mem_pool_alloc(size_t size);
