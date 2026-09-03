#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

PATH = Path("components/dt_board/dt_board_waveshare_7.c")

if not PATH.exists():
    sys.exit(f"ERROR: {PATH} not found")

src = PATH.read_text()

if "tear-free partial renderer active" in src:
    print("Tear-free double-framebuffer patch already applied.")
    sys.exit(0)


def find_function(text, signature):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"Function not found: {signature}")

    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace missing: {signature}")

    depth = 0
    i = brace

    in_string = False
    in_char = False
    in_line = False
    in_block = False
    escape = False

    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""

        if in_line:
            if c == "\n":
                in_line = False
            i += 1
            continue

        if in_block:
            if c == "*" and n == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                in_char = False
            i += 1
            continue

        if c == "/" and n == "/":
            in_line = True
            i += 2
            continue

        if c == "/" and n == "*":
            in_block = True
            i += 2
            continue

        if c == '"':
            in_string = True
            i += 1
            continue

        if c == "'":
            in_char = True
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
    a, b = find_function(text, signature)
    return text[:a] + replacement + text[b:]


# ----------------------------------------------------------------------
# Validate current architecture
# ----------------------------------------------------------------------

required = [
    "LVGL partial buffers: 2 x",
    ".num_fbs = 1,",
    "static void lvgl_flush_cb(",
]

for marker in required:
    if marker not in src:
        sys.exit(
            f"ERROR: expected current-source marker missing:\n"
            f"  {marker}\n"
            f"No changes made."
        )


# ----------------------------------------------------------------------
# Backup
# ----------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    Path("backups")
    / f"source-pre-double-fb-{stamp}"
    / PATH
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(PATH, backup)

print(f"Backup: {backup}")


# ----------------------------------------------------------------------
# Ensure memcpy/memset declaration
# ----------------------------------------------------------------------

include_anchor = "#include <stdint.h>\n"

if "#include <string.h>" not in src:
    if include_anchor not in src:
        sys.exit("ERROR: stdint include anchor missing")

    src = src.replace(
        include_anchor,
        include_anchor + "#include <string.h>\n",
        1
    )


# ----------------------------------------------------------------------
# Two complete RGB framebuffers
# ----------------------------------------------------------------------

src = src.replace(
    ".num_fbs = 1,",
    ".num_fbs = 2,",
    1
)


# ----------------------------------------------------------------------
# Add front/back framebuffer and dirty-region state
# ----------------------------------------------------------------------

anchor = """static TaskHandle_t s_lvgl_task;
static TaskHandle_t s_touch_task;
"""

replacement = """static TaskHandle_t s_lvgl_task;
static TaskHandle_t s_touch_task;

/*
 * Tear-free presentation:
 *
 *   front_fb = currently being scanned out
 *   back_fb  = CPU/LVGL update target
 *
 * LVGL still renders through its fast internal-SRAM partial buffers.
 */
static uint16_t *s_front_fb;
static uint16_t *s_back_fb;

static volatile bool s_fb_swap_pending;

static bool s_dirty_valid;
static int s_dirty_x1;
static int s_dirty_y1;
static int s_dirty_x2;
static int s_dirty_y2;
"""

if anchor not in src:
    sys.exit("ERROR: framebuffer state anchor missing")

src = src.replace(anchor, replacement, 1)


# ----------------------------------------------------------------------
# Add frame-boundary callback before LVGL bridge
# ----------------------------------------------------------------------

anchor = """/* -------------------------------------------------------------------------- */
/* LVGL bridge                                                                */
/* -------------------------------------------------------------------------- */
"""

callback_code = r'''
/* -------------------------------------------------------------------------- */
/* Tear-free framebuffer switching                                            */
/* -------------------------------------------------------------------------- */

static bool IRAM_ATTR rgb_frame_finish_cb(
    esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_data_t *edata,
    void *user_ctx
)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    if (!s_fb_swap_pending || s_lvgl_task == NULL) {
        return false;
    }

    s_fb_swap_pending = false;

    BaseType_t task_woken = pdFALSE;

    vTaskNotifyGiveFromISR(
        s_lvgl_task,
        &task_woken
    );

    return task_woken == pdTRUE;
}


static void dirty_region_add(const lv_area_t *area)
{
    if (!s_dirty_valid) {
        s_dirty_x1 = area->x1;
        s_dirty_y1 = area->y1;
        s_dirty_x2 = area->x2;
        s_dirty_y2 = area->y2;
        s_dirty_valid = true;
        return;
    }

    if (area->x1 < s_dirty_x1) s_dirty_x1 = area->x1;
    if (area->y1 < s_dirty_y1) s_dirty_y1 = area->y1;
    if (area->x2 > s_dirty_x2) s_dirty_x2 = area->x2;
    if (area->y2 > s_dirty_y2) s_dirty_y2 = area->y2;
}


static void copy_area_to_fb(
    uint16_t *dst_fb,
    const lv_area_t *area,
    const uint16_t *src
)
{
    const int width =
        area->x2 - area->x1 + 1;

    const size_t row_bytes =
        (size_t)width * sizeof(uint16_t);

    for (int y = area->y1; y <= area->y2; ++y) {
        uint16_t *dst =
            dst_fb +
            ((size_t)y * DT_LCD_H_RES) +
            area->x1;

        memcpy(
            dst,
            src,
            row_bytes
        );

        src += width;
    }
}


static void sync_dirty_region_between_fbs(void)
{
    if (!s_dirty_valid) {
        return;
    }

    const int width =
        s_dirty_x2 - s_dirty_x1 + 1;

    const size_t row_bytes =
        (size_t)width * sizeof(uint16_t);

    for (int y = s_dirty_y1; y <= s_dirty_y2; ++y) {
        const uint16_t *src =
            s_front_fb +
            ((size_t)y * DT_LCD_H_RES) +
            s_dirty_x1;

        uint16_t *dst =
            s_back_fb +
            ((size_t)y * DT_LCD_H_RES) +
            s_dirty_x1;

        memcpy(
            dst,
            src,
            row_bytes
        );
    }

    s_dirty_valid = false;
}


'''

