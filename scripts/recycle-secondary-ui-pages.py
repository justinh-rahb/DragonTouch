#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys
import time

ROOT = Path.cwd().resolve()
P = ROOT / "components/dt_ui/dt_ui.c"

if not P.exists():
    sys.exit(f"ERROR: missing {P}")

src = P.read_text()

if "DT_UI_RECYCLE_SECONDARY_PAGES" in src:
    print("Secondary-page recycling is already installed.")
    sys.exit(0)

required = [
    "DT_UI_LAZY_PAGE_BUILD",
    "static void ensure_page_built(dt_ui_page_t page)",
    "static void show_page(dt_ui_page_t selected)",
    "bool page_built[DT_PAGE_COUNT]",
    "if (s_ui.home_button != NULL)",
    "s_ui.jog_buttons",
    "s_ui.bed_buttons",
    "s_ui.fan_buttons",
]

for marker in required:
    if marker not in src:
        sys.exit(
            "ERROR: expected lazy/Stage-2 marker missing:\n"
            f"  {marker}\n"
            "Nothing modified."
        )

def find_function(text, signature):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"function missing: {signature}")

    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"opening brace missing: {signature}")

    depth = 0
    i = brace
    in_string = in_char = in_line = in_block = escape = False

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

def replace_function(text, signature, replacement):
    a, b = find_function(text, signature)
    return text[:a] + replacement + text[b:]

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    ROOT
    / "backups"
    / f"pre-secondary-page-recycle-{stamp}"
    / P.relative_to(ROOT)
)
backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(P, backup)
print(f"Backup: {backup}")

if '#include "esp_heap_caps.h"' not in src:
    anchor = '#include "esp_log.h"\n'
    if anchor not in src:
        sys.exit("ERROR: esp_log include anchor missing")
    src = src.replace(
        anchor,
        anchor + '#include "esp_heap_caps.h"\n',
        1
    )

old_decl = 'static void ensure_page_built(dt_ui_page_t page);\n'
new_decl = (
    'static void ensure_page_built(dt_ui_page_t page);\n'
    'static void recycle_secondary_pages(dt_ui_page_t keep);\n'
)

if old_decl not in src:
    sys.exit("ERROR: ensure_page_built forward declaration missing")

src = src.replace(old_decl, new_decl, 1)

new_show_page = r'''static void show_page(dt_ui_page_t selected)
{
    if (
        selected < DT_UI_PAGE_HOME ||
        selected > DT_UI_PAGE_SETTINGS
    ) {
        return;
    }

    /*
     * DT_UI_RECYCLE_SECONDARY_PAGES
     *
     * Home is permanently resident. Keep at most one secondary page's
     * content tree alive. This bounds LVGL's resident object/style
     * allocation instead of allowing every visited page to accumulate.
     */
    recycle_secondary_pages(selected);

    ensure_page_built(selected);

    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        const bool active =
            i == selected;

        if (active) {
            lv_obj_remove_flag(
                s_ui.pages[i],
                LV_OBJ_FLAG_HIDDEN
            );
        } else {
            lv_obj_add_flag(
                s_ui.pages[i],
                LV_OBJ_FLAG_HIDDEN
            );
        }

        lv_obj_set_style_text_color(
            s_ui.nav_icons[i],
            color(
                active
                    ? DT_COLOR_ACCENT
                    : DT_COLOR_MUTED
            ),
            0
        );

        lv_obj_set_style_bg_opa(
            s_ui.nav_buttons[i],
            active
                ? LV_OPA_20
                : LV_OPA_TRANSP,
            0
        );

        lv_obj_set_style_border_width(
            s_ui.nav_buttons[i],
            active ? 3 : 0,
            0
        );

        lv_obj_set_style_border_side(
            s_ui.nav_buttons[i],
            LV_BORDER_SIDE_LEFT,
            0
        );

        lv_obj_set_style_border_color(
            s_ui.nav_buttons[i],
            color(DT_COLOR_ACCENT),
            0
        );
    }
}'''

