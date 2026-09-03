#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

ROOT = Path(".")
BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
UI = Path("components/dt_ui/dt_ui.c")
SDK = Path("sdkconfig")
DEFAULTS = Path("sdkconfig.defaults")


def latest(pattern):
    matches = sorted(Path("backups").glob(pattern))
    return matches[-1] if matches else None


# --------------------------------------------------------------------
# Restore the version where the user reported touch was basically perfect:
#
# - board: immediately before custom tear-free double-FB experiment
# - UI: immediately before navigation/style optimization experiment
# --------------------------------------------------------------------

board_backup = latest(
    "source-pre-double-fb-*/components/dt_board/dt_board_waveshare_7.c"
)

ui_backup = latest(
    "source-pre-ui-optimization-*/components/dt_ui/dt_ui.c"
)

if board_backup is None:
    sys.exit(
        "ERROR: cannot find source-pre-double-fb board backup"
    )

if ui_backup is None:
    sys.exit(
        "ERROR: cannot find source-pre-ui-optimization UI backup"
    )

stamp = time.strftime("%Y%m%d-%H%M%S")
safety = Path("backups") / f"source-pre-parallel-render-{stamp}"

for p in (BOARD, UI, SDK, DEFAULTS):
    if p.exists():
        dst = safety / p
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)

print(f"Safety backup: {safety}")
print(f"Restoring board: {board_backup}")
print(f"Restoring UI:    {ui_backup}")

shutil.copy2(board_backup, BOARD)
shutil.copy2(ui_backup, UI)


# --------------------------------------------------------------------
# Configure LVGL 9.3 to use FreeRTOS internally and two SW draw units.
#
# LVGL will create two renderer threads. Our existing dt_lvgl task still
# owns LVGL API/event processing, but pixel rasterization can execute in
# parallel across the ESP32-S3's two cores.
# --------------------------------------------------------------------

SETTINGS = {
    "CONFIG_LV_USE_OS":
        "CONFIG_LV_USE_OS=2",

    "CONFIG_LV_OS_NONE":
        "# CONFIG_LV_OS_NONE is not set",

    "CONFIG_LV_OS_FREERTOS":
        "CONFIG_LV_OS_FREERTOS=y",

    "CONFIG_LV_USE_FREERTOS_TASK_NOTIFY":
        "CONFIG_LV_USE_FREERTOS_TASK_NOTIFY=y",

    "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT":
        "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2",

    "CONFIG_LV_DRAW_THREAD_STACK_SIZE":
        "CONFIG_LV_DRAW_THREAD_STACK_SIZE=8192",

    "CONFIG_LV_DRAW_THREAD_PRIO":
        "CONFIG_LV_DRAW_THREAD_PRIO=3",

    "CONFIG_COMPILER_OPTIMIZATION_PERF":
        "CONFIG_COMPILER_OPTIMIZATION_PERF=y",

    "CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM":
        "CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y",

    "CONFIG_LV_DEF_REFR_PERIOD":
        "CONFIG_LV_DEF_REFR_PERIOD=10",
}


def update_config(path):
    if not path.exists():
        path.write_text("")

    lines = path.read_text().splitlines()
    out = []
    seen = set()

    for line in lines:
        replaced = False

        for key, value in SETTINGS.items():
            if (
                line.startswith(key + "=")
                or line == f"# {key} is not set"
            ):
                if key not in seen:
                    out.append(value)
                    seen.add(key)

                replaced = True
                break

        if not replaced:
            out.append(line)

    for key, value in SETTINGS.items():
        if key not in seen:
            out.append(value)

    path.write_text("\n".join(out) + "\n")


update_config(SDK)
update_config(DEFAULTS)


# --------------------------------------------------------------------
# Add runtime confirmation to the board source.
# --------------------------------------------------------------------

src = BOARD.read_text()

marker = "LVGL renderer: OS=%d SW draw units=%d"

if marker not in src:
    anchor = """    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);
"""

    replacement = """    lv_init();

    ESP_LOGI(
        TAG,
        "LVGL renderer: OS=%d SW draw units=%d",
        (int)LV_USE_OS,
        (int)LV_DRAW_SW_DRAW_UNIT_CNT
    );

    lv_tick_set_cb(lvgl_tick_ms);
"""

    if anchor not in src:
        sys.exit(
            "ERROR: could not locate lv_init() anchor after restore"
        )

    src = src.replace(anchor, replacement, 1)
    BOARD.write_text(src)


print()
print("Restored known-good interaction baseline.")
print("Enabled:")
print("  - LVGL FreeRTOS integration")
print("  - 2 parallel software draw units")
print("  - 8 KB stack per drawing thread")
print("  - -O2 performance optimization")
print("  - LVGL fast IRAM")
print("  - 10 ms LVGL refresh period")
