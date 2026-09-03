#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

P = Path("components/dt_ui/dt_ui.c")

if not P.exists():
    sys.exit(f"ERROR: missing {P}")

src = P.read_text()

if "NAV_DIAG" in src:
    print("Navigation instrumentation already installed.")
    sys.exit(0)

old = '''static void nav_event(lv_event_t *event)
{
    dt_ui_page_t page = (dt_ui_page_t)(uintptr_t)lv_event_get_user_data(event);
    show_page(page);
}'''

# Account for formatting in the actual source.
if old not in src:
    import re

    pattern = re.compile(
        r'static void nav_event\(lv_event_t \*event\)\s*'
        r'\{\s*'
        r'dt_ui_page_t page\s*=\s*'
        r'\(dt_ui_page_t\)\(uintptr_t\)'
        r'lv_event_get_user_data\(event\);\s*'
        r'show_page\(page\);\s*'
        r'\}',
        re.S
    )

    match = pattern.search(src)

    if not match:
        sys.exit(
            "ERROR: could not uniquely locate nav_event(). "
            "No changes made."
        )

    start, end = match.span()
else:
    start = src.index(old)
    end = start + len(old)


stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    Path("backups")
    / f"pre-nav-instrumentation-{stamp}"
    / P
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(P, backup)

replacement = '''static void nav_event(lv_event_t *event)
{
    static uint32_t sequence = 0;

    dt_ui_page_t page =
        (dt_ui_page_t)(uintptr_t)
        lv_event_get_user_data(event);

    sequence++;

    ESP_LOGI(
        TAG,
        "NAV_DIAG #%lu page=%d",
        (unsigned long)sequence,
        (int)page
    );

    show_page(page);
}'''

src = src[:start] + replacement + src[end:]

P.write_text(src)

print(f"Backup: {backup}")
print(f"Patched: {P}")
print()
print("No UI behavior changed.")
print("Each DragonTouch navigation callback will emit NAV_DIAG.")
