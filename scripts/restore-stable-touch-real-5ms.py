#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

BOARD = Path("components/dt_board/dt_board_waveshare_7.c")
CMAKE = Path("components/dt_board/CMakeLists.txt")
SDK = Path("sdkconfig")
DEFAULTS = Path("sdkconfig.defaults")

# ------------------------------------------------------------
# Find the snapshot taken immediately before the official-GT911
# experiment. That snapshot contains:
#
# - the proven stable RGB configuration
# - custom GT911 acquisition on CPU1
# - buffered press/release queue
# - release debounce
# ------------------------------------------------------------

candidates = sorted(
    Path("backups").glob(
        "pre-official-gt911-*/components/dt_board/dt_board_waveshare_7.c"
    )
)

if not candidates:
    sys.exit(
        "ERROR: pre-official-gt911 board backup not found"
    )

board_backup = candidates[-1]

cmake_backup = (
    board_backup.parents[2]
    / "components/dt_board/CMakeLists.txt"
)

stamp = time.strftime("%Y%m%d-%H%M%S")
safety = Path("backups") / f"pre-real-5ms-touch-{stamp}"

for p in (BOARD, CMAKE, SDK, DEFAULTS):
    if p.exists():
        dst = safety / p
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)

print(f"Safety backup: {safety}")
print(f"Restoring: {board_backup}")

shutil.copy2(
    board_backup,
    BOARD
)

if cmake_backup.exists():
    print(f"Restoring: {cmake_backup}")
    shutil.copy2(
        cmake_backup,
        CMAKE
    )


# ------------------------------------------------------------
# Verify we're back on the desired stable architecture.
# ------------------------------------------------------------

src = BOARD.read_text()

required = [
    ".num_fbs = 1,",
    ".bounce_buffer_size_px = (DT_LCD_H_RES * 10),",
    ".bb_mode = true,",
    ".avoid_tearing = false,",
    "static void gt911_poll_task(void *arg)",
    "static void lvgl_touch_read_cb(",
    "touch_event_push_locked",
    "DT_GT911_RELEASE_DEBOUNCE_MS",
]

for marker in required:
    if marker not in src:
        sys.exit(
            "ERROR: restored snapshot does not contain expected marker:\n"
            f"  {marker}"
        )


# ------------------------------------------------------------
# Change FreeRTOS scheduling resolution from 10 ms to 1 ms.
#
# This makes:
#
#   pdMS_TO_TICKS(5) == 5
#
# instead of zero/one tick at the default 100 Hz.
# ------------------------------------------------------------

def set_freertos_hz(path):
    if not path.exists():
        return

    lines = path.read_text().splitlines()
    out = []
    found = False

    for line in lines:
        if line.startswith("CONFIG_FREERTOS_HZ="):
            if not found:
                out.append("CONFIG_FREERTOS_HZ=1000")
                found = True
        else:
            out.append(line)

    if not found:
        out.append("CONFIG_FREERTOS_HZ=1000")

    path.write_text("\n".join(out) + "\n")


set_freertos_hz(SDK)
set_freertos_hz(DEFAULTS)

print()
print("Restored known-good touch/display implementation.")
print("Set CONFIG_FREERTOS_HZ=1000.")
print()
print("Expected GT911 cadence after rebuild:")
print("  requested = 5 ms")
print("  effective = 5 ms / 5 scheduler ticks")