src = replace_function(
    src,
    "static void show_page(dt_ui_page_t selected)",
    new_show_page
)

pos = src.find("static void ensure_page_built(dt_ui_page_t page)\n{")
if pos < 0:
    sys.exit("ERROR: ensure_page_built definition missing")

helpers = r'''
static void log_ui_heap(
    const char *phase,
    dt_ui_page_t page
)
{
    const size_t internal_free =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    const size_t internal_largest =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    const size_t psram_free =
        heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    ESP_LOGI(
        TAG,
        "UI_HEAP %s page=%d internal=%u largest=%u psram=%u",
        phase,
        (int)page,
        (unsigned)internal_free,
        (unsigned)internal_largest,
        (unsigned)psram_free
    );
}


static void clear_recycled_page_refs(
    dt_ui_page_t page
)
{
    if (page != DT_UI_PAGE_CONTROL) {
        return;
    }

    s_ui.home_button = NULL;
    s_ui.heat_button = NULL;
    s_ui.extrude_button = NULL;
    s_ui.retract_button = NULL;

    s_ui.axes_text = NULL;
    s_ui.nozzle_control_text = NULL;
    s_ui.bed_control_text = NULL;
    s_ui.fan_control_text = NULL;

    for (size_t i = 0; i < 6; ++i) {
        s_ui.jog_buttons[i] = NULL;
    }

    for (size_t i = 0; i < 3; ++i) {
        s_ui.bed_buttons[i] = NULL;
        s_ui.fan_buttons[i] = NULL;
    }

    for (
        size_t i = 0;
        i < DT_CONTROL_TAB_COUNT;
        ++i
    ) {
        s_ui.control_tabs[i] = NULL;
        s_ui.control_panels[i] = NULL;
    }
}


static void recycle_secondary_pages(
    dt_ui_page_t keep
)
{
    for (
        int i = DT_UI_PAGE_CONTROL;
        i <= DT_UI_PAGE_SETTINGS;
        ++i
    ) {
        const dt_ui_page_t page =
            (dt_ui_page_t)i;

        if (
            page == keep ||
            !s_ui.page_built[page]
        ) {
            continue;
        }

        log_ui_heap(
            "before-clean",
            page
        );

        lv_obj_clean(
            s_ui.pages[page]
        );

        clear_recycled_page_refs(
            page
        );

        s_ui.page_built[page] =
            false;

        log_ui_heap(
            "after-clean",
            page
        );
    }
}


'''

src = src[:pos] + helpers + src[pos:]

old = '''    const int64_t started_us = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "lazy build page=%d start",
        (int)page
    );'''

new = '''    const int64_t started_us = esp_timer_get_time();

    log_ui_heap(
        "before-build",
        page
    );

    ESP_LOGI(
        TAG,
        "lazy build page=%d start",
        (int)page
    );'''

if old not in src:
    sys.exit("ERROR: lazy-build start anchor missing")

src = src.replace(old, new, 1)

old = '''    ESP_LOGI(
        TAG,
        "lazy build page=%d complete in %lld ms",
        (int)page,
        (long long)((esp_timer_get_time() - started_us) / 1000)
    );'''

new = '''    ESP_LOGI(
        TAG,
        "lazy build page=%d complete in %lld ms",
        (int)page,
        (long long)(
            (
                esp_timer_get_time() -
                started_us
            ) / 1000
        )
    );

    log_ui_heap(
        "after-build",
        page
    );'''

if old not in src:
    sys.exit("ERROR: lazy-build completion anchor missing")

src = src.replace(old, new, 1)

P.write_text(src)

print(f"Patched: {P}")
print()
print("Behavior:")
print("  - Home stays resident")
print("  - only one secondary page stays resident")
print("  - leaving a secondary page deletes its children")
print("  - Control runtime pointers are nulled after recycle")
print("  - internal heap/largest block/PSRAM are logged")
print()
print("Unchanged:")
print("  - display / RGB buffering")
print("  - GT911")
print("  - Stage 2 Moonraker runtime")
print("  - watchdog configuration")
