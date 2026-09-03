#!/usr/bin/env python3

from pathlib import Path
import shutil
import time

ROOT = Path.cwd()
PARTITIONS = ROOT / "partitions.csv"
SDK = ROOT / "sdkconfig"
DEFAULTS = ROOT / "sdkconfig.defaults"

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = ROOT / "backups" / f"pre-partition-fix-{stamp}"
backup.mkdir(parents=True, exist_ok=True)

for p in (PARTITIONS, SDK, DEFAULTS):
    if p.exists():
        shutil.copy2(p, backup / p.name)

print(f"Backup: {backup}")

# Exact factory layout previously read from this board's original flash.
PARTITIONS.write_text("""# Name,Type,SubType,Offset,Size,Flags
nvs,data,nvs,0x9000,20K,
otadata,data,ota,0xe000,8K,
app0,app,ota_0,0x10000,6400K,
app1,app,ota_1,0x650000,6400K,
spiffs,data,spiffs,0xc90000,3456K,
coredump,data,coredump,0xff0000,64K,
""")

def patch_config(path):
    if not path.exists():
        path.write_text("")

    lines = path.read_text().splitlines()

    remove_prefixes = (
        "CONFIG_PARTITION_TABLE_SINGLE_APP=",
        "# CONFIG_PARTITION_TABLE_SINGLE_APP is not set",
        "CONFIG_PARTITION_TABLE_TWO_OTA=",
        "# CONFIG_PARTITION_TABLE_TWO_OTA is not set",
        "CONFIG_PARTITION_TABLE_CUSTOM=",
        "# CONFIG_PARTITION_TABLE_CUSTOM is not set",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=",
        "CONFIG_PARTITION_TABLE_FILENAME=",
        "CONFIG_PARTITION_TABLE_OFFSET=",
    )

    out = []

    for line in lines:
        if any(
            line.startswith(prefix)
            for prefix in remove_prefixes
        ):
            continue

        out.append(line)

    out.extend([
        "# CONFIG_PARTITION_TABLE_SINGLE_APP is not set",
        "# CONFIG_PARTITION_TABLE_TWO_OTA is not set",
        "CONFIG_PARTITION_TABLE_CUSTOM=y",
        'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"',
        'CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"',
        "CONFIG_PARTITION_TABLE_OFFSET=0x8000",
    ])

    path.write_text("\n".join(out) + "\n")

patch_config(SDK)
patch_config(DEFAULTS)

print()
print("Installed Waveshare factory-size partition layout:")
print("  app0     6400 KiB")
print("  app1     6400 KiB")
print("  SPIFFS   3456 KiB")
print("  coredump   64 KiB")
print()
print("Current DragonTouch image needs only ~1.21 MiB.")
