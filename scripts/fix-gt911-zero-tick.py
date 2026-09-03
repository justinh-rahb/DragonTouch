#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

path = Path("components/dt_board/dt_board_waveshare_7.c")

if not path.exists():
    sys.exit(f"ERROR: {path} not found")

src = path.read_text()

if "GT911 effective poll interval" in src:
    print("Zero-tick fix already applied.")
    sys.exit(0)

old_init = '''    TickType_t wake_time = xTaskGetTickCount();

    uint32_t read_errors = 0;
'''

new_init = '''    TickType_t wake_time = xTaskGetTickCount();

    /*
     * pdMS_TO_TICKS() truncates. With ESP-IDF's normal 100 Hz
     * FreeRTOS tick, 5 ms becomes zero ticks, which xTaskDelayUntil()
     * rejects. Clamp the cadence to at least one scheduler tick.
     */
    TickType_t poll_ticks =
        pdMS_TO_TICKS(DT_GT911_POLL_MS);

    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    ESP_LOGI(
        TAG,
        "GT911 effective poll interval=%u ms (%u ticks)",
        (unsigned)(poll_ticks * portTICK_PERIOD_MS),
        (unsigned)poll_ticks
    );

    uint32_t read_errors = 0;
'''

if old_init not in src:
    sys.exit(
        "ERROR: Could not locate GT911 poll initialization. "
        "Source not modified."
    )

src = src.replace(old_init, new_init, 1)


old_delay = '''        vTaskDelayUntil(
            &wake_time,
            pdMS_TO_TICKS(DT_GT911_POLL_MS)
        );
'''

new_delay = '''        vTaskDelayUntil(
            &wake_time,
            poll_ticks
        );
'''

if old_delay not in src:
    sys.exit(
        "ERROR: Could not locate GT911 vTaskDelayUntil call. "
        "Source not modified."
    )

src = src.replace(old_delay, new_delay, 1)


stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    Path("backups")
    / f"source-pre-zero-tick-fix-{stamp}"
    / path
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(path, backup)

path.write_text(src)

print(f"Backup: {backup}")
print(f"Patched: {path}")
print("GT911 polling is now clamped to >= 1 FreeRTOS tick.")
