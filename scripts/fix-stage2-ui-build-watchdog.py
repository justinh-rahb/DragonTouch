#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

P = Path("components/dt_ui/dt_ui.c")

if not P.exists():
    sys.exit(f"ERROR: missing {P}")

src = P.read_text()

if "UI_BUILD_WATCHDOG_FIX" in src:
    print("UI construction watchdog fix already installed.")
    sys.exit(0)


def find_function(text, signature):
    start = text.find(signature)

    if start < 0:
        raise RuntimeError(
            f"function missing: {signature}"
        )

    brace = text.find("{", start)

    if brace < 0:
        raise RuntimeError(
            f"opening brace missing: {signature}"
        )

    depth = 0
    i = brace

    in_string = False
    in_char = False
    in_line = False
    in_block = False
    escape = False

    while i < len(text):
        c = text[i]
        n = (
            text[i + 1]
            if i + 1 < len(text)
            else ""
        )

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

    raise RuntimeError(
        f"closing brace missing: {signature}"
    )


def replace_function(
    text,
    signature,
    replacement
):
    a, b = find_function(
        text,
        signature
    )

    return (
        text[:a] +
        replacement +
        text[b:]
    )


# ------------------------------------------------------------
# Safety checks
# ------------------------------------------------------------

required = [
    "static void create_pages(",
    "static void create_control_page(",
    "DT_UI_PAGE_HOME",
    "DT_UI_PAGE_CONTROL",
    "DT_UI_PAGE_SETTINGS",
]

for marker in required:
    if marker not in src:
        sys.exit(
            "ERROR: expected Stage 2 UI marker "
            f"missing: {marker}"
        )


# ------------------------------------------------------------
# Backup
# ------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")

backup = (
    Path("backups")
    / f"pre-ui-build-watchdog-fix-{stamp}"
    / P
)

backup.parent.mkdir(
    parents=True,
    exist_ok=True
)

shutil.copy2(
    P,
    backup
)

print(f"Backup: {backup}")


# ------------------------------------------------------------
# FreeRTOS includes only for actual ESP build.
#
# Desktop host preview stays buildable.
# ------------------------------------------------------------

include_anchor = '#include "esp_log.h"\n'

includes = '''
#ifndef DT_UI_HOST_PREVIEW
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif
'''

if (
    "freertos/task.h" not in src
):
    if include_anchor not in src:
        sys.exit(
            "ERROR: esp_log include anchor missing"
        )

    src = src.replace(
        include_anchor,
        include_anchor + includes,
        1
    )


# ------------------------------------------------------------
# Scheduler yield helper.
#
# IMPORTANT:
# vTaskDelay(1) means one scheduler tick.
# At the current 100 Hz FreeRTOS rate that's ~10 ms.
#
# Unlike pdMS_TO_TICKS(1), it cannot collapse to zero.
# ------------------------------------------------------------

global_anchor = (
    'static const char *TAG = "dt_ui";'
)

helper = r'''

/*
 * UI_BUILD_WATCHDOG_FIX
 *
 * During startup app_main owns the LVGL lock while the full
 * object tree is constructed. Stage 2 contains enough objects
 * that uninterrupted construction can starve IDLE0 for longer
 * than the task watchdog period.
 *
 * Yielding one scheduler tick between major page builds lets
 * IDLE0 service the watchdog. The LVGL task cannot alter this
 * tree because app_main still owns the LVGL mutex.
 */
static void ui_build_yield(void)
{
#ifndef DT_UI_HOST_PREVIEW
    vTaskDelay(1);
#endif
}
'''

if "UI_BUILD_WATCHDOG_FIX" not in src:
    pos = src.find(global_anchor)

    if pos < 0:
        sys.exit(
            "ERROR: TAG declaration missing"
        )

    insert = (
        pos +
        len(global_anchor)
    )

    src = (
        src[:insert] +
        helper +
        src[insert:]
    )


# ------------------------------------------------------------
# Replace create_pages().
#
# Two changes:
#
# 1. Only Home is visible while children are constructed.
#    Control/Files/etc. are HIDDEN before their child objects
#    are added. This avoids pointless layout/redraw processing
#    for five pages that aren't currently displayed.
#
# 2. Yield after each major page.
# ------------------------------------------------------------

new_create_pages = r'''static void create_pages(lv_obj_t *content)
{
    /*
     * Create the containers first.
     *
     * Only Home should participate in layout while the rest of
     * the UI tree is being constructed.
     */
    for (
        int i = 0;
        i < DT_PAGE_COUNT;
        ++i
    ) {
        s_ui.pages[i] =
            make_page(content);

        if (
            i != DT_UI_PAGE_HOME
        ) {
            lv_obj_add_flag(
                s_ui.pages[i],
                LV_OBJ_FLAG_HIDDEN
            );
        }
    }

    ESP_LOGI(
        TAG,
        "UI build: Home"
    );

    create_home_page(
        s_ui.pages[
            DT_UI_PAGE_HOME
        ]
    );

    ui_build_yield();


    ESP_LOGI(
        TAG,
        "UI build: Control"
    );

    create_control_page(
        s_ui.pages[
            DT_UI_PAGE_CONTROL
        ]
    );

    ui_build_yield();


    ESP_LOGI(
        TAG,
        "UI build: Files"
    );

    create_files_page(
        s_ui.pages[
            DT_UI_PAGE_FILES
        ]
    );

    ui_build_yield();


    ESP_LOGI(
        TAG,
        "UI build: Filament"
    );

    static const char *filament_cards[] = {
        "ACTIVE TOOL",
        "MATERIAL SLOTS",
        "LOAD / UNLOAD"
    };

    create_stub_page(
        s_ui.pages[
            DT_UI_PAGE_FILAMENT
        ],
        "Filament",
        "Tool and material controls adapt "
        "to the selected printer's capabilities.",
        filament_cards,
        3
    );

    ui_build_yield();


    ESP_LOGI(
        TAG,
        "UI build: Devices"
    );

    static const char *device_cards[] = {
        "SELECTED PRINTER",
        "DISCOVERED DEVICES",
        "DRAGON GROUP"
    };

    create_stub_page(
        s_ui.pages[
            DT_UI_PAGE_DEVICES
        ],
        "Devices",
        "Discover and explicitly pair "
        "same-LAN printers and Dragon-family siblings.",
        device_cards,
        3
    );

    ui_build_yield();


    ESP_LOGI(
        TAG,
        "UI build: Settings"
    );

    static const char *settings_cards[] = {
        "WI-FI",
        "DISPLAY",
        "UPDATE & RECOVERY",
        "ABOUT"
    };

    create_stub_page(
        s_ui.pages[
            DT_UI_PAGE_SETTINGS
        ],
        "Settings",
        "Device-local preferences, provisioning, "
        "diagnostics, and recovery.",
        settings_cards,
        4
    );

    ui_build_yield();

    ESP_LOGI(
        TAG,
        "UI build: pages complete"
    );
}'''

try:
    src = replace_function(
        src,
        "static void create_pages(lv_obj_t *content)",
        new_create_pages
    )

except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


P.write_text(src)

print(f"Patched: {P}")
print()
print("Changes:")
print("  - Home is the only visible page during startup")
print("  - all other pages are constructed hidden")
print("  - one scheduler tick yielded after each page")
print("  - watchdog timeout itself is UNCHANGED")
print()
print("Not changed:")
print("  - display configuration")
print("  - LVGL buffers")
print("  - GT911")
print("  - Stage 2 runtime")
print("  - Moonraker commands")
