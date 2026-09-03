#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

PATH = Path("components/dt_board/dt_board_waveshare_7.c")

if not PATH.exists():
    sys.exit(f"ERROR: {PATH} not found")

src = PATH.read_text()

if 'LVGL service task started (stack=24576 bytes)' in src:
    print("LVGL stack fix already applied.")
    sys.exit(0)


def find_function(text: str, signature: str):
    """
    Locate a C function by signature and return the byte range covering
    the complete function body. Handles braces inside comments and strings.
    """
    start = text.find(signature)

    if start < 0:
        raise RuntimeError(f"Could not find function signature: {signature}")

    brace = text.find("{", start)

    if brace < 0:
        raise RuntimeError(f"Could not find opening brace for: {signature}")

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

    raise RuntimeError(f"Could not find closing brace for: {signature}")


def replace_function(text: str, signature: str, replacement: str):
    start, end = find_function(text, signature)
    return text[:start] + replacement + text[end:]


# ---------------------------------------------------------------------
# Backup
# ---------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")

backup = (
    Path("backups")
    / f"source-pre-stack-fix-{stamp}"
    / PATH
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(PATH, backup)

print(f"Backup: {backup}")


# ---------------------------------------------------------------------
# Dedicated LVGL task
# ---------------------------------------------------------------------

new_lvgl_task = r'''static void lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "LVGL service task started (stack=24576 bytes)"
    );

    /*
     * First rendering is deliberately done here rather than from app_main().
     * Rendering the complete DragonTouch UI requires considerably more stack
     * than ESP-IDF's default main task provides.
     */
    ESP_LOGI(TAG, "rendering first DragonTouch frame");

    lv_refr_now(s_lv_display);

    ESP_LOGI(
        TAG,
        "first render complete; LVGL stack free=%u bytes",
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );

    /*
     * Keep the LCD dark until a valid frame has been rendered.
     */
    esp_err_t err = ch422g_set(
        DT_IOEXP_LCD_BACKLIGHT,
        true
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "backlight enable failed: %s",
            esp_err_to_name(err)
        );

        s_lvgl_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "LCD backlight enabled");
    ESP_LOGI(TAG, "DragonTouch LVGL hardware path active");

    TickType_t next_stack_report =
        xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    while (true) {
        uint32_t wait_ms = lv_timer_handler();

        /*
         * Keep touch/UI response quick while still yielding CPU time.
         */
        if (wait_ms < 5) {
            wait_ms = 5;
        }

        if (wait_ms > 20) {
            wait_ms = 20;
        }

        vTaskDelay(pdMS_TO_TICKS(wait_ms));

        if (xTaskGetTickCount() >= next_stack_report) {
            ESP_LOGI(
                TAG,
                "LVGL stack free=%u bytes",
                (unsigned)uxTaskGetStackHighWaterMark(NULL)
            );

            next_stack_report =
                xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        }
    }
}'''

try:
    src = replace_function(
        src,
        "static void lvgl_task(void *arg)",
        new_lvgl_task
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ---------------------------------------------------------------------
# Start function
# ---------------------------------------------------------------------

new_start = r'''esp_err_t dt_board_waveshare_7_lvgl_start(void)
{
    if (s_lv_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lvgl_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * ESP-IDF's xTaskCreate() stack size is specified in bytes.
     *
     * Start generously during hardware bring-up. The LVGL task reports
     * its minimum remaining stack so we can tune this later.
     */
    BaseType_t result = xTaskCreate(
        lvgl_task,
        "dt_lvgl",
        24576,
        NULL,
        5,
        &s_lvgl_task
    );

    if (result != pdPASS) {
        s_lvgl_task = NULL;

        ESP_LOGE(
            TAG,
            "failed to create dedicated LVGL task"
        );

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "dedicated LVGL task created"
    );

    return ESP_OK;
}'''

try:
    src = replace_function(
        src,
        "esp_err_t dt_board_waveshare_7_lvgl_start(void)",
        new_start
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


PATH.write_text(src)

print(f"Patched: {PATH}")
print()
print("Changes applied:")
print("  - first LVGL render moved off app_main")
print("  - backlight enable moved onto LVGL task")
print("  - LVGL task stack set to 24576 bytes")
print("  - stack high-water logging enabled")
