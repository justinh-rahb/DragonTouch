#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time


BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
UI = Path("components/dt_ui/dt_ui.c")
SDK = Path("sdkconfig")
DEFAULTS = Path("sdkconfig.defaults")


def find_function(text, signature):
    start = text.find(signature)

    if start < 0:
        raise RuntimeError(f"function missing: {signature}")

    brace = text.find("{", start)

    if brace < 0:
        raise RuntimeError(f"opening brace missing: {signature}")

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

    raise RuntimeError(f"closing brace missing: {signature}")


# ------------------------------------------------------------
# Safety backup
# ------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"pre-final-touch-cleanup-{stamp}"

for p in (BOARD, UI, SDK, DEFAULTS):
    if p.exists():
        dst = backup / p
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)

print(f"Safety backup: {backup}")


# ------------------------------------------------------------
# Restore board implementation from immediately BEFORE the
# diagnostic instrumentation was added.
#
# That preserves:
#   - stable RGB implementation
#   - custom GT911 polling
#   - buffered transition queue
#   - release debounce
#
# while removing:
#   - GPIO4 diagnostic ISR
#   - TOUCH_DIAG counters
#   - diagnostic task
#   - LVGL diagnostic callbacks
# ------------------------------------------------------------

candidates = sorted(
    Path("backups").glob(
        "pre-touch-instrumentation-*/"
        "components/dt_board/dt_board_waveshare_7.c"
    )
)

if not candidates:
    sys.exit(
        "ERROR: pre-touch-instrumentation backup not found"
    )

clean_board = candidates[-1]

print(f"Restoring clean board layer from: {clean_board}")

shutil.copy2(
    clean_board,
    BOARD
)


# ------------------------------------------------------------
# Verify the known-good RGB/touch architecture survived.
# ------------------------------------------------------------

board = BOARD.read_text()

required_board = [
    ".num_fbs = 1,",
    ".bounce_buffer_size_px = (DT_LCD_H_RES * 10),",
    ".bb_mode = true,",
    ".avoid_tearing = false,",
    "touch_event_push_locked",
    "DT_GT911_RELEASE_DEBOUNCE_MS",
    "static void gt911_poll_task(void *arg)",
    "static void lvgl_touch_read_cb(",
]

for marker in required_board:
    if marker not in board:
        sys.exit(
            "ERROR: restored board missing expected marker:\n"
            f"  {marker}"
        )


# ------------------------------------------------------------
# Remove NAV_DIAG instrumentation ONLY.
#
# Preserve the critical make_nav_icon() hit-testing fix.
# ------------------------------------------------------------

ui = UI.read_text()

icon_fix = (
    "Decorative nav icons must never consume pointer input"
)

if icon_fix not in ui:
    sys.exit(
        "ERROR: nav-icon hit-testing fix is missing. "
        "Refusing cleanup."
    )

start, end = find_function(
    ui,
    "static void nav_event(lv_event_t *event)"
)

normal_nav = '''static void nav_event(lv_event_t *event)
{
    dt_ui_page_t page =
        (dt_ui_page_t)(uintptr_t)
        lv_event_get_user_data(event);

    show_page(page);
}'''

ui = ui[:start] + normal_nav + ui[end:]

UI.write_text(ui)


# ------------------------------------------------------------
# Restore normal FreeRTOS tick rate.
#
# Increasing this to 1000 Hz did NOT improve the behavior.
# At 100 Hz the GT911 polling task falls back to a 10 ms
# effective interval, which has now been demonstrated sufficient.
# ------------------------------------------------------------

def set_hz(path, hz):
    if not path.exists():
        return

    lines = path.read_text().splitlines()

    found = False
    out = []

    for line in lines:
        if line.startswith("CONFIG_FREERTOS_HZ="):
            if not found:
                out.append(f"CONFIG_FREERTOS_HZ={hz}")
                found = True
        else:
            out.append(line)

    if not found:
        out.append(f"CONFIG_FREERTOS_HZ={hz}")

    path.write_text(
        "\n".join(out) + "\n"
    )


set_hz(SDK, 100)
set_hz(DEFAULTS, 100)


print()
print("Final working configuration:")
print()
print("DISPLAY:")
print("  1 PSRAM framebuffer")
print("  800x10 RGB bounce buffer")
print("  internal DMA LVGL buffers")
print("  esp_lvgl_port on CPU0")
print()
print("TOUCH:")
print("  GT911 custom polling")
print("  buffered press/release transitions")
print("  release debounce")
print("  normal 100 Hz FreeRTOS tick")
print()
print("UI FIX:")
print("  decorative nav icons are NOT clickable")
print()
print("REMOVED:")
print("  TOUCH_DIAG")
print("  passive GPIO4 diagnostic ISR")
print("  diagnostic statistics task")
print("  NAV_DIAG")
