#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
CMAKE = Path("components/dt_board/CMakeLists.txt")
APP = Path("main/app_main.c")

for p in (BOARD, CMAKE, APP):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")


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

    raise RuntimeError(f"Closing brace not found: {signature}")


def replace_function(text, signature, replacement):
    start, end = find_function(text, signature)
    return text[:start] + replacement + text[end:]


# --------------------------------------------------------------------
# Safety backup of current mess
# --------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"pre-espressif-lvgl-port-{stamp}"

for p in (BOARD, CMAKE, APP):
    dst = backup / p
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Safety backup: {backup}")


# --------------------------------------------------------------------
# Restore the hardware implementation from before our internal-buffer
# experiments.
#
# We only want its proven GPIO / CH422G / RGB timing implementation.
# Its hand-written LVGL functions are replaced below.
# --------------------------------------------------------------------

candidates = sorted(
    Path("backups").glob(
        "source-pre-internal-lvgl-*/"
        "components/dt_board/dt_board_waveshare_7.c"
    )
)

if not candidates:
    sys.exit(
        "ERROR: source-pre-internal-lvgl board backup not found"
    )

hardware_source = candidates[-1]

print(f"Hardware baseline: {hardware_source}")

shutil.copy2(
    hardware_source,
    BOARD
)

src = BOARD.read_text()


# --------------------------------------------------------------------
# Include Espressif's maintained LVGL port.
# --------------------------------------------------------------------

if '#include "esp_lvgl_port.h"' not in src:
    anchor = '#include "lvgl.h"\n'

    if anchor not in src:
        sys.exit("ERROR: lvgl.h include not found")

    src = src.replace(
        anchor,
        anchor + '#include "esp_lvgl_port.h"\n',
        1
    )


# --------------------------------------------------------------------
# RGB driver MUST provide two framebuffers for avoid-tearing mode.
# --------------------------------------------------------------------

if ".num_fbs = 1," in src:
    src = src.replace(
        ".num_fbs = 1,",
        ".num_fbs = 2,",
        1
    )

if ".num_fbs = 2," not in src:
    sys.exit(
        "ERROR: RGB configuration does not contain num_fbs=2"
    )


# --------------------------------------------------------------------
# Replace our entire LVGL initialization path.
#
# NO:
#   custom flush callback
#   custom VSYNC semaphore
#   custom flush_wait
#   custom LVGL task
#   lv_refr_now()
#
# esp_lvgl_port owns all of that now.
# --------------------------------------------------------------------

