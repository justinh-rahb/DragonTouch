#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

UI = Path("components/dt_ui/dt_ui.c")
BOARD = Path("components/dt_board/dt_board_waveshare_7.c")

for p in (UI, BOARD):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

ui = UI.read_text()
board = BOARD.read_text()

if "DragonTouch optimized page switch" in ui:
    print("Navigation optimization already applied.")
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


# ---------------------------------------------------------------------
# Backup
# ---------------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"source-pre-ui-optimization-{stamp}"

for p in (UI, BOARD):
    dst = backup / p
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Backup: {backup}")


# ---------------------------------------------------------------------
# Add active-page state
# ---------------------------------------------------------------------

old = """    lv_obj_t *dialog_confirm_label;
    bool ready;
} dt_ui_state_t;
"""

new = """    lv_obj_t *dialog_confirm_label;

    /*
     * DragonTouch optimized page switch:
     * only the previous and next pages are touched during navigation.
     */
    dt_ui_page_t active_page;
    bool active_page_valid;

    bool ready;
} dt_ui_state_t;
"""

if old not in ui:
    sys.exit("ERROR: UI state anchor not found")

ui = ui.replace(old, new, 1)


# ---------------------------------------------------------------------
# Replace show_page()
# ---------------------------------------------------------------------

new_show_page = r'''static void set_nav_active(dt_ui_page_t page, bool active)
{
    lv_obj_set_style_text_color(
        s_ui.nav_icons[page],
        color(active ? DT_COLOR_ACCENT : DT_COLOR_MUTED),
        0
    );

    lv_obj_set_style_bg_opa(
        s_ui.nav_buttons[page],
        active ? LV_OPA_20 : LV_OPA_TRANSP,
        0
    );

    /*
     * Border color/side are static and established once when the
     * navigation button is created. Only its width changes here.
     */
    lv_obj_set_style_border_width(
        s_ui.nav_buttons[page],
        active ? 3 : 0,
        0
    );
}


static void show_page(dt_ui_page_t selected)
{
    /*
     * Do absolutely nothing when clicking the already active page.
     */
    if (
        s_ui.active_page_valid &&
        s_ui.active_page == selected
    ) {
        return;
    }

    if (!s_ui.active_page_valid) {
        /*
         * One-time initialization. Previously show_page() performed
         * this complete six-page sweep on EVERY navigation event.
         */
        for (int i = 0; i < DT_PAGE_COUNT; ++i) {
            lv_obj_add_flag(
                s_ui.pages[i],
                LV_OBJ_FLAG_HIDDEN
            );

            set_nav_active(
                (dt_ui_page_t)i,
                false
            );
        }

    } else {
        const dt_ui_page_t previous =
            s_ui.active_page;

        lv_obj_add_flag(
            s_ui.pages[previous],
            LV_OBJ_FLAG_HIDDEN
        );

        set_nav_active(
            previous,
            false
        );
    }

    lv_obj_remove_flag(
        s_ui.pages[selected],
        LV_OBJ_FLAG_HIDDEN
    );

    set_nav_active(
        selected,
        true
    );

    s_ui.active_page = selected;
    s_ui.active_page_valid = true;
}'''

try:
    ui = replace_function(
        ui,
        "static void show_page(dt_ui_page_t selected)",
        new_show_page
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ---------------------------------------------------------------------
# Stop doing whole-object translucent rendering for disabled buttons.
#
# A disabled button is now drawn opaque using a muted surface color.
# This avoids blending every child/pixel of the button at 50% opacity.
# ---------------------------------------------------------------------

old = """    lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
"""

new = """    /*
     * Avoid whole-object opacity for interactive states.
     * Software alpha composition is unnecessarily expensive on ESP32-S3.
     */
    lv_obj_set_style_bg_color(
        button,
        color(DT_COLOR_BORDER),
        LV_STATE_PRESSED
    );

    lv_obj_set_style_bg_opa(
        button,
        LV_OPA_COVER,
        LV_STATE_PRESSED
    );

    lv_obj_set_style_bg_color(
        button,
        color(DT_COLOR_SURFACE),
        LV_STATE_DISABLED
    );

    lv_obj_set_style_bg_opa(
        button,
        LV_OPA_COVER,
        LV_STATE_DISABLED
    );
"""

if old not in ui:
    sys.exit("ERROR: button opacity anchor not found")

ui = ui.replace(old, new, 1)


# ---------------------------------------------------------------------
# Give disabled labels a muted color instead of dimming the whole object.
# ---------------------------------------------------------------------

new_set_enabled = r'''static void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(
            button,
            LV_STATE_DISABLED
        );
    } else {
        lv_obj_add_state(
            button,
            LV_STATE_DISABLED
        );
    }

    lv_obj_t *label =
        lv_obj_get_child(button, 0);

    if (label != NULL) {
        lv_obj_set_style_text_color(
            label,
            color(enabled ? DT_COLOR_TEXT : DT_COLOR_MUTED),
            0
        );
    }
}'''

try:
    ui = replace_function(
        ui,
        "static void set_button_enabled(lv_obj_t *button, bool enabled)",
        new_set_enabled
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ---------------------------------------------------------------------
# Border color and side belong in one-time nav creation, not every click.
# ---------------------------------------------------------------------

anchor = """        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_radius(button, 4, 0);
"""

replacement = """        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_radius(button, 4, 0);

        /*
         * Static active-marker properties. show_page() now only changes
         * border width.
         */
        lv_obj_set_style_border_color(
            button,
            color(DT_COLOR_ACCENT),
            0
        );

        lv_obj_set_style_border_side(
            button,
            LV_BORDER_SIDE_LEFT,
            0
        );

        lv_obj_set_style_border_width(
            button,
            0,
            0
        );
"""

if anchor not in ui:
    sys.exit("ERROR: nav creation anchor not found")

ui = ui.replace(anchor, replacement, 1)


UI.write_text(ui)


# ---------------------------------------------------------------------
# Increase internal LVGL partial buffers slightly.
#
# 32 rows = 51,200 bytes each / 102,400 bytes total.
# This still fits comfortably while reducing flush count by 25%.
# ---------------------------------------------------------------------

if "#define DT_LVGL_BUFFER_ROWS   24" in board:
    board = board.replace(
        "#define DT_LVGL_BUFFER_ROWS   24",
        "#define DT_LVGL_BUFFER_ROWS   32",
        1
    )

BOARD.write_text(board)


print("Optimization applied.")
print()
print("Changes:")
print("  - page switches modify only old + new page")
print("  - clicking current page becomes a no-op")
print("  - nav static styles no longer rewritten every click")
print("  - disabled buttons no longer use whole-object 50% alpha")
print("  - LVGL internal buffers increased 24 -> 32 rows")
