#!/usr/bin/env python3

from pathlib import Path
import re
import shutil
import sys
import time

BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
CMAKE = Path("components/dt_board/CMakeLists.txt")

for p in (BOARD, CMAKE):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

src = BOARD.read_text()

# ------------------------------------------------------------
# Refuse to touch the known-good RGB configuration.
# ------------------------------------------------------------

required = [
    ".num_fbs = 1,",
    ".bounce_buffer_size_px = (DT_LCD_H_RES * 10),",
    "port_cfg.task_affinity = 0;",
    ".buff_dma = true,",
    ".direct_mode = false,",
    ".bb_mode = true,",
    ".avoid_tearing = false,",
]

for marker in required:
    if marker not in src:
        sys.exit(
            f"ERROR: stable RGB marker missing:\n  {marker}\n"
            "Nothing modified."
        )


def find_function(text, signature):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"Function missing: {signature}")

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


# ------------------------------------------------------------
# Determine which legacy ESP-IDF I2C controller this board uses.
# ------------------------------------------------------------

m = re.search(
    r'i2c_driver_install\s*\(\s*([^,\n]+)',
    src
)

if not m:
    sys.exit("ERROR: could not determine existing I2C port")

i2c_port = m.group(1).strip()

print(f"Detected existing I2C bus: {i2c_port}")


# ------------------------------------------------------------
# Backup
# ------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"pre-official-gt911-{stamp}"

for p in (BOARD, CMAKE):
    dst = backup / p
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Backup: {backup}")


# ------------------------------------------------------------
# Required headers
# ------------------------------------------------------------

include_anchor = '#include "esp_lvgl_port.h"\n'

includes = (
    '#include "esp_lcd_panel_io.h"\n'
    '#include "esp_lcd_touch.h"\n'
    '#include "esp_lcd_touch_gt911.h"\n'
)

if '#include "esp_lcd_touch_gt911.h"' not in src:
    if include_anchor not in src:
        sys.exit("ERROR: esp_lvgl_port.h include not found")

    src = src.replace(
        include_anchor,
        include_anchor + includes,
        1
    )


# ------------------------------------------------------------
# Add official-driver handles.
# ------------------------------------------------------------

anchor = "static lv_indev_t *s_lv_touch;\n"

if anchor not in src:
    sys.exit("ERROR: s_lv_touch declaration not found")

if "s_gt911_handle" not in src:
    src = src.replace(
        anchor,
        anchor + '''
static esp_lcd_panel_io_handle_t s_gt911_io;
static esp_lcd_touch_handle_t s_gt911_handle;
''',
        1
    )


# ------------------------------------------------------------
# Replace complete init function.
#
# RGB settings below are copied verbatim from the working baseline.
# Touch is the ONLY architectural change.
# ------------------------------------------------------------

