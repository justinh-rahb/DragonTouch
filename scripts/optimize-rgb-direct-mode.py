#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

SRC = Path("components/dt_board/dt_board_waveshare_7.c")
SDK = Path("sdkconfig")
DEFAULTS = Path("sdkconfig.defaults")

if not SRC.exists():
    sys.exit(f"ERROR: {SRC} not found")

src = SRC.read_text()

if "LVGL DIRECT double-framebuffer mode" in src:
    print("Direct-framebuffer optimization already applied.")
    sys.exit(0)


def find_function(text, signature):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"Function not found: {signature}")

    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"Opening brace not found: {signature}")

    depth = 0
    i = brace
    in_string = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    escape = False

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
            i += 1
            continue

        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == "'":
                in_char = False
            i += 1
            continue

        if ch == "/" and nxt == "/":
            in_line_comment = True
            i += 2
            continue

        if ch == "/" and nxt == "*":
            in_block_comment = True
            i += 2
            continue

        if ch == '"':
            in_string = True
            i += 1
            continue

        if ch == "'":
            in_char = True
            i += 1
            continue

        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1

        i += 1

    raise RuntimeError(f"Closing brace not found: {signature}")


def replace_function(text, signature, replacement):
    start, end = find_function(text, signature)
    return text[:start] + replacement + text[end:]


# ---------------------------------------------------------------------
# Backup
# ---------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup_root = Path("backups") / f"source-pre-direct-rgb-{stamp}"

for path in (SRC, SDK, DEFAULTS):
    if path.exists():
        dest = backup_root / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dest)

print(f"Backup: {backup_root}")


# ---------------------------------------------------------------------
# Buffer constants
# ---------------------------------------------------------------------

old = '''#define DT_LVGL_BUFFER_ROWS   48
#define DT_LVGL_BUFFER_PIXELS (DT_LCD_H_RES * DT_LVGL_BUFFER_ROWS)
#define DT_LVGL_BUFFER_BYTES  (DT_LVGL_BUFFER_PIXELS * sizeof(uint16_t))
'''

new = '''/*
 * LVGL DIRECT double-framebuffer mode.
 *
 * Both screen-sized RGB565 buffers are owned by ESP-IDF's RGB driver
 * and live in PSRAM. LVGL renders directly into them.
 */
#define DT_FRAME_BUFFER_PIXELS (DT_LCD_H_RES * DT_LCD_V_RES)
#define DT_FRAME_BUFFER_BYTES  (DT_FRAME_BUFFER_PIXELS * sizeof(uint16_t))
'''

if old not in src:
    sys.exit("ERROR: Could not locate old LVGL buffer definitions")

src = src.replace(old, new, 1)


# ---------------------------------------------------------------------
# Two hardware framebuffers
# ---------------------------------------------------------------------

if ".num_fbs = 1," not in src:
    sys.exit("ERROR: Could not locate RGB framebuffer count")

src = src.replace(
    ".num_fbs = 1,",
    ".num_fbs = 2,",
    1
)


# ---------------------------------------------------------------------
# Add VSYNC state
# ---------------------------------------------------------------------

anchor = '''static TaskHandle_t s_lvgl_task;
static TaskHandle_t s_touch_task;
'''

replacement = '''static TaskHandle_t s_lvgl_task;
static TaskHandle_t s_touch_task;

/*
 * In DIRECT double-buffer mode, LVGL must not start modifying the
 * previous buffer until the RGB engine reaches VSYNC.
 */
static volatile bool s_waiting_for_vsync;
'''

if anchor not in src:
    sys.exit("ERROR: Task-handle anchor not found")

src = src.replace(anchor, replacement, 1)


# ---------------------------------------------------------------------
# Replace LVGL flush path
# ---------------------------------------------------------------------

