#!/usr/bin/env python3

from pathlib import Path
import re
import shutil
import sys
import time

BOARD = Path("components/dt_board/dt_board_waveshare_7.c")

if not BOARD.exists():
    sys.exit(f"ERROR: {BOARD} not found")

src = BOARD.read_text()

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    Path("backups")
    / f"pre-no-bounce-{stamp}"
    / BOARD
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(BOARD, backup)

print(f"Backup: {backup}")


# ------------------------------------------------------------
# RGB panel:
#   retain 2 full PSRAM framebuffers
#   completely disable bounce buffers
# ------------------------------------------------------------

src, count_fbs = re.subn(
    r'\.num_fbs\s*=\s*\d+\s*,',
    '.num_fbs = 2,',
    src,
    count=1
)

if count_fbs != 1:
    sys.exit("ERROR: Could not locate .num_fbs")


src, count_bounce = re.subn(
    r'\.bounce_buffer_size_px\s*=\s*[^,]+,',
    '.bounce_buffer_size_px = 0,',
    src,
    count=1
)

if count_bounce != 1:
    sys.exit(
        "ERROR: Could not locate .bounce_buffer_size_px"
    )


# ------------------------------------------------------------
# esp_lvgl_port:
# tell it that RGB bounce-buffer mode is NOT active.
#
# Avoid-tearing stays enabled because we still have two real
# RGB framebuffers.
# ------------------------------------------------------------

src, count_bb = re.subn(
    r'\.bb_mode\s*=\s*true\s*,',
    '.bb_mode = false,',
    src,
    count=1
)

if count_bb == 0:
    src, count_bb = re.subn(
        r'\.bb_mode\s*=\s*1\s*,',
        '.bb_mode = false,',
        src,
        count=1
    )

if count_bb != 1:
    sys.exit(
        "ERROR: Could not uniquely locate esp_lvgl_port bb_mode"
    )


# ------------------------------------------------------------
# Add an unmistakable runtime log.
# ------------------------------------------------------------

marker = "RGB mode: 2 FB, DIRECT, NO bounce buffer"

if marker not in src:
    anchor = '''    ESP_LOGI(
        TAG,
        "Espressif RGB/LVGL port registered"
    );
'''

    replacement = '''    ESP_LOGI(
        TAG,
        "Espressif RGB/LVGL port registered"
    );

    ESP_LOGI(
        TAG,
        "RGB mode: 2 FB, DIRECT, NO bounce buffer"
    );
'''

    if anchor not in src:
        sys.exit(
            "ERROR: Could not locate LVGL-port registration log"
        )

    src = src.replace(
        anchor,
        replacement,
        1
    )


BOARD.write_text(src)

print(f"Patched: {BOARD}")
print()
print("RGB configuration is now:")
print("  - 2 full framebuffers")
print("  - framebuffers in PSRAM")
print("  - LVGL DIRECT mode")
print("  - avoid_tearing enabled")
print("  - bounce buffer DISABLED")
