#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

P = Path("components/dt_ui/dt_ui.c")

if not P.exists():
    sys.exit(f"ERROR: missing {P}")

src = P.read_text()

marker = "Decorative nav icons must never consume pointer input"

if marker in src:
    print("Nav icon hit-testing fix already applied.")
    sys.exit(0)

old = '''static lv_obj_t *make_nav_icon(lv_obj_t *parent, dt_nav_icon_t type)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 22, 22);
    lv_obj_set_style_text_color(icon, color(DT_COLOR_MUTED), 0);
    lv_obj_add_event_cb(icon, nav_icon_draw, LV_EVENT_DRAW_MAIN, (void *)(uintptr_t)type);
    return icon;
}'''

new = '''static lv_obj_t *make_nav_icon(lv_obj_t *parent, dt_nav_icon_t type)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);

    /*
     * Decorative nav icons must never consume pointer input.
     *
     * lv_obj_create() creates a CLICKABLE base object by default.
     * Without removing that flag, touches landing directly on the
     * 22x22 icon target the icon instead of the parent nav button,
     * so the parent's LV_EVENT_CLICKED callback never runs.
     */
    lv_obj_remove_flag(
        icon,
        LV_OBJ_FLAG_CLICKABLE |
        LV_OBJ_FLAG_CLICK_FOCUSABLE |
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_size(icon, 22, 22);
    lv_obj_set_style_text_color(icon, color(DT_COLOR_MUTED), 0);
    lv_obj_add_event_cb(
        icon,
        nav_icon_draw,
        LV_EVENT_DRAW_MAIN,
        (void *)(uintptr_t)type
    );

    return icon;
}'''

if old not in src:
    sys.exit(
        "ERROR: expected make_nav_icon() implementation not found. "
        "Nothing modified."
    )

stamp = time.strftime("%Y%m%d-%H%M%S")

backup = (
    Path("backups")
    / f"pre-nav-hit-test-fix-{stamp}"
    / P
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(P, backup)

src = src.replace(old, new, 1)
P.write_text(src)

print(f"Backup: {backup}")
print(f"Patched: {P}")
print()
print("Fixed:")
print("  nav icon CLICKABLE      -> disabled")
print("  nav icon CLICK_FOCUSABLE -> disabled")
print("  nav icon SCROLLABLE     -> disabled")
print()
print("Parent nav button remains clickable.")
