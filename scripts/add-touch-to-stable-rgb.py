#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
APP = Path("main/app_main.c")

for p in (BOARD, APP):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

src = BOARD.read_text()

# ------------------------------------------------------------------
# Refuse to run unless we're on the known-good RGB baseline.
# ------------------------------------------------------------------

required = [
    ".num_fbs = 1,",
    ".bounce_buffer_size_px = (DT_LCD_H_RES * 10),",
    "port_cfg.task_affinity = 0;",
    ".buff_dma = true,",
    ".direct_mode = false,",
    ".bb_mode = true,",
    ".avoid_tearing = false,",
    "static void gt911_poll_task(",
    "static void lvgl_touch_read_cb(",
    "static esp_err_t gt911_reset_select_address(",
    "static esp_err_t gt911_probe(",
]

for marker in required:
    if marker not in src:
        sys.exit(
            "ERROR: expected baseline marker missing:\n"
            f"  {marker}\n"
            "Nothing modified."
        )


# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------

def find_function(text, signature):
    start = text.find(signature)

    if start < 0:
        raise RuntimeError(f"function missing: {signature}")

    brace = text.find("{", start)

    if brace < 0:
        raise RuntimeError(f"opening brace missing: {signature}")

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

    raise RuntimeError(f"closing brace missing: {signature}")


# ------------------------------------------------------------------
# Safety backup
# ------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"pre-stable-touch-{stamp}"

for p in (BOARD, APP):
    dst = backup / p
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Backup: {backup}")


# ------------------------------------------------------------------
# Modify ONLY the init function.
# ------------------------------------------------------------------

start, end = find_function(
    src,
    "esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)"
)

init = src[start:end]

# Re-enable the already-validated GT911 reset/probe.
if "gt911_reset_select_address()" not in init:
    anchor = '''    ESP_RETURN_ON_ERROR(
        waveshare_rgb_init(),
        TAG,
        "RGB initialization failed"
    );
'''

    addition = anchor + '''
    /*
     * GT911 hardware bring-up.
     * This is the same reset/address sequence validated during raw-touch
     * bring-up. It does not modify RGB/LCD configuration.
     */
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
'''

    if anchor not in init:
        sys.exit("ERROR: RGB initialization anchor missing")

    init = init.replace(anchor, addition, 1)


# Register cached GT911 state as a normal LVGL pointer device while
# esp_lvgl_port's mutex is held.
if "stable GT911 LVGL input registered" not in init:
    anchor = '''    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }
'''

    addition = anchor + '''
    /*
     * GT911 I2C is NOT performed from this callback.
     *
     * gt911_poll_task() updates cached x/y/pressed state asynchronously;
     * lvgl_touch_read_cb() only copies that cached state.
     */
    s_lv_touch = lv_indev_create();

    if (s_lv_touch == NULL) {
        lvgl_port_unlock();
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

    lv_timer_t *touch_timer =
        lv_indev_get_read_timer(s_lv_touch);

    if (touch_timer != NULL) {
        lv_timer_set_period(
            touch_timer,
            DT_LVGL_INPUT_PERIOD_MS
        );
    }

    ESP_LOGI(
        TAG,
        "stable GT911 LVGL input registered (%d ms)",
        DT_LVGL_INPUT_PERIOD_MS
    );
'''

    if anchor not in init:
        sys.exit("ERROR: lvgl_port_lock anchor missing")

    init = init.replace(anchor, addition, 1)

src = src[:start] + init + src[end:]


# ------------------------------------------------------------------
# Modify ONLY the start function.
# Keep RGB configuration completely untouched.
# ------------------------------------------------------------------

start, end = find_function(
    src,
    "esp_err_t dt_board_waveshare_7_lvgl_start(void)"
)

start_fn = src[start:end]

if "GT911 acquisition task started on CPU1" not in start_fn:
    anchor = '''    ESP_LOGI(
        TAG,
        "LCD backlight enabled; Espressif LVGL task owns display"
    );
'''

    addition = anchor + '''
    /*
     * Keep I2C touch acquisition off the display/LVGL core.
     *
     * CPU0:
     *   RGB initialization + esp_lvgl_port
     *
     * CPU1:
     *   low-cost GT911 polling
     */
    if (s_touch_task == NULL) {
        BaseType_t result = xTaskCreatePinnedToCore(
            gt911_poll_task,
            "gt911",
            4096,
            NULL,
            4,
            &s_touch_task,
            1
        );

        if (result != pdPASS) {
            s_touch_task = NULL;

            ESP_LOGE(
                TAG,
                "failed to create GT911 task"
            );

            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(
            TAG,
            "GT911 acquisition task started on CPU1"
        );
    }
'''

    if anchor not in start_fn:
        sys.exit("ERROR: backlight log anchor missing")

    start_fn = start_fn.replace(anchor, addition, 1)