new_init = r'''esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(
        TAG,
        "Waveshare hardware init + Espressif LVGL port"
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
     * Let Espressif own:
     *
     * - lv_init()
     * - LVGL tick
     * - LVGL service task
     * - display flush synchronization
     * - RGB VSYNC/bounce-buffer synchronization
     */
    lvgl_port_cfg_t port_cfg =
        ESP_LVGL_PORT_INIT_CONFIG();

    port_cfg.task_priority = 4;
    port_cfg.task_stack = 16384;
    port_cfg.task_affinity = 1;
    port_cfg.task_max_sleep_ms = 20;
    port_cfg.timer_period_ms = 5;

    ESP_RETURN_ON_ERROR(
        lvgl_port_init(&port_cfg),
        TAG,
        "esp_lvgl_port initialization failed"
    );

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = NULL,
        .panel_handle = s_panel,

        /*
         * avoid_tearing causes esp_lvgl_port to use the RGB driver's
         * actual full-screen framebuffers instead of allocating its
         * own unrelated buffers.
         */
        .buffer_size =
            DT_LCD_H_RES * DT_LCD_V_RES,

        .double_buffer = true,

        .hres = DT_LCD_H_RES,
        .vres = DT_LCD_V_RES,

        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,

        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },

        .flags = {
            .buff_dma = false,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = true,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            /*
             * Our RGB driver uses internal DMA bounce buffers.
             * IDF 5.3 uses the bounce-frame-finish event for proper
             * synchronization in this mode.
             */
            .bb_mode = true,

            /*
             * Critical: obtain the RGB driver's two real framebuffers
             * and synchronize presentation at frame boundaries.
             */
            .avoid_tearing = true,
        },
    };

    s_lv_display =
        lvgl_port_add_disp_rgb(
            &display_cfg,
            &rgb_cfg
        );

    if (s_lv_display == NULL) {
        return ESP_FAIL;
    }

    lv_display_set_default(
        s_lv_display
    );

    /*
     * app_main() creates its UI between init() and start().
     * Hold the Espressif LVGL recursive mutex across that interval.
     */
    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Espressif RGB/LVGL port registered"
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


new_start = r'''esp_err_t dt_board_waveshare_7_lvgl_start(void)
{
    if (s_lv_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * UI creation is complete. Release the port task.
     *
     * Importantly, there is NO synchronous lv_refr_now() here.
     * The maintained LVGL port performs normal refresh scheduling.
     */
    lvgl_port_unlock();

    /*
     * Give the first normal refresh a short opportunity to complete
     * before exposing the panel.
     */
    vTaskDelay(pdMS_TO_TICKS(100));

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
        "LCD backlight enabled; Espressif LVGL task owns display"
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

BOARD.write_text(src)


# --------------------------------------------------------------------
# Ensure dt_board links esp_lvgl_port.
# --------------------------------------------------------------------

cmake = CMAKE.read_text()

if "esp_lvgl_port" not in cmake:
    if "REQUIRES" not in cmake:
        sys.exit("ERROR: dt_board CMakeLists has no REQUIRES section")

    cmake = cmake.replace(
        "REQUIRES",
        "REQUIRES esp_lvgl_port",
        1
    )

CMAKE.write_text(cmake)


# --------------------------------------------------------------------
# DISPLAY-ONLY diagnostic.
#
# No GT911.
# No button events.
# No DragonTouch UI.
#
# The screen changes itself every 500 ms.
# --------------------------------------------------------------------

APP.write_text(r'''#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#include "lvgl.h"

#include "dt_board.h"

static const char *TAG = "rgb_diag";

static lv_obj_t *s_background;
static lv_obj_t *s_box;
static lv_obj_t *s_label;

static unsigned s_frame;

static const uint32_t colors[] = {
    0x202020,
    0x24364A,
    0x254430,
    0x503020,
    0x40284A,
    0x303030,
};


static void animation_timer(lv_timer_t *timer)
{
    (void)timer;

    ++s_frame;

    const unsigned index =
        s_frame %
        (sizeof(colors) / sizeof(colors[0]));

    /*
     * Large redraw: nearly the whole display.
     */
    lv_obj_set_style_bg_color(
        s_background,
        lv_color_hex(colors[index]),
        0
    );

    /*
     * Small redraw + movement.
     */
    const int32_t x =
        190 + ((s_frame * 73) % 500);

    const int32_t y =
        80 + ((s_frame * 47) % 300);

    lv_obj_set_pos(
        s_box,
        x,
        y
    );

    lv_label_set_text_fmt(
        s_label,
        "FRAME %u",
        s_frame
    );

    ESP_LOGI(
        TAG,
        "frame %u",
        s_frame
    );
}


static void create_ui(lv_display_t *display)
{
    lv_obj_t *screen =
        lv_display_get_screen_active(
            display
        );

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


    s_background =
        lv_obj_create(screen);

    lv_obj_remove_style_all(
        s_background
    );

    lv_obj_set_size(
        s_background,
        800,
        480
    );

    lv_obj_set_pos(
        s_background,
        0,
        0
    );

    lv_obj_set_style_bg_color(
        s_background,
        lv_color_hex(colors[0]),
        0
    );

    lv_obj_set_style_bg_opa(
        s_background,
        LV_OPA_COVER,
        0
    );


    s_box =
        lv_obj_create(screen);

    lv_obj_remove_style_all(
        s_box
    );

    lv_obj_set_size(
        s_box,
        80,
        80
    );

    lv_obj_set_style_bg_color(
        s_box,
        lv_color_hex(0xE02020),
        0
    );

    lv_obj_set_style_bg_opa(
        s_box,
        LV_OPA_COVER,
        0
    );


    s_label =
        lv_label_create(screen);

    lv_label_set_text(
        s_label,
        "FRAME 0"
    );

    lv_obj_set_style_text_color(
        s_label,
        lv_color_hex(0xFFFFFF),
        0
    );

    lv_obj_align(
        s_label,
        LV_ALIGN_TOP_MID,
        0,
        20
    );


    lv_timer_create(
        animation_timer,
        500,
        NULL
    );

    ESP_LOGI(
        TAG,
        "automatic RGB redraw diagnostic created"
    );
}


void app_main(void)
{
    lv_display_t *display = NULL;

    ESP_LOGI(
        TAG,
        "starting official-port RGB diagnostic"
    );

    ESP_ERROR_CHECK(
        dt_board_lvgl_init(
            &display
        )
    );

    create_ui(
        display
    );

    ESP_ERROR_CHECK(
        dt_board_lvgl_start()
    );

    ESP_LOGI(
        TAG,
        "diagnostic running"
    );
}
''')

print()
print("Migration complete.")
print()
print("This build intentionally contains:")
print("  - Espressif esp_lvgl_port")
print("  - two RGB PSRAM framebuffers")
print("  - direct mode")
print("  - bounce-buffer-aware anti-tearing")
print("  - automatic redraw test")
print()
print("It intentionally contains NO touch input.")
