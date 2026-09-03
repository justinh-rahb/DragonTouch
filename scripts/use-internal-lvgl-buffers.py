#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

SRC = Path("components/dt_board/dt_board_waveshare_7.c")
UI  = Path("components/dt_ui/dt_ui.c")

for p in (SRC, UI):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

src = SRC.read_text()
ui = UI.read_text()

if "LVGL partial buffers: 2 x" in src:
    print("Internal-buffer patch already applied.")
    sys.exit(0)

if "LVGL DIRECT double-framebuffer mode active" not in src:
    sys.exit("ERROR: current DIRECT-mode source not detected")


def find_function(text, signature):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"Function not found: {signature}")

    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace missing: {signature}")

    depth = 0
    i = brace
    string = char = line_comment = block_comment = False
    escape = False

    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""

        if line_comment:
            if c == "\n":
                line_comment = False
            i += 1
            continue

        if block_comment:
            if c == "*" and n == "/":
                block_comment = False
                i += 2
                continue
            i += 1
            continue

        if string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                string = False
            i += 1
            continue

        if char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                char = False
            i += 1
            continue

        if c == "/" and n == "/":
            line_comment = True
            i += 2
            continue

        if c == "/" and n == "*":
            block_comment = True
            i += 2
            continue

        if c == '"':
            string = True
            i += 1
            continue

        if c == "'":
            char = True
            i += 1
            continue

        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1

        i += 1

    raise RuntimeError(f"Closing brace missing: {signature}")


def replace_function(text, signature, replacement):
    start, end = find_function(text, signature)
    return text[:start] + replacement + text[end:]


def remove_function(text, signature):
    start, end = find_function(text, signature)

    while end < len(text) and text[end] == "\n":
        end += 1

    return text[:start] + text[end:]


# ------------------------------------------------------------------
# Backup
# ------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"source-pre-internal-lvgl-{stamp}"

for p in (SRC, UI):
    dst = backup / p
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Backup: {backup}")


# ------------------------------------------------------------------
# Replace full-screen DIRECT buffer constants
# ------------------------------------------------------------------

old = '''/*
 * LVGL DIRECT double-framebuffer mode.
 *
 * Both screen-sized RGB565 buffers are owned by ESP-IDF's RGB driver
 * and live in PSRAM. LVGL renders directly into them.
 */
#define DT_FRAME_BUFFER_PIXELS (DT_LCD_H_RES * DT_LCD_V_RES)
#define DT_FRAME_BUFFER_BYTES  (DT_FRAME_BUFFER_PIXELS * sizeof(uint16_t))
'''

new = '''/*
 * LVGL renders into small INTERNAL-RAM partial buffers.
 *
 * 24 rows:
 *   800 * 24 * 2 bytes = 38,400 bytes per buffer
 *   76,800 bytes total for two buffers
 *
 * The RGB driver owns one complete framebuffer in PSRAM.
 */
#define DT_LVGL_BUFFER_ROWS   24
#define DT_LVGL_BUFFER_PIXELS (DT_LCD_H_RES * DT_LVGL_BUFFER_ROWS)
#define DT_LVGL_BUFFER_BYTES  (DT_LVGL_BUFFER_PIXELS * sizeof(uint16_t))
'''

if old not in src:
    sys.exit("ERROR: DIRECT framebuffer constants not found")

src = src.replace(old, new, 1)


# ------------------------------------------------------------------
# Single RGB framebuffer
# ------------------------------------------------------------------

if ".num_fbs = 2," not in src:
    sys.exit("ERROR: num_fbs=2 not found")

src = src.replace(
    ".num_fbs = 2,",
    ".num_fbs = 1,",
    1
)


# ------------------------------------------------------------------
# Remove DIRECT/VSYNC state
# ------------------------------------------------------------------

direct_state = '''/*
 * In DIRECT double-buffer mode, LVGL must not start modifying the
 * previous buffer until the RGB engine reaches VSYNC.
 */
static volatile bool s_waiting_for_vsync;
'''

src = src.replace(direct_state, "")


# ------------------------------------------------------------------
# Remove DIRECT-mode VSYNC functions
# ------------------------------------------------------------------

for signature in (
    "static bool rgb_vsync_cb(",
    "static void lvgl_flush_wait_cb(",
):
    if signature in src:
        src = remove_function(src, signature)


# ------------------------------------------------------------------
# Normal partial-buffer flush
# ------------------------------------------------------------------

flush = r'''static void lvgl_flush_cb(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *px_map
)
{
    /*
     * px_map lives in fast internal SRAM.
     *
     * esp_lcd copies only this dirty rectangle into the RGB driver's
     * complete PSRAM framebuffer.
     */
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        px_map
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "LVGL flush failed: %s",
            esp_err_to_name(err)
        );
    }

    lv_display_flush_ready(display);
}'''

