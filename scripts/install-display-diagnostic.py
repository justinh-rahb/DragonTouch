#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
APP   = Path("main/app_main.c")
SDK   = Path("sdkconfig")
DEFS  = Path("sdkconfig.defaults")

stamp = time.strftime("%Y%m%d-%H%M%S")
safety = Path("backups") / f"pre-display-diagnostic-{stamp}"

for p in (BOARD, APP, SDK, DEFS):
    if p.exists():
        dst = safety / p
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)

print(f"Safety backup: {safety}")


# ------------------------------------------------------------------
# Restore the last version before the internal-partial-buffer
# experiment. This is the visually correct double-FB DIRECT path.
# ------------------------------------------------------------------

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

restore = candidates[-1]

print(f"Restoring display driver: {restore}")
shutil.copy2(restore, BOARD)


# ------------------------------------------------------------------
# Disable the failed LVGL multi-renderer experiment.
# Keep -O2 and the other performance options.
# ------------------------------------------------------------------

def fix_config(path):
    if not path.exists():
        return

    lines = path.read_text().splitlines()
    out = []

    prefixes = (
        "CONFIG_LV_OS_NONE=",
        "CONFIG_LV_OS_FREERTOS=",
        "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=",
        "CONFIG_LV_DRAW_THREAD_STACK_SIZE=",
        "CONFIG_LV_DRAW_THREAD_PRIO=",
        "CONFIG_LV_USE_FREERTOS_TASK_NOTIFY=",
    )

    comments = (
        "# CONFIG_LV_OS_NONE is not set",
        "# CONFIG_LV_OS_FREERTOS is not set",
        "# CONFIG_LV_USE_FREERTOS_TASK_NOTIFY is not set",
    )

    for line in lines:
        if line.startswith(prefixes):
            continue

        if line in comments:
            continue

        out.append(line)

    out += [
        "CONFIG_LV_OS_NONE=y",
        "# CONFIG_LV_OS_FREERTOS is not set",
        "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1",
    ]

    path.write_text("\n".join(out) + "\n")


fix_config(SDK)
fix_config(DEFS)


# ------------------------------------------------------------------
# Replace DragonTouch UI temporarily with a deliberately trivial UI.
#
# This gives us TWO independent tests:
#
# PRESSED:
#   changes only a tiny status label
#
# CLICKED:
#   redraws essentially the whole 630x480 content panel
#
# Therefore:
#
# small update good / large update bad = display/render path problem
# both good                         = DragonTouch UI problem
# ------------------------------------------------------------------

