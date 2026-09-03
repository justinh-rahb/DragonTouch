#!/usr/bin/env python3

from pathlib import Path
import re
import shutil
import sys
import time

BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
APP = Path("main/app_main.c")
SDK = Path("sdkconfig")
DEFAULTS = Path("sdkconfig.defaults")

for p in (BOARD, APP, SDK, DEFAULTS):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"pre-waveshare-stable-rgb-{stamp}"

for p in (BOARD, APP, SDK, DEFAULTS):
    dst = backup / p
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Backup: {backup}")


# ============================================================
# RGB PANEL
# ============================================================

src = BOARD.read_text()

src, n = re.subn(
    r'\.num_fbs\s*=\s*\d+\s*,',
    '.num_fbs = 1,',
    src,
    count=1
)

if n != 1:
    sys.exit("ERROR: num_fbs not found")

src, n = re.subn(
    r'\.bounce_buffer_size_px\s*=\s*[^,]+,',
    '.bounce_buffer_size_px = (DT_LCD_H_RES * 10),',
    src,
    count=1
)

if n != 1:
    sys.exit("ERROR: bounce_buffer_size_px not found")


# ============================================================
# ESP LVGL PORT
# ============================================================

replacements = {
    "port_cfg.task_affinity = 1;":
        "port_cfg.task_affinity = 0;",

    ".buffer_size =\n            DT_LCD_H_RES * DT_LCD_V_RES,":
        ".buffer_size =\n            DT_LCD_H_RES * 20,",

    ".double_buffer = true,":
        ".double_buffer = true,",

    ".buff_dma = false,":
        ".buff_dma = true,",

    ".direct_mode = true,":
        ".direct_mode = false,",

    ".bb_mode = false,":
        ".bb_mode = true,",

    ".avoid_tearing = true,":
        ".avoid_tearing = false,",
}

for old, new in replacements.items():
    if old in src:
        src = src.replace(old, new, 1)
    elif old != ".double_buffer = true,":
        print(f"WARNING: marker not found: {old!r}")


# Replace old mode log if present
src = re.sub(
    r'RGB mode: 2 FB, DIRECT, NO bounce buffer',
    'RGB mode: 1 FB + 10-line bounce buffer',
    src
)

# Add unmistakable log
marker = "Waveshare stable RGB mode active"

if marker not in src:
    anchor = '''    ESP_LOGI(
        TAG,
        "Espressif RGB/LVGL port registered"
    );
'''

    replacement = '''    ESP_LOGI(
        TAG,
        "Espressif RGB/LVGL port registered"
    );

    ESP_LOGI(
        TAG,
        "Waveshare stable RGB mode active: "
        "1 FB + bounce, internal LVGL buffers, CPU0"
    );
'''

    if anchor not in src:
        sys.exit("ERROR: LVGL registration log not found")

    src = src.replace(anchor, replacement, 1)

BOARD.write_text(src)


# ============================================================
# SDKCONFIG
# ============================================================

wanted = {
    "CONFIG_COMPILER_OPTIMIZATION_PERF":
        "CONFIG_COMPILER_OPTIMIZATION_PERF=y",

    "CONFIG_SPIRAM_XIP_FROM_PSRAM":
        "CONFIG_SPIRAM_XIP_FROM_PSRAM=y",

    "CONFIG_SPIRAM_FETCH_INSTRUCTIONS":
        "CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y",

    "CONFIG_SPIRAM_RODATA":
        "CONFIG_SPIRAM_RODATA=y",

    "CONFIG_ESP32S3_DATA_CACHE_LINE_16B":
        "# CONFIG_ESP32S3_DATA_CACHE_LINE_16B is not set",

    "CONFIG_ESP32S3_DATA_CACHE_LINE_32B":
        "# CONFIG_ESP32S3_DATA_CACHE_LINE_32B is not set",

    "CONFIG_ESP32S3_DATA_CACHE_LINE_64B":
        "CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y",
}


