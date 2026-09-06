#include "dt_lv_mem_pool.h"

#include "esp_heap_caps.h"

/*
 * DT_LVGL_MEM_POOL_PSRAM
 *
 * LVGL's built-in allocator hands out its ENTIRE app-wide memory arena --
 * every widget, style, animation, and buffer, and (via lodepng_malloc/
 * realloc/free) every PNG decode -- from ONE pool obtained exactly once,
 * at lv_mem_init() time. By default (CONFIG_LV_MEM_ADR=0x0, no
 * LV_MEM_POOL_ALLOC override) that pool is a `static` C array baked into
 * internal RAM at link time, sized by CONFIG_LV_MEM_SIZE_KILOBYTES
 * (64KB in this project). Every LVGL allocation in the whole app competes
 * for that single fixed block, and it was fragmented/full enough that
 * even a 48x48 thumbnail's PNG decode buffers couldn't find room --
 * lodepng_decode32() failing with error 83, "memory allocation failed",
 * even though the actual decode only needs a few KB.
 *
 * This function replaces that static array. LV_MEM_POOL_INCLUDE and
 * LV_MEM_POOL_ALLOC (wired into the vendored lvgl__lvgl component's own
 * build in main/CMakeLists.txt -- the vendored files themselves are not
 * touched) redirect LVGL's entire pool into PSRAM instead, where several
 * MB are free versus internal RAM's tight budget. This matches the
 * project's own rule (AGENTS.md) of keeping large buffers off internal
 * SRAM, which is reserved for the RGB panel's DMA.
 *
 * lv_mem_init() calls this exactly once, for exactly
 * CONFIG_LV_MEM_SIZE_KILOBYTES worth of bytes: LVGL's own tlsf
 * sub-allocator manages everything *within* that one block for the life
 * of the app, so there's no matching "free" hook to provide -- nothing
 * here is ever handed back to heap_caps.
 */
void *dt_lv_mem_pool_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
