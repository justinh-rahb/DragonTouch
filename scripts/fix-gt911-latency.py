#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

PATH = Path("components/dt_board/dt_board_waveshare_7.c")

if not PATH.exists():
    sys.exit(f"ERROR: {PATH} not found")

src = PATH.read_text()

if 'GT911 acquisition task started (5 ms)' in src:
    print("GT911 latency fix already applied.")
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


# --------------------------------------------------------------------
# Validate expected starting point
# --------------------------------------------------------------------

required = [
    "static esp_err_t gt911_sample(void)",
    "static void lvgl_touch_read_cb(",
    'LVGL service task started (stack=24576 bytes)',
]

for marker in required:
    if marker not in src:
        sys.exit(
            f"ERROR: Expected marker not found:\n  {marker}\n"
            "Source was not modified."
        )


# --------------------------------------------------------------------
# Backup
# --------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")

backup = (
    Path("backups")
    / f"source-pre-touch-latency-{stamp}"
    / PATH
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(PATH, backup)

print(f"Backup: {backup}")


# --------------------------------------------------------------------
# Add timing constants
# --------------------------------------------------------------------

anchor = "#define GT911_MAX_POINTS      5\n"

replacement = """#define GT911_MAX_POINTS      5

/*
 * Hardware acquisition and LVGL consumption are deliberately decoupled.
 *
 * GT911 is sampled quickly in its own task. LVGL's read callback only
 * copies cached state and therefore never performs blocking I2C.
 */
#define DT_GT911_POLL_MS         5
#define DT_LVGL_INPUT_PERIOD_MS 10
"""

if anchor not in src:
    sys.exit("ERROR: GT911_MAX_POINTS anchor missing")

src = src.replace(anchor, replacement, 1)


# --------------------------------------------------------------------
# Add touch-task state and synchronization
# --------------------------------------------------------------------

anchor = "static TaskHandle_t s_lvgl_task;\n"

replacement = """static TaskHandle_t s_lvgl_task;
static TaskHandle_t s_touch_task;

/*
 * Protect the tiny cached touch-state structure.
 * No LVGL calls occur while this lock is held.
 */
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
"""

if anchor not in src:
    sys.exit("ERROR: s_lvgl_task anchor missing")

src = src.replace(anchor, replacement, 1)


# --------------------------------------------------------------------
# Replace blocking gt911_sample() with dedicated acquisition task
# --------------------------------------------------------------------

new_touch_task = r'''static void gt911_poll_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "GT911 acquisition task started (%d ms)",
        DT_GT911_POLL_MS
    );

    TickType_t wake_time = xTaskGetTickCount();

    uint32_t read_errors = 0;

    while (true) {
        uint8_t status = 0;

        esp_err_t err = gt911_read(
            GT911_REG_STATUS,
            &status,
            1
        );

        if (err == ESP_OK && (status & 0x80) != 0) {
            const uint8_t count = status & 0x0F;

            if (count == 0) {
                /*
                 * Explicit release packet.
                 */
                taskENTER_CRITICAL(&s_touch_lock);

                s_touch_pressed = false;

                taskEXIT_CRITICAL(&s_touch_lock);

                err = gt911_write_u8(
                    GT911_REG_STATUS,
                    0
                );

            } else if (count <= GT911_MAX_POINTS) {
                /*
                 * DragonTouch currently needs only the primary contact.
                 */
                uint8_t point[8];

                err = gt911_read(
                    GT911_REG_POINTS,
                    point,
                    sizeof(point)
                );

                if (err == ESP_OK) {
                    uint16_t x =
                        (uint16_t)point[1] |
                        ((uint16_t)point[2] << 8);

                    uint16_t y =
                        (uint16_t)point[3] |
                        ((uint16_t)point[4] << 8);

                    /*
                     * Our physical hardware test established native
                     * landscape orientation with no transform required.
                     */
                    if (x >= DT_LCD_H_RES) {
                        x = DT_LCD_H_RES - 1;
                    }

                    if (y >= DT_LCD_V_RES) {
                        y = DT_LCD_V_RES - 1;
                    }

                    taskENTER_CRITICAL(&s_touch_lock);

                    s_touch_x = x;
                    s_touch_y = y;
                    s_touch_pressed = true;

                    taskEXIT_CRITICAL(&s_touch_lock);

                    /*
                     * Acknowledge only after coordinate data was read.
                     */
                    err = gt911_write_u8(
                        GT911_REG_STATUS,
                        0
                    );
                }

            } else {
                ESP_LOGW(
                    TAG,
                    "GT911 invalid touch count=%u",
                    count
                );

                err = gt911_write_u8(
                    GT911_REG_STATUS,
                    0
                );
            }
        }

        if (err != ESP_OK) {
            read_errors++;

            /*
             * Avoid flooding the console if I2C ever becomes unhappy.
             */
            if (read_errors == 1 || (read_errors % 100) == 0) {
                ESP_LOGW(
                    TAG,
                    "GT911 I2C error #%u: %s",
                    (unsigned)read_errors,
                    esp_err_to_name(err)
                );
            }
        }

        vTaskDelayUntil(
            &wake_time,
            pdMS_TO_TICKS(DT_GT911_POLL_MS)
        );
    }
}'''

try:
    src = replace_function(
        src,
        "static esp_err_t gt911_sample(void)",
        new_touch_task
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# --------------------------------------------------------------------
# Replace LVGL callback with zero-I2C cached-state callback
# --------------------------------------------------------------------

new_read_cb = r'''static void lvgl_touch_read_cb(
    lv_indev_t *indev,
    lv_indev_data_t *data
)
{
    (void)indev;

    uint16_t x;
    uint16_t y;
    bool pressed;

    /*
     * Never perform I2C from the LVGL task.
     *
     * The GT911 task continuously updates this cache. The critical
     * section lasts only long enough to copy 5 bytes of state.
     */
    taskENTER_CRITICAL(&s_touch_lock);

    x = s_touch_x;
    y = s_touch_y;
    pressed = s_touch_pressed;

    taskEXIT_CRITICAL(&s_touch_lock);

    data->point.x = x;
    data->point.y = y;

    data->state =
        pressed
            ? LV_INDEV_STATE_PRESSED
            : LV_INDEV_STATE_RELEASED;
}'''

try:
    src = replace_function(
        src,
        "static void lvgl_touch_read_cb(",
        new_read_cb
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# --------------------------------------------------------------------
# Increase LVGL input sampling frequency to 10ms.
# --------------------------------------------------------------------

anchor = """    lv_indev_set_display(
        s_lv_touch,
        s_lv_display
    );

    *display = s_lv_display;
"""

replacement = """    lv_indev_set_display(
        s_lv_touch,
        s_lv_display
    );

    /*
     * LVGL's default input polling follows LV_DEF_REFR_PERIOD.
     * Touch is cached asynchronously, so reading it every 10 ms is cheap.
     */
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
"""

if anchor not in src:
    sys.exit(
        "ERROR: Could not locate LVGL input registration anchor"
    )

src = src.replace(anchor, replacement, 1)


# --------------------------------------------------------------------
# Start GT911 acquisition from the LVGL task after first frame and
# backlight initialization. This avoids I2C concurrency during the
# sensitive initial panel bring-up.
# --------------------------------------------------------------------

anchor = '''    ESP_LOGI(TAG, "LCD backlight enabled");
    ESP_LOGI(TAG, "DragonTouch LVGL hardware path active");

    TickType_t next_stack_report =
'''

replacement = '''    ESP_LOGI(TAG, "LCD backlight enabled");

    /*
     * Start physical touch acquisition only after initial display
     * bring-up is complete.
     */
    BaseType_t touch_result = xTaskCreate(
        gt911_poll_task,
        "gt911",
        4096,
        NULL,
        5,
        &s_touch_task
    );

    if (touch_result != pdPASS) {
        s_touch_task = NULL;

        ESP_LOGE(
            TAG,
            "failed to create GT911 acquisition task"
        );

    } else {
        ESP_LOGI(
            TAG,
            "GT911 acquisition task created"
        );
    }

    ESP_LOGI(TAG, "DragonTouch LVGL hardware path active");

    TickType_t next_stack_report =
'''

if anchor not in src:
    sys.exit(
        "ERROR: Could not locate backlight/LVGL-task anchor"
    )

src = src.replace(anchor, replacement, 1)


PATH.write_text(src)

print(f"Patched: {PATH}")
print()
print("Touch latency changes:")
print("  - GT911 I2C moved completely off LVGL task")
print("  - GT911 acquisition period: 5 ms")
print("  - cached touch state protected by critical section")
print("  - LVGL input consumption period: 10 ms")
print("  - LVGL read callback now performs zero I2C transactions")
