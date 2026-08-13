# Gate 0 read-only recovery capture

This runbook identifies one K-Touch/PandaTouch and captures its complete 16 MiB flash
twice. It does not authorize erasing, writing, or flashing hardware. A matching backup
is necessary but does not by itself prove that restoration is safe.

The commands below were checked against ESP-IDF 5.3.1's existing Python environment:
`/Users/justinh/.espressif/python_env/idf5.3_py3.14_env/bin/python`, with esptool
4.12.0. Re-check `python -m esptool version` when reproducing the capture.

## Before connecting

1. Photograph the enclosure, labels, PCB revision, USB bridge, cables, and power state.
2. Disconnect the printer and every other serial device. DragonTouch must not be
   connected to a printer control bus during this capture.
3. Choose an explicit backup directory on reliable storage. Do not use a temporary
   directory. Record the computer, date, cable, and physical device revision.
4. Locate the single new serial device after connecting USB. On macOS, compare the
   output of `ls /dev/cu.*` before and after connection; do not guess the port.

Set these values explicitly for the current device:

```sh
DT_IDF_PYTHON=/Users/justinh/.espressif/python_env/idf5.3_py3.14_env/bin/python
DT_PORT=/dev/cu.usbserial-REPLACE_ME
DT_BACKUP_DIR=/absolute/path/DragonTouch-factory-backup-REPLACE_DATE
set -o pipefail
mkdir -p "$DT_BACKUP_DIR"
```

## Identify without writing flash

Put the ESP32-S3 into its ROM download mode using the board's physical boot/reset
procedure. These commands reset the chip and may load Espressif's temporary flasher
stub into RAM, but do not write SPI flash:

```sh
"$DT_IDF_PYTHON" -m esptool --chip esp32s3 --port "$DT_PORT" chip_id 2>&1 | tee "$DT_BACKUP_DIR/chip-id.txt"
"$DT_IDF_PYTHON" -m esptool --chip esp32s3 --port "$DT_PORT" read_mac 2>&1 | tee "$DT_BACKUP_DIR/mac.txt"
"$DT_IDF_PYTHON" -m esptool --chip esp32s3 --port "$DT_PORT" flash_id 2>&1 | tee "$DT_BACKUP_DIR/flash-id.txt"
"$DT_IDF_PYTHON" -m esptool --chip esp32s3 --port "$DT_PORT" get_security_info 2>&1 | tee "$DT_BACKUP_DIR/security-info.txt"
```

Stop if the chip is not reported as ESP32-S3, flash capacity is not 16 MiB, security
state is unexpected, or the physical device identity is uncertain. Preserve the raw
command output even when a check fails.

## Capture two independent full reads

Re-enter ROM download mode before each read. Use the full address range
`0x000000–0xFFFFFF`; do not substitute an application or partition-sized range.

```sh
"$DT_IDF_PYTHON" -m esptool --chip esp32s3 --port "$DT_PORT" --baud 460800 read_flash 0x0 0x1000000 "$DT_BACKUP_DIR/factory-a.bin" 2>&1 | tee "$DT_BACKUP_DIR/read-a.txt"
```

Disconnect USB, reconnect it, re-identify the port, re-enter ROM download mode, and
perform the second read into a different file:

```sh
"$DT_IDF_PYTHON" -m esptool --chip esp32s3 --port "$DT_PORT" --baud 460800 read_flash 0x0 0x1000000 "$DT_BACKUP_DIR/factory-b.bin" 2>&1 | tee "$DT_BACKUP_DIR/read-b.txt"
```

Both files must be exactly 16,777,216 bytes and byte-for-byte identical:

```sh
wc -c "$DT_BACKUP_DIR/factory-a.bin" "$DT_BACKUP_DIR/factory-b.bin"
shasum -a 256 "$DT_BACKUP_DIR/factory-a.bin" "$DT_BACKUP_DIR/factory-b.bin" | tee "$DT_BACKUP_DIR/sha256.txt"
cmp "$DT_BACKUP_DIR/factory-a.bin" "$DT_BACKUP_DIR/factory-b.bin"
```

Any size mismatch, hash mismatch, or non-zero `cmp` result fails Gate 0. Do not average,
patch, or choose one read; diagnose the connection and repeat both captures.

## Preserve the conventional partition-table region

The complete images already contain this data. Extract the conventional ESP-IDF
partition-table sector for inspection without assuming that the factory used it:

```sh
dd if="$DT_BACKUP_DIR/factory-a.bin" of="$DT_BACKUP_DIR/partition-table-sector.bin" bs=4096 skip=8 count=1
"$DT_IDF_PYTHON" /Users/justinh/esp/esp-idf/components/partition_table/gen_esp32part.py "$DT_BACKUP_DIR/partition-table-sector.bin" "$DT_BACKUP_DIR/partition-table.csv"
```

If parsing fails, record that result and investigate the factory layout from the full
image; do not infer a partition offset. Capture a stock 115200-baud boot log separately
after leaving download mode, including reset reason, flash mode, and detected memory.

## Gate remains closed

Copy the directory to a second storage device and verify its SHA-256 manifest there.
Do not run esptool commands containing `write`, `erase`, or `verify_flash` against the
device. A later, explicitly reviewed procedure must account for secure boot, flash
encryption, eFuses, partition layout, bootloader compatibility, and a proven stock
restore before DragonTouch receives hardware-write permission.
