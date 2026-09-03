#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

SRC = Path("components/dt_board/dt_board_waveshare_7.c")
UI = Path("components/dt_ui/dt_ui.c")
SDK = Path("sdkconfig")
DEFAULTS = Path("sdkconfig.defaults")

for p in (SRC, UI, SDK, DEFAULTS):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"source-pre-performance-{stamp}"

for p in (SRC, UI, SDK, DEFAULTS):
    dst = backup / p
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Backup: {backup}")


def configure_sdkconfig(path: Path):
    text = path.read_text().splitlines()

    choices = {
        "CONFIG_COMPILER_OPTIMIZATION_DEBUG":
            "# CONFIG_COMPILER_OPTIMIZATION_DEBUG is not set",
        "CONFIG_COMPILER_OPTIMIZATION_SIZE":
            "# CONFIG_COMPILER_OPTIMIZATION_SIZE is not set",
        "CONFIG_COMPILER_OPTIMIZATION_NONE":
            "# CONFIG_COMPILER_OPTIMIZATION_NONE is not set",
        "CONFIG_COMPILER_OPTIMIZATION_PERF":
            "CONFIG_COMPILER_OPTIMIZATION_PERF=y",
        "CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM":
            "CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y",
        "CONFIG_LV_DEF_REFR_PERIOD":
            "CONFIG_LV_DEF_REFR_PERIOD=10",
        "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240":
            "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y",
    }

    seen = set()
    output = []

    for line in text:
        matched = False

        for key, replacement in choices.items():
            if (
                line.startswith(key + "=")
                or line.startswith("# " + key + " is not set")
            ):
                if key not in seen:
                    output.append(replacement)
                    seen.add(key)
                matched = True
                break

        if not matched:
            output.append(line)

    for key, replacement in choices.items():
        if key not in seen:
            output.append(replacement)

    path.write_text("\n".join(output) + "\n")


def configure_defaults(path: Path):
    keys = (
        "CONFIG_COMPILER_OPTIMIZATION_DEBUG",
        "CONFIG_COMPILER_OPTIMIZATION_SIZE",
        "CONFIG_COMPILER_OPTIMIZATION_NONE",
        "CONFIG_COMPILER_OPTIMIZATION_PERF",
        "CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM",
        "CONFIG_LV_DEF_REFR_PERIOD",
    )

    lines = []

    for line in path.read_text().splitlines():
        if any(key in line for key in keys):
            continue
        lines.append(line)

    lines += [
        "",
        "# Waveshare / LVGL performance",
        "CONFIG_COMPILER_OPTIMIZATION_PERF=y",
        "CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y",
        "CONFIG_LV_DEF_REFR_PERIOD=10",
    ]

    path.write_text("\n".join(lines) + "\n")


configure_sdkconfig(SDK)
configure_defaults(DEFAULTS)


# ------------------------------------------------------------
# Pin LVGL to CPU1.
#
# RGB peripheral initialization occurred on CPU0, so keeping the
# software renderer on CPU1 avoids unnecessary competition with
# RGB interrupt work.
# ------------------------------------------------------------

src = SRC.read_text()

old = '''BaseType_t result = xTaskCreate(
        lvgl_task,
        "dt_lvgl",
        24576,
        NULL,
        5,
        &s_lvgl_task
    );'''

new = '''BaseType_t result = xTaskCreatePinnedToCore(
        lvgl_task,
        "dt_lvgl",
        24576,
        NULL,
        5,
        &s_lvgl_task,
        1
    );'''

if old not in src:
    sys.exit("ERROR: Could not locate LVGL task creation")

src = src.replace(old, new, 1)


# ------------------------------------------------------------
# Keep GT911 acquisition on CPU0.
# ------------------------------------------------------------

old = '''BaseType_t touch_result = xTaskCreate(
        gt911_poll_task,
        "gt911",
        4096,
        NULL,
        5,
        &s_touch_task
    );'''

new = '''BaseType_t touch_result = xTaskCreatePinnedToCore(
        gt911_poll_task,
        "gt911",
        4096,
        NULL,
        5,
        &s_touch_task,
        0
    );'''

if old not in src:
    sys.exit("ERROR: Could not locate GT911 task creation")

src = src.replace(old, new, 1)

SRC.write_text(src)


# ------------------------------------------------------------
# Make navigation react on press rather than waiting for release.
#
# This is appropriate for the page-navigation rail and lets us
# distinguish input latency from redraw latency immediately.
# ------------------------------------------------------------

ui = UI.read_text()

old = '''lv_obj_add_event_cb(button, nav_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);'''

new = '''lv_obj_add_event_cb(button, nav_event, LV_EVENT_PRESSED, (void *)(uintptr_t)i);'''

if old not in ui:
    sys.exit("ERROR: Could not locate DragonTouch navigation callback")

ui = ui.replace(old, new, 1)
UI.write_text(ui)


print("Performance patch applied:")
print("  compiler: -O2")
print("  LVGL fast functions: IRAM")
print("  LVGL refresh period: 10 ms")
print("  LVGL task: CPU1")
print("  GT911 task: CPU0")
print("  navigation: react on PRESSED")