APP.write_text(r'''#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include "dt_board.h"

static const char *TAG = "dt_diag";

static lv_obj_t *s_panel;
static lv_obj_t *s_page_label;
static lv_obj_t *s_press_label;

static uint32_t s_press_count;

static const uint32_t s_page_colors[] = {
    0x202020,
    0x243044,
    0x243A30,
    0x403024,
    0x38243E,
    0x303030,
};

static void nav_event(lv_event_t *event)
{
    const lv_event_code_t code =
        lv_event_get_code(event);

    const unsigned page =
        (unsigned)(uintptr_t)
        lv_event_get_user_data(event);

    const int64_t now_ms =
        esp_timer_get_time() / 1000;

    if (code == LV_EVENT_PRESSED) {
        ++s_press_count;

        lv_label_set_text_fmt(
            s_press_label,
            "PRESS %u   count=%lu",
            page + 1,
            (unsigned long)s_press_count
        );

        ESP_LOGI(
            TAG,
            "PRESSED page=%u t=%lld ms",
            page + 1,
            (long long)now_ms
        );

        return;
    }

    if (code == LV_EVENT_CLICKED) {
        /*
         * Deliberately invalidate most of the screen.
         *
         * If this simple operation shakes/corrupts, the board/display
         * integration is wrong. DragonTouch isn't involved.
         */
        lv_obj_set_style_bg_color(
            s_panel,
            lv_color_hex(s_page_colors[page]),
            0
        );

        lv_label_set_text_fmt(
            s_page_label,
            "PAGE %u",
            page + 1
        );

        ESP_LOGI(
            TAG,
            "CLICKED page=%u t=%lld ms",
            page + 1,
            (long long)now_ms
        );
    }
}


static void create_diagnostic_ui(lv_display_t *display)
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
     * Left navigation rail
     */
    lv_obj_t *rail =
        lv_obj_create(screen);

    lv_obj_remove_style_all(rail);

    lv_obj_set_size(
        rail,
        170,
        480
    );

    lv_obj_align(
        rail,
        LV_ALIGN_LEFT_MID,
        0,
        0
    );

    lv_obj_set_style_bg_color(
        rail,
        lv_color_hex(0x181818),
        0
    );

    lv_obj_set_style_bg_opa(
        rail,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_pad_all(
        rail,
        12,
        0
    );

    lv_obj_set_style_pad_row(
        rail,
        8,
        0
    );

    lv_obj_set_layout(
        rail,
        LV_LAYOUT_FLEX
    );

    lv_obj_set_flex_flow(
        rail,
        LV_FLEX_FLOW_COLUMN
    );


    for (unsigned i = 0; i < 6; ++i) {
        lv_obj_t *button =
            lv_button_create(rail);

        lv_obj_set_size(
            button,
            146,
            58
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

        lv_obj_set_style_bg_color(
            button,
            lv_color_hex(0x303030),
            0
        );

        lv_obj_set_style_bg_opa(
            button,
            LV_OPA_COVER,
            0
        );

        lv_obj_add_event_cb(
            button,
            nav_event,
            LV_EVENT_PRESSED,
            (void *)(uintptr_t)i
        );

        lv_obj_add_event_cb(
            button,
            nav_event,
            LV_EVENT_CLICKED,
            (void *)(uintptr_t)i
        );

        lv_obj_t *label =
            lv_label_create(button);

        lv_label_set_text_fmt(
            label,
            "TEST %u",
            i + 1
        );

        lv_obj_center(label);
    }


    /*
     * Large redraw target
     */
    s_panel =
        lv_obj_create(screen);

    lv_obj_remove_style_all(s_panel);

    lv_obj_set_size(
        s_panel,
        630,
        480
    );

    lv_obj_align(
        s_panel,
        LV_ALIGN_RIGHT_MID,
        0,
        0
    );

    lv_obj_set_style_bg_color(
        s_panel,
        lv_color_hex(s_page_colors[0]),
        0
    );

    lv_obj_set_style_bg_opa(
        s_panel,
        LV_OPA_COVER,
        0
    );


    s_page_label =
        lv_label_create(s_panel);

    lv_label_set_text(
        s_page_label,
        "PAGE 1"
    );

    lv_obj_set_style_text_font(
        s_page_label,
        &lv_font_montserrat_20,
        0
    );

    lv_obj_center(s_page_label);


    s_press_label =
        lv_label_create(s_panel);

    lv_label_set_text(
        s_press_label,
        "Touch a TEST button"
    );

    lv_obj_align(
        s_press_label,
        LV_ALIGN_BOTTOM_MID,
        0,
        -30
    );

    ESP_LOGI(
        TAG,
        "minimal display/touch diagnostic created"
    );
}


void app_main(void)
{
    ESP_LOGI(
        TAG,
        "starting Waveshare display diagnostic"
    );

    lv_display_t *display = NULL;

    ESP_ERROR_CHECK(
        dt_board_lvgl_init(&display)
    );

    create_diagnostic_ui(display);

    ESP_ERROR_CHECK(
        dt_board_lvgl_start()
    );

    ESP_LOGI(
        TAG,
        "diagnostic running"
    );
}
''')

print(f"Diagnostic app installed: {APP}")
print()
print("Test meaning:")
print("  PRESSED = tiny label redraw")
print("  CLICKED = large 630x480 redraw")
print()
print("Do not commit this diagnostic app.")