src = src[:start] + start_fn + src[end:]

BOARD.write_text(src)


# ------------------------------------------------------------------
# Static touch diagnostic.
#
# Nothing moves.
# No full-screen redraw.
# Four corner references should remain absolutely fixed.
# ------------------------------------------------------------------

APP.write_text(r'''#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#include "lvgl.h"

#include "dt_board.h"

static const char *TAG = "touch_diag";

static lv_obj_t *s_status;
static unsigned s_count;


static void button_event(lv_event_t *event)
{
    const unsigned id =
        (unsigned)(uintptr_t)
        lv_event_get_user_data(event);

    ++s_count;

    lv_label_set_text_fmt(
        s_status,
        "TOUCH %u     COUNT %u",
        id + 1,
        s_count
    );

    ESP_LOGI(
        TAG,
        "button=%u count=%u",
        id + 1,
        s_count
    );
}


static lv_obj_t *marker(
    lv_obj_t *parent,
    const char *text,
    lv_align_t align,
    int32_t x,
    int32_t y
)
{
    lv_obj_t *box =
        lv_obj_create(parent);

    lv_obj_set_size(
        box,
        145,
        55
    );

    lv_obj_set_style_radius(
        box,
        0,
        0
    );

    lv_obj_set_style_bg_color(
        box,
        lv_color_hex(0x202020),
        0
    );

    lv_obj_set_style_bg_opa(
        box,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_border_width(
        box,
        3,
        0
    );

    lv_obj_set_style_border_color(
        box,
        lv_color_hex(0xFFFFFF),
        0
    );

    lv_obj_align(
        box,
        align,
        x,
        y
    );

    lv_obj_t *label =
        lv_label_create(box);

    lv_label_set_text(
        label,
        text
    );

    lv_obj_center(label);

    return box;
}


static void create_ui(lv_display_t *display)
{
    lv_obj_t *screen =
        lv_display_get_screen_active(display);

    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(0x101010),
        0
    );

    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        0
    );


    /*
     * Permanent drift references.
     */
    marker(
        screen,
        "TOP LEFT",
        LV_ALIGN_TOP_LEFT,
        10,
        10
    );

    marker(
        screen,
        "TOP RIGHT",
        LV_ALIGN_TOP_RIGHT,
        -10,
        10
    );

    marker(
        screen,
        "BOTTOM LEFT",
        LV_ALIGN_BOTTOM_LEFT,
        10,
        -10
    );

    marker(
        screen,
        "BOTTOM RIGHT",
        LV_ALIGN_BOTTOM_RIGHT,
        -10,
        -10
    );


    /*
     * Six fixed touch targets.
     */
    for (unsigned i = 0; i < 6; ++i) {
        lv_obj_t *button =
            lv_button_create(screen);

        const int col = i % 3;
        const int row = i / 3;

        lv_obj_set_size(
            button,
            150,
            65
        );

        lv_obj_set_pos(
            button,
            165 + col * 165,
            150 + row * 85
        );

        lv_obj_set_style_radius(
            button,
            2,
            0
        );

        lv_obj_set_style_shadow_width(
            button,
            0,
            0
        );

        lv_obj_add_event_cb(
            button,
            button_event,
            LV_EVENT_PRESSED,
            (void *)(uintptr_t)i
        );

        lv_obj_t *label =
            lv_label_create(button);

        lv_label_set_text_fmt(
            label,
            "BUTTON %u",
            i + 1
        );

        lv_obj_center(label);
    }


    /*
     * Only this small label changes after startup.
     */
    s_status =
        lv_label_create(screen);

    lv_label_set_text(
        s_status,
        "TOUCH A BUTTON"
    );

    lv_obj_align(
        s_status,
        LV_ALIGN_CENTER,
        0,
        -100
    );

    ESP_LOGI(
        TAG,
        "stable RGB + GT911 diagnostic created"
    );
}


void app_main(void)
{
    lv_display_t *display = NULL;

    ESP_LOGI(
        TAG,
        "starting stable touch diagnostic"
    );

    ESP_ERROR_CHECK(
        dt_board_lvgl_init(&display)
    );

    create_ui(display);

    ESP_ERROR_CHECK(
        dt_board_lvgl_start()
    );

    ESP_LOGI(
        TAG,
        "diagnostic running"
    );
}
''')

print(f"Patched: {BOARD}")
print(f"Patched: {APP}")
print()
print("RGB configuration was NOT changed.")
print()
print("Added:")
print("  - GT911 reset/probe")
print("  - cached LVGL pointer input")
print("  - GT911 acquisition task on CPU1")
print("  - static touch diagnostic")
