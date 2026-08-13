# DragonTouch

DragonTouch is the touch-console member of the Dragon device family, targeting
BIGTREETECH K-Touch and PandaTouch hardware. The first milestone is trustworthy
native ESP-IDF board bring-up; family-device discovery and the single-pane control
surface come after the display, touch, backlight, PSRAM, and Wi-Fi paths are proven.

This repository is deliberately groundwork only. It currently provides:

- an ESP-IDF 5.3 / ESP32-S3 build skeleton;
- a clean-room hardware contract derived from publicly documented behavior;
- a compile-time pin-collision guard for the known RGB and GT911 signals;
- a native LVGL printer-console shell using the Dragon design language and red accent;
- bring-up gates that prevent us from jumping straight to product UI;
- CI that compiles the scaffold for ESP32-S3.

No image in this repository is ready to flash to hardware yet. Back up a device's
factory flash and partition table before the first HIL run. The read-only identification
and dual-backup procedure is in [docs/GATE0-RECOVERY.md](docs/GATE0-RECOVERY.md).

## Build

```sh
idf.py set-target esp32s3
idf.py build
```

The expected baseline is ESP-IDF 5.3 or newer, 16 MB flash, and octal PSRAM. The reference firmware selects QIO, but the effective flash mode remains a first-hardware verification item for this native ESP-IDF project.
See [docs/HARDWARE.md](docs/HARDWARE.md) before changing any display timing or pin.

## Roadmap

1. Back up stock flash and record the factory partition map.
2. Prove serial, flash, PSRAM, and a black-screen/backlight-safe boot.
3. Prove stable RGB output under simultaneous Wi-Fi traffic.
4. Prove GT911 touch coordinates and orientation.
5. Connect the LVGL shell to the board display and a board-level HIL pattern.
6. Integrate `dragon-core` Wi-Fi, discovery, and remote-device contracts.
7. Build the same-LAN Dragon-family single-pane UI.

The LVGL shell is documented in [docs/UI.md](docs/UI.md). It compiles independently
of the unfinished display driver and keeps all machine-affecting controls disabled
until a capability-aware product adapter is connected.

## Reference boundary

PaxxTouch is GPL-3.0-labelled software and is used only as a behavioral and hardware
reference. DragonTouch does not copy its implementation. See
[docs/PAXXTOUCH-NOTES.md](docs/PAXXTOUCH-NOTES.md) for provenance and extracted facts.

## License

License selection is intentionally deferred until the project owners choose one.