new_flush = r'''static bool rgb_vsync_cb(
    esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_data_t *event_data,
    void *user_ctx
)
{
    (void)panel;
    (void)event_data;
    (void)user_ctx;

    BaseType_t task_woken = pdFALSE;

    if (s_waiting_for_vsync && s_lvgl_task != NULL) {
        s_waiting_for_vsync = false;

        vTaskNotifyGiveFromISR(
            s_lvgl_task,
            &task_woken
        );
    }

    return task_woken == pdTRUE;
}


static void lvgl_flush_cb(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *px_map
)
{
    (void)area;

    /*
     * DIRECT mode can generate several dirty-area flush callbacks for one
     * frame. LVGL has already rendered those areas into the complete
     * hardware framebuffer. Switch buffers only on the final callback.
     */
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    /*
     * Discard a stale task notification left by an earlier VSYNC.
     */
    (void)ulTaskNotifyTake(pdTRUE, 0);

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        0,
        DT_LCD_H_RES,
        DT_LCD_V_RES,
        px_map
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "DIRECT framebuffer switch failed: %s",
            esp_err_to_name(err)
        );

        lv_display_flush_ready(display);
        return;
    }

    /*
     * The next VSYNC confirms that the RGB engine has had an opportunity
     * to switch to the newly rendered framebuffer.
     */
    s_waiting_for_vsync = true;
}


static void lvgl_flush_wait_cb(lv_display_t *display)
{
    if (lv_display_flush_is_last(display)) {
        if (ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(100)
            ) == 0) {

            ESP_LOGW(
                TAG,
                "VSYNC wait timed out"
            );

            s_waiting_for_vsync = false;
        }
    }

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


# ---------------------------------------------------------------------
# Replace LVGL initialization
# ---------------------------------------------------------------------

new_init = r'''esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)
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
     * Use ESP-IDF's two actual RGB framebuffers as the LVGL draw buffers.
     * This removes the partial-buffer -> framebuffer memcpy path entirely.
     */
    ESP_RETURN_ON_ERROR(
        esp_lcd_rgb_panel_get_frame_buffer(
            s_panel,
            2,
            &s_lv_buf1,
            &s_lv_buf2
        ),
        TAG,
        "failed to obtain RGB framebuffers"
    );

    ESP_LOGI(
        TAG,
        "LVGL DIRECT buffers: 2 x %u bytes in PSRAM",
        (unsigned)DT_FRAME_BUFFER_BYTES
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

    lv_display_set_flush_wait_cb(
        s_lv_display,
        lvgl_flush_wait_cb
    );

    lv_display_set_buffers(
        s_lv_display,
        s_lv_buf1,
        s_lv_buf2,
        DT_FRAME_BUFFER_BYTES,
        LV_DISPLAY_RENDER_MODE_DIRECT
    );

    /*
     * ESP-IDF 5.3 provides VSYNC as the synchronization event for this
     * RGB controller.
     */
    const esp_lcd_rgb_panel_event_callbacks_t rgb_callbacks = {
        .on_vsync = rgb_vsync_cb,
    };

    ESP_RETURN_ON_ERROR(
        esp_lcd_rgb_panel_register_event_callbacks(
            s_panel,
            &rgb_callbacks,
            NULL
        ),
        TAG,
        "RGB VSYNC callback registration failed"
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

    ESP_LOGI(
        TAG,
        "LVGL DIRECT double-framebuffer mode active"
    );

    *display = s_lv_display;

    return ESP_OK;
}'''

try:
    src = replace_function(
        src,
        "esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)",
        new_init
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ---------------------------------------------------------------------
# Make LVGL task delay scheduler-safe and measure first render
# ---------------------------------------------------------------------

old = '''    ESP_LOGI(TAG, "rendering first DragonTouch frame");

    lv_refr_now(s_lv_display);

    ESP_LOGI(
        TAG,
        "first render complete; LVGL stack free=%u bytes",
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );
'''

new = '''    ESP_LOGI(TAG, "rendering first DragonTouch frame");

    int64_t render_start_us = esp_timer_get_time();

    lv_refr_now(s_lv_display);

    int64_t render_elapsed_us =
        esp_timer_get_time() - render_start_us;

    ESP_LOGI(
        TAG,
        "first render complete in %lld ms; LVGL stack free=%u bytes",
        (long long)(render_elapsed_us / 1000),
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );
'''

if old not in src:
    sys.exit("ERROR: First-render logging anchor not found")

src = src.replace(old, new, 1)


old = '''        vTaskDelay(pdMS_TO_TICKS(wait_ms));
'''

new = '''        TickType_t delay_ticks =
            pdMS_TO_TICKS(wait_ms);

        if (delay_ticks == 0) {
            delay_ticks = 1;
        }

        vTaskDelay(delay_ticks);
'''

if old not in src:
    sys.exit("ERROR: LVGL delay anchor not found")

src = src.replace(old, new, 1)


SRC.write_text(src)


# ---------------------------------------------------------------------
# Set LVGL default refresh period to 10 ms
# ---------------------------------------------------------------------

def set_config(path, key, value):
    if not path.exists():
        return

    text = path.read_text()
    line = f"{key}={value}"

    found = False
    output = []

    for existing in text.splitlines():
        if existing.startswith(key + "="):
            output.append(line)
            found = True
        else:
            output.append(existing)

    if not found:
        output.append(line)

    path.write_text("\n".join(output) + "\n")


set_config(
    DEFAULTS,
    "CONFIG_LV_DEF_REFR_PERIOD",
    "10"
)

set_config(
    SDK,
    "CONFIG_LV_DEF_REFR_PERIOD",
    "10"
)

print(f"Patched: {SRC}")
print("Set CONFIG_LV_DEF_REFR_PERIOD=10")
print()
print("Changes:")
print("  - RGB driver now owns two full 800x480 framebuffers")
print("  - LVGL renders directly into those framebuffers")
print("  - removed partial PSRAM copy path")
print("  - VSYNC synchronization added")
print("  - LVGL scheduler delay clamped to >= 1 RTOS tick")
print("  - first-frame render time is now measured")
