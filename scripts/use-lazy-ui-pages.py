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

if "DT_UI_LAZY_PAGE_BUILD" in src:
    print("Lazy page construction is already installed.")
    sys.exit(0)

required = [
    "static void create_home_page(",
    "static void create_control_page(",
    "static void create_files_page(",
    "static void create_stub_page(",
    "static void create_pages(",
    "static void show_page(",
    "if (s_ui.home_button != NULL)",
    "if (s_ui.jog_buttons[i] != NULL)",
    "if (s_ui.heat_button != NULL)",
    "if (s_ui.extrude_button != NULL)",
]

for marker in required:
    if marker not in src:
        sys.exit(
            "ERROR: expected Stage 2 marker missing:\n"
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
backup = ROOT / "backups" / f"pre-lazy-ui-pages-{stamp}" / P.relative_to(ROOT)
backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(P, backup)
print(f"Backup: {backup}")

state_anchor = "    bool ready;\n} dt_ui_state_t;"
if state_anchor not in src:
    sys.exit("ERROR: dt_ui_state_t ready anchor missing")

src = src.replace(
    state_anchor,
    '''    /*
     * DT_UI_LAZY_PAGE_BUILD
     *
     * Page containers always exist, but only Home is populated during boot.
     * Other pages are populated on first visit.
     */
    bool page_built[DT_PAGE_COUNT];

    bool ready;
} dt_ui_state_t;''',
    1
)

if '#include "esp_timer.h"' not in src:
    anchor = '#include "esp_log.h"\n'
    if anchor not in src:
        sys.exit("ERROR: esp_log include anchor missing")
    src = src.replace(anchor, anchor + '#include "esp_timer.h"\n', 1)

show_sig = "static void show_page(dt_ui_page_t selected)"
if "static void ensure_page_built(dt_ui_page_t page);" not in src:
    pos = src.find(show_sig)
    if pos < 0:
        sys.exit("ERROR: show_page() not found")
    src = src[:pos] + "static void ensure_page_built(dt_ui_page_t page);\n\n" + src[pos:]

new_show_page = '''static void show_page(dt_ui_page_t selected)
{
    if (
        selected < DT_UI_PAGE_HOME ||
        selected > DT_UI_PAGE_SETTINGS
    ) {
        return;
    }

    ensure_page_built(selected);

    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        const bool active = i == selected;

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
            color(active ? DT_COLOR_ACCENT : DT_COLOR_MUTED),
            0
        );

        lv_obj_set_style_bg_opa(
            s_ui.nav_buttons[i],
            active ? LV_OPA_20 : LV_OPA_TRANSP,
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

src = replace_function(src, show_sig, new_show_page)

create_pages_pos = src.find("static void create_pages(lv_obj_t *content)")
if create_pages_pos < 0:
    sys.exit("ERROR: create_pages() not found")

lazy_builder = '''
static void ensure_page_built(dt_ui_page_t page)
{
    if (
        page < DT_UI_PAGE_HOME ||
        page > DT_UI_PAGE_SETTINGS
    ) {
        return;
    }

    if (s_ui.page_built[page]) {
        return;
    }

    const int64_t started_us = esp_timer_get_time();

    ESP_LOGI(
        TAG,
        "lazy build page=%d start",
        (int)page
    );

    switch (page) {
    case DT_UI_PAGE_HOME:
        create_home_page(
            s_ui.pages[DT_UI_PAGE_HOME]
        );
        break;

    case DT_UI_PAGE_CONTROL:
        create_control_page(
            s_ui.pages[DT_UI_PAGE_CONTROL]
        );
        break;

    case DT_UI_PAGE_FILES:
        create_files_page(
            s_ui.pages[DT_UI_PAGE_FILES]
        );
        break;

    case DT_UI_PAGE_FILAMENT: {
        static const char *cards[] = {
            "ACTIVE TOOL",
            "MATERIAL SLOTS",
            "LOAD / UNLOAD"
        };

        create_stub_page(
            s_ui.pages[DT_UI_PAGE_FILAMENT],
            "Filament",
            "Tool and material controls adapt "
            "to the selected printer's capabilities.",
            cards,
            3
        );
        break;
    }

    case DT_UI_PAGE_DEVICES: {
        static const char *cards[] = {
            "SELECTED PRINTER",
            "DISCOVERED DEVICES",
            "DRAGON GROUP"
        };

        create_stub_page(
            s_ui.pages[DT_UI_PAGE_DEVICES],
            "Devices",
            "Discover and explicitly pair "
            "same-LAN printers and Dragon-family siblings.",
            cards,
            3
        );
        break;
    }

    case DT_UI_PAGE_SETTINGS: {
        static const char *cards[] = {
            "WI-FI",
            "DISPLAY",
            "UPDATE & RECOVERY",
            "ABOUT"
        };

        create_stub_page(
            s_ui.pages[DT_UI_PAGE_SETTINGS],
            "Settings",
            "Device-local preferences, provisioning, "
            "diagnostics, and recovery.",
            cards,
            4
        );
        break;
    }

    default:
        return;
    }

    s_ui.page_built[page] = true;

    ESP_LOGI(
        TAG,
        "lazy build page=%d complete in %lld ms",
        (int)page,
        (long long)((esp_timer_get_time() - started_us) / 1000)
    );
}

'''

src = src[:create_pages_pos] + lazy_builder + src[create_pages_pos:]

new_create_pages = '''static void create_pages(lv_obj_t *content)
{
    ESP_LOGI(
        TAG,
        "lazy page mode: creating containers"
    );

    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        s_ui.pages[i] = make_page(content);

        lv_obj_add_flag(
            s_ui.pages[i],
            LV_OBJ_FLAG_HIDDEN
        );
    }

    /*
     * Home is the only full page tree constructed synchronously at boot.
     */
    ensure_page_built(
        DT_UI_PAGE_HOME
    );

    ESP_LOGI(
        TAG,
        "lazy page mode: boot page complete"
    );
}'''

src = replace_function(
    src,
    "static void create_pages(lv_obj_t *content)",
    new_create_pages
)

P.write_text(src)

print(f"Patched: {P}")
print()
print("New behavior:")
print("  - boot builds Home only")
print("  - Control/Files/Filament/Devices/Settings build on first visit")
print("  - live Stage 2 updates remain NULL-safe before Control exists")
print("  - no display/touch/runtime code changed")
print("  - watchdog timeout unchanged")
