#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

PATH = Path("components/dt_board/dt_board_waveshare_7.c")

if not PATH.exists():
    sys.exit(f"ERROR: {PATH} not found")

src = PATH.read_text()

old = "tp_io_cfg.scl_speed_hz = 400000;"
new = """/*
     * Legacy ESP-IDF I2C panel IO requires this to be zero.
     * The bus itself was already configured at 400 kHz by
     * waveshare_i2c_init().
     */
    tp_io_cfg.scl_speed_hz = 0;"""

if old not in src:
    if "tp_io_cfg.scl_speed_hz = 0;" in src:
        print("Legacy-I2C GT911 fix already applied.")
        sys.exit(0)

    sys.exit(
        "ERROR: expected GT911 scl_speed_hz assignment not found. "
        "No changes made."
    )

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    Path("backups")
    / f"pre-gt911-legacy-i2c-fix-{stamp}"
    / PATH
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(PATH, backup)

src = src.replace(old, new, 1)
PATH.write_text(src)

print(f"Backup: {backup}")
print(f"Patched: {PATH}")
print()
print("GT911 panel IO:")
print("  scl_speed_hz field = 0")
print("  actual shared I2C bus remains 400 kHz")