def update_config(path):
    lines = path.read_text().splitlines()
    out = []
    handled = set()

    for line in lines:
        matched = False

        for key, value in wanted.items():
            if (
                line.startswith(key + "=")
                or line == f"# {key} is not set"
            ):
                if key not in handled:
                    out.append(value)
                    handled.add(key)

                matched = True
                break

        if not matched:
            out.append(line)

    for key, value in wanted.items():
        if key not in handled:
            out.append(value)

    path.write_text("\n".join(out) + "\n")


update_config(SDK)
update_config(DEFAULTS)


# ============================================================
# DIAGNOSTIC UI
#
# IMPORTANT:
# no moving objects and no full-screen color changes.
#
# Static reference marks stay on screen. Only a tiny center box
# changes once per second. If the whole image moves, that is
# genuine RGB drift.
# ============================================================

APP.write_text(r'''#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "dt_board.h"

static const char *TAG = "drift_diag";

static lv_obj_t *s_status;
static lv_obj_t *s_counter;
static unsigned s_frame;


static void timer_cb(lv_timer_t *timer)
{
    (void)timer;

    ++s_frame;

    lv_obj_set_style_bg_color(
        s_status,
        lv_color_hex(
            (s_frame & 1) ? 0x205020 : 0x502020
        ),
        0
    );

    lv_label_set_text_fmt(
        s_counter,
        "UPDATE %u",
        s_frame
    );

    ESP_LOGI(
        TAG,
        "small update %u",
        s_frame
    );
}


static lv_obj_t *make_marker(
    lv_obj_t *parent,
    const char *text,
    lv_align_t align
)
{
    lv_obj_t *box = lv_obj_create(parent);

    lv_obj_set_size(box, 150, 70);
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
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(
        box,
        lv_color_hex(0xFFFFFF),
        0
    );
    lv_obj_set_style_radius(box, 0, 0);

    lv_obj_align(box, align, 10, 10);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, text);
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
     * Four static reference points.
     * These NEVER move or redraw after startup.
     */
    make_marker(screen, "TOP LEFT", LV_ALIGN_TOP_LEFT);
    make_marker(screen, "TOP RIGHT", LV_ALIGN_TOP_RIGHT);
    make_marker(screen, "BOTTOM LEFT", LV_ALIGN_BOTTOM_LEFT);
    make_marker(screen, "BOTTOM RIGHT", LV_ALIGN_BOTTOM_RIGHT);


    s_status = lv_obj_create(screen);

    lv_obj_set_size(
        s_status,
        220,
        100
    );

    lv_obj_set_style_radius(
        s_status,
        0,
        0
    );

    lv_obj_set_style_border_width(
        s_status,
        4,
        0
    );

    lv_obj_set_style_border_color(
        s_status,
        lv_color_hex(0xFFFFFF),
        0
    );

    lv_obj_set_style_bg_color(
        s_status,
        lv_color_hex(0x502020),
        0
    );

    lv_obj_set_style_bg_opa(
        s_status,
        LV_OPA_COVER,
        0
    );

    lv_obj_center(s_status);


    s_counter = lv_label_create(s_status);

    lv_label_set_text(
        s_counter,
        "UPDATE 0"
    );

    lv_obj_center(s_counter);


    lv_timer_create(
        timer_cb,
        1000,
        NULL
    );

    ESP_LOGI(
        TAG,
        "static drift diagnostic created"
    );
}


void app_main(void)
{
    lv_display_t *display = NULL;

    ESP_LOGI(
        TAG,
        "starting RGB drift diagnostic"
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

print()
print("Configured vendor-style stable RGB baseline:")
print("  RGB framebuffers : 1")
print("  bounce buffer    : 800 x 10 pixels")
print("  LVGL buffers     : 2 x 20 rows, internal DMA SRAM")
print("  LVGL task core   : CPU0")
print("  PSRAM XIP        : enabled")
print("  D-cache line     : 64 bytes")
print("  pixel clock      : unchanged")
print()
print("Diagnostic now has NO moving objects.")