new_init = f'''esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)
{{
    if (display == NULL) {{
        return ESP_ERR_INVALID_ARG;
    }}

    ESP_LOGI(
        TAG,
        "Waveshare stable RGB + official GT911"
    );

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

    /*
     * Keep the already-validated CH422G reset/address procedure.
     * It selects GT911 address 0x5D and leaves GPIO4 as the INT input.
     */
    ESP_RETURN_ON_ERROR(
        gt911_reset_select_address(),
        TAG,
        "GT911 reset failed"
    );

    /*
     * Official Espressif GT911 I2C transport.
     *
     * ESP-IDF 5.3 supports wrapping the already-installed legacy I2C
     * controller with esp_lcd panel IO.
     */
    esp_lcd_panel_io_i2c_config_t tp_io_cfg =
        ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

    tp_io_cfg.dev_addr = 0x5D;
    tp_io_cfg.scl_speed_hz = 400000;

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(
            (esp_lcd_i2c_bus_handle_t){i2c_port},
            &tp_io_cfg,
            &s_gt911_io
        ),
        TAG,
        "GT911 panel IO creation failed"
    );

    esp_lcd_touch_io_gt911_config_t gt911_driver_cfg = {{
        .dev_addr = 0x5D,
    }};

    const esp_lcd_touch_config_t tp_cfg = {{
        .x_max = DT_LCD_H_RES,
        .y_max = DT_LCD_V_RES,

        /*
         * Reset is controlled through CH422G, not an ESP GPIO.
         */
        .rst_gpio_num = GPIO_NUM_NC,

        /*
         * Real GT911 interrupt line on this Waveshare board.
         */
        .int_gpio_num = GPIO_NUM_4,

        .levels = {{
            .reset = 0,
            .interrupt = 0,
        }},

        .flags = {{
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        }},

        .driver_data = &gt911_driver_cfg,
    }};

    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(
            s_gt911_io,
            &tp_cfg,
            &s_gt911_handle
        ),
        TAG,
        "official GT911 driver initialization failed"
    );

    /*
     * Stable display configuration. DO NOT CHANGE.
     */
    lvgl_port_cfg_t port_cfg =
        ESP_LVGL_PORT_INIT_CONFIG();

    port_cfg.task_priority = 4;
    port_cfg.task_stack = 16384;
    port_cfg.task_affinity = 0;
    port_cfg.task_max_sleep_ms = 20;
    port_cfg.timer_period_ms = 5;

    ESP_RETURN_ON_ERROR(
        lvgl_port_init(&port_cfg),
        TAG,
        "esp_lvgl_port initialization failed"
    );

    const lvgl_port_display_cfg_t display_cfg = {{
        .io_handle = NULL,
        .panel_handle = s_panel,

        .buffer_size =
            DT_LCD_H_RES * 20,

        .double_buffer = true,

        .hres = DT_LCD_H_RES,
        .vres = DT_LCD_V_RES,

        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,

        .rotation = {{
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        }},

        .flags = {{
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        }},
    }};

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {{
        .flags = {{
            .bb_mode = true,
            .avoid_tearing = false,
        }},
    }};

    s_lv_display =
        lvgl_port_add_disp_rgb(
            &display_cfg,
            &rgb_cfg
        );

    if (s_lv_display == NULL) {{
        return ESP_FAIL;
    }}

    lv_display_set_default(
        s_lv_display
    );

    /*
     * Hand the official GT911 driver directly to esp_lvgl_port.
     *
     * GPIO4 is now used as the real GT911 interrupt source.
     * esp_lvgl_port wakes its LVGL task from touch interrupts and invokes
     * esp_lcd_touch_read_data()/get_coordinates through the maintained
     * touch integration.
     */
    const lvgl_port_touch_cfg_t touch_cfg = {{
        .disp = s_lv_display,
        .handle = s_gt911_handle,
    }};

    s_lv_touch =
        lvgl_port_add_touch(
            &touch_cfg
        );

    if (s_lv_touch == NULL) {{
        return ESP_FAIL;
    }}

    /*
     * app_main creates DragonTouch while this recursive LVGL mutex is held.
     */
    if (!lvgl_port_lock(0)) {{
        return ESP_FAIL;
    }}

    ESP_LOGI(
        TAG,
        "stable RGB mode unchanged"
    );

    ESP_LOGI(
        TAG,
        "official GT911 interrupt input active on GPIO4"
    );

    *display = s_lv_display;

    return ESP_OK;
}}'''

src = replace_function(
    src,
    "esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)",
    new_init
)


# ------------------------------------------------------------
# Start function: no custom GT911 task anymore.
# ------------------------------------------------------------

new_start = r'''esp_err_t dt_board_waveshare_7_lvgl_start(void)
{
    if (s_lv_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * DragonTouch object construction is complete.
     */
    lvgl_port_unlock();

    vTaskDelay(
        pdMS_TO_TICKS(100)
    );

    ESP_RETURN_ON_ERROR(
        ch422g_set(
            DT_IOEXP_LCD_BACKLIGHT,
            true
        ),
        TAG,
        "LCD backlight enable failed"
    );

    ESP_LOGI(
        TAG,
        "LCD backlight enabled"
    );

    ESP_LOGI(
        TAG,
        "official GT911 + esp_lvgl_port touch path running"
    );

    return ESP_OK;
}'''

src = replace_function(
    src,
    "esp_err_t dt_board_waveshare_7_lvgl_start(void)",
    new_start
)

BOARD.write_text(src)


# ------------------------------------------------------------
# Ensure dt_board explicitly links the GT911 component.
# ------------------------------------------------------------

cmake = CMAKE.read_text()

if "esp_lcd_touch_gt911" not in cmake:
    cmake = cmake.replace(
        "REQUIRES",
        "REQUIRES esp_lcd_touch_gt911",
        1
    )

CMAKE.write_text(cmake)

print()
print("Official GT911 migration applied.")
print()
print("UNCHANGED:")
print("  - one RGB framebuffer")
print("  - 800x10 RGB bounce buffer")
print("  - internal DMA LVGL buffers")
print("  - RGB/LVGL task on CPU0")
print("  - PSRAM XIP/cache configuration")
print()
print("REMOVED FROM ACTIVE PATH:")
print("  - custom GT911 polling task")
print("  - custom touch cache")
print("  - custom edge queue")
print("  - custom release debounce")
print()
print("ACTIVE TOUCH PATH:")
print("  GPIO4 interrupt -> esp_lcd_touch_gt911 -> esp_lvgl_port -> LVGL")