if anchor not in src:
    sys.exit("ERROR: LVGL bridge anchor missing")

src = src.replace(
    anchor,
    callback_code + anchor,
    1
)


# ----------------------------------------------------------------------
# Replace flush callback
# ----------------------------------------------------------------------

new_flush = r'''static void lvgl_flush_cb(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *px_map
)
{
    /*
     * LVGL rendered this dirty rectangle into fast INTERNAL SRAM.
     * Copy it only into the framebuffer which is NOT currently being
     * scanned by the RGB engine.
     */
    copy_area_to_fb(
        s_back_fb,
        area,
        (const uint16_t *)px_map
    );

    dirty_region_add(area);

    /*
     * There may be multiple flushes in one LVGL refresh cycle.
     * Don't present the new framebuffer until every dirty rectangle
     * has been copied.
     */
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    /*
     * Clear any stale notification from an earlier LCD frame.
     */
    (void)ulTaskNotifyTake(
        pdTRUE,
        0
    );

    /*
     * color_data is exactly one of the driver's own RGB framebuffers.
     * ESP-IDF therefore does not copy it; it selects this framebuffer
     * as the driver's next source.
     */
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        0,
        DT_LCD_H_RES,
        DT_LCD_V_RES,
        s_back_fb
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "framebuffer switch request failed: %s",
            esp_err_to_name(err)
        );

        lv_display_flush_ready(display);
        return;
    }

    /*
     * In bounce-buffer mode ESP-IDF changes the source framebuffer only
     * after the complete current LCD frame has finished.
     */
    s_fb_swap_pending = true;

    if (ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(100)
        ) == 0) {

        s_fb_swap_pending = false;

        ESP_LOGW(
            TAG,
            "framebuffer swap timed out"
        );

        lv_display_flush_ready(display);
        return;
    }

    /*
     * The former back buffer is now the visible front buffer.
     */
    uint16_t *old_front = s_front_fb;

    s_front_fb = s_back_fb;
    s_back_fb = old_front;

    /*
     * Bring the now-inactive framebuffer up to the same state by copying
     * only the bounding rectangle changed in this LVGL refresh.
     *
     * This is what allows subsequent LVGL refreshes to remain PARTIAL:
     * both full framebuffers always start from identical content.
     */
    sync_dirty_region_between_fbs();

    lv_display_flush_ready(display);
}'''

try:
    src = replace_function(
        src,
        "static void lvgl_flush_cb(",
        new_flush
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ----------------------------------------------------------------------
# Add framebuffer acquisition and frame-finish callback after RGB init
# ----------------------------------------------------------------------

anchor = """    ESP_RETURN_ON_ERROR(
        waveshare_rgb_init(),
        TAG,
        "RGB initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        gt911_reset_select_address(),
"""

replacement = """    ESP_RETURN_ON_ERROR(
        waveshare_rgb_init(),
        TAG,
        "RGB initialization failed"
    );

    /*
     * ESP-IDF allocated two complete RGB565 framebuffers in PSRAM.
     * Framebuffer zero is the initial scanout source.
     */
    ESP_RETURN_ON_ERROR(
        esp_lcd_rgb_panel_get_frame_buffer(
            s_panel,
            2,
            (void **)&s_front_fb,
            (void **)&s_back_fb
        ),
        TAG,
        "failed to obtain RGB framebuffers"
    );

    memset(
        s_front_fb,
        0,
        DT_LCD_H_RES * DT_LCD_V_RES * sizeof(uint16_t)
    );

    memset(
        s_back_fb,
        0,
        DT_LCD_H_RES * DT_LCD_V_RES * sizeof(uint16_t)
    );

    const esp_lcd_rgb_panel_event_callbacks_t rgb_callbacks = {
        .on_bounce_frame_finish = rgb_frame_finish_cb,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_rgb_panel_register_event_callbacks(
            s_panel,
            &rgb_callbacks,
            NULL
        ),
        TAG,
        "failed to register RGB frame callback"
    );

    ESP_LOGI(
        TAG,
        "tear-free double framebuffer initialized"
    );

    ESP_RETURN_ON_ERROR(
        gt911_reset_select_address(),
"""

if anchor not in src:
    sys.exit("ERROR: RGB-init anchor missing")

src = src.replace(
    anchor,
    replacement,
    1
)


# ----------------------------------------------------------------------
# Add explicit mode log
# ----------------------------------------------------------------------

anchor = """    ESP_LOGI(
        TAG,
        "LVGL touch consumption period=%d ms",
        DT_LVGL_INPUT_PERIOD_MS
    );

    *display = s_lv_display;
"""

replacement = """    ESP_LOGI(
        TAG,
        "LVGL touch consumption period=%d ms",
        DT_LVGL_INPUT_PERIOD_MS
    );

    ESP_LOGI(
        TAG,
        "tear-free partial renderer active"
    );

    *display = s_lv_display;
"""

if anchor not in src:
    sys.exit("ERROR: LVGL completion anchor missing")

src = src.replace(
    anchor,
    replacement,
    1
)

PATH.write_text(src)

print(f"Patched: {PATH}")
print()
print("Rendering architecture:")
print("  LVGL: 2 small INTERNAL-SRAM partial buffers")
print("  RGB:  2 full PSRAM framebuffers")
print("  LCD:  existing internal bounce buffers")
print("  Swap: complete LCD frame boundary")
print("  Sync: only changed bounding region copied back")
