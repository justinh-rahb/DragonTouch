#!/usr/bin/env python3

from pathlib import Path
import shutil
import time

SDK = Path("sdkconfig")
DEFAULTS = Path("sdkconfig.defaults")

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = Path("backups") / f"pre-disable-parallel-lvgl-{stamp}"

for p in (SDK, DEFAULTS):
    if p.exists():
        dst = backup / p
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)

print(f"Backup: {backup}")

def update(path, defaults=False):
    if not path.exists():
        return

    lines = path.read_text().splitlines()
    out = []

    remove_prefixes = (
        "CONFIG_LV_OS_NONE=",
        "CONFIG_LV_OS_FREERTOS=",
        "CONFIG_LV_OS_PTHREAD=",
        "CONFIG_LV_OS_CMSIS_RTOS2=",
        "CONFIG_LV_OS_RTTHREAD=",
        "CONFIG_LV_OS_WINDOWS=",
        "CONFIG_LV_OS_MQX=",
        "CONFIG_LV_OS_CUSTOM=",
        "CONFIG_LV_USE_FREERTOS_TASK_NOTIFY=",
        "CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=",
        "CONFIG_LV_DRAW_THREAD_STACK_SIZE=",
        "CONFIG_LV_DRAW_THREAD_PRIO=",
    )

    remove_comments = (
        "# CONFIG_LV_OS_NONE is not set",
        "# CONFIG_LV_OS_FREERTOS is not set",
        "# CONFIG_LV_USE_FREERTOS_TASK_NOTIFY is not set",
    )

    for line in lines:
        if line.startswith(remove_prefixes):
            continue
        if line in remove_comments:
            continue
        out.append(line)

    out.append("CONFIG_LV_OS_NONE=y")
    out.append("CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1")

    if not defaults:
        out.append("# CONFIG_LV_OS_FREERTOS is not set")

    path.write_text("\n".join(out) + "\n")


update(SDK)
update(DEFAULTS, defaults=True)

print("Disabled LVGL parallel rendering.")
print("Preserved:")
print("  - -O2 compiler optimization")
print("  - LVGL fast IRAM")
print("  - 10 ms refresh/input cadence")
print("  - internal-SRAM LVGL partial buffers")
print("  - dedicated GT911 acquisition task")
