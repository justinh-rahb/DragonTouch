#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

P = Path("components/dt_board/dt_board_waveshare_7.c")

if not P.exists():
    sys.exit(f"ERROR: missing {P}")

src = P.read_text()

required = [
    ".num_fbs = 1,",
    ".bounce_buffer_size_px = (DT_LCD_H_RES * 10),",
    ".buff_dma = true,",
    ".buff_spiram = false,",
    ".direct_mode = false,",
    ".bb_mode = true,",
    ".avoid_tearing = false,",
]

for marker in required:
    if marker not in src:
        sys.exit(
            "ERROR: known-good RGB baseline marker missing:\n"
            f"  {marker}\n"
            "No changes made."
        )

old = '''.buffer_size =
            DT_LCD_H_RES * 20,'''

new = '''.buffer_size =
            DT_LCD_H_RES * 10,'''

if old not in src:
    if new in src:
        print("LVGL draw buffers already set to 10 rows.")
        sys.exit(0)

    sys.exit(
        "ERROR: expected 20-row LVGL buffer configuration "
        "not found. No changes made."
    )

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    Path("backups")
    / f"pre-lvgl-10row-buffer-{stamp}"
    / P
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(P, backup)

src = src.replace(old, new, 1)
P.write_text(src)

print(f"Backup: {backup}")
print(f"Patched: {P}")
print()
print("RGB scanout configuration: UNCHANGED")
print("GT911 configuration:       UNCHANGED")
print()
print("LVGL draw buffers:")
print("  before: 2 x 800 x 20 RGB565 = ~64 KB internal DMA RAM")
print("  after:  2 x 800 x 10 RGB565 = ~32 KB internal DMA RAM")