try:
    src = replace_function(
        src,
        "static void lvgl_flush_cb(",
        flush
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ------------------------------------------------------------------
# Replace complete LVGL initialization
# ------------------------------------------------------------------

init = r'''esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Waveshare LVGL hardware initialization");

    ESP_RETURN_ON_ERROR(
        waveshare_i2c_init(),
        TAG,
        "I2C initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        waveshare_expander_init(),
        TAG,
        "CH422G initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        waveshare_rgb_init(),
        TAG,
        "RGB initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        gt911_reset_select_address(),
        TAG,
        "GT911 reset failed"
    );

    ESP_RETURN_ON_ERROR(
        gt911_probe(),
        TAG,
        "GT911 probe failed"
    );

    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);

    /*
     * Render buffers intentionally live in INTERNAL SRAM.
     *
     * The LCD driver's complete framebuffer remains in PSRAM.
     */
    s_lv_buf1 = heap_caps_malloc(
        DT_LVGL_BUFFER_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    s_lv_buf2 = heap_caps_malloc(
        DT_LVGL_BUFFER_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (s_lv_buf1 == NULL || s_lv_buf2 == NULL) {
        ESP_LOGE(
            TAG,
            "internal LVGL buffer allocation failed; "
            "largest internal block=%u",
            (unsigned)heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            )
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "LVGL partial buffers: 2 x %u bytes INTERNAL SRAM",
        (unsigned)DT_LVGL_BUFFER_BYTES
    );

    ESP_LOGI(
        TAG,
        "internal heap remaining=%u bytes",
        (unsigned)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        )
    );

    s_lv_display = lv_display_create(
        DT_LCD_H_RES,
        DT_LCD_V_RES
    );

    if (s_lv_display == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_color_format(
        s_lv_display,
        LV_COLOR_FORMAT_RGB565
    );

    lv_display_set_flush_cb(
        s_lv_display,
        lvgl_flush_cb
    );

    lv_display_set_buffers(
        s_lv_display,
        s_lv_buf1,
        s_lv_buf2,
        DT_LVGL_BUFFER_BYTES,
        LV_DISPLAY_RENDER_MODE_PARTIAL
    );

    lv_display_set_default(s_lv_display);

    s_lv_touch = lv_indev_create();

    if (s_lv_touch == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_indev_set_type(
        s_lv_touch,
        LV_INDEV_TYPE_POINTER
    );

    lv_indev_set_read_cb(
        s_lv_touch,
        lvgl_touch_read_cb
    );

    lv_indev_set_display(
        s_lv_touch,
        s_lv_display
    );

    lv_timer_t *touch_read_timer =
        lv_indev_get_read_timer(s_lv_touch);

    if (touch_read_timer != NULL) {
        lv_timer_set_period(
            touch_read_timer,
            DT_LVGL_INPUT_PERIOD_MS
        );
    }

    ESP_LOGI(
        TAG,
        "LVGL touch consumption period=%d ms",
        DT_LVGL_INPUT_PERIOD_MS
    );

    *display = s_lv_display;

    return ESP_OK;
}'''

try:
    src = replace_function(
        src,
        "esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)",
        init
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ------------------------------------------------------------------
# Instrument LVGL handler latency
# ------------------------------------------------------------------

old = '''        uint32_t wait_ms = lv_timer_handler();
'''

new = '''        int64_t handler_start_us = esp_timer_get_time();

        uint32_t wait_ms = lv_timer_handler();

        int64_t handler_us =
            esp_timer_get_time() - handler_start_us;

        if (handler_us > 50000) {
            ESP_LOGW(
                TAG,
                "slow LVGL handler: %lld ms",
                (long long)(handler_us / 1000)
            );
        }
'''

if old not in src:
    sys.exit("ERROR: lv_timer_handler call not found")

src = src.replace(old, new, 1)


SRC.write_text(src)


# ------------------------------------------------------------------
# Restore upstream navigation semantics
# ------------------------------------------------------------------

if "LV_EVENT_PRESSED" in ui:
    ui = ui.replace(
        "LV_EVENT_PRESSED",
        "LV_EVENT_CLICKED"
    )

UI.write_text(ui)

print(f"Patched: {SRC}")
print(f"Patched: {UI}")
print()
print("New rendering architecture:")
print("  - 1 full RGB framebuffer in PSRAM")
print("  - RGB driver's internal DMA bounce buffers retained")
print("  - 2 x 24-row LVGL render buffers in INTERNAL SRAM")
print("  - PARTIAL LVGL rendering")
print("  - custom VSYNC wait removed")
print("  - navigation restored to LV_EVENT_CLICKED")
print("  - LVGL handler calls >50 ms will be logged")
