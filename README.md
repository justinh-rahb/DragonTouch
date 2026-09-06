# DragonTouch

DragonTouch is the touch-console member of the Dragon device family: a native
ESP-IDF firmware that turns an ESP32-S3 touch display into a Klipper console,
talking to Moonraker over HTTP.

Two boards are supported, selected in `menuconfig` under **DragonTouch
hardware → Target board**:

- **BIGTREETECH K-Touch / PandaTouch** — the original target.
- **Waveshare ESP32-S3-Touch-LCD-7** — 800x480 RGB565, GT911 touch, 16 MB
  flash, 8 MB octal PSRAM. This branch defaults to it.

## What it does

- **Home** — job state, progress and time, a temperature row, quick-access
  macros, and a live webcam pane.
- **Control** — jog with selectable step, bed levelling, heater presets and
  numeric entry, fan control including discovered `fan_generic` objects.
- **Filament** — capability-aware AFC (Box Turtle) lane control: load,
  unload, eject and recovery reset. Every action is gated on the macros the
  printer actually reports through Moonraker, so a printer without AFC simply
  shows the manual extruder controls.
- **Files** — browse `gcodes`, with embedded thumbnails and metadata.
- **Webcam** — on-demand snapshots decoded on the device.
- **Printers** — save several Moonraker instances and switch between them
  from the header, without restarting.

Commands report their outcome on screen, carrying Klipper's own error text
when one is rejected.

## Building

```sh
idf.py set-target esp32s3
idf.py build flash monitor
```

ESP-IDF 5.3 or newer. `sdkconfig.defaults` pins the board, flash and PSRAM
settings, so a clean checkout builds for the right hardware without running
`menuconfig`.

### dragon-core

DragonTouch depends on [`dragon-core`](https://github.com/justinh-rahb/dragon-core)
for Wi-Fi, the setup portal, the Moonraker client and shared UI. `main/idf_component.yml`
references it by relative path, so the two repositories must sit side by side:

```
projects/
├── DragonTouch/
└── dragon-core/
```

## Configuring for your printer

Gcode macro names, the chamber sensor and the heater ceilings differ from one
machine to the next. They live in
[`components/dt_config/include/dt_printer_profile.h`](components/dt_config/include/dt_printer_profile.h)
with defaults that suit a stock Klipper printer.

**Do not edit that file.** Put a `dt_printer_profile_local.h` beside it
defining only what differs:

```c
#define DT_PROFILE_LEVEL_GCODE  "QUAD_GANTRY_LEVEL"
#define DT_PROFILE_QUICK2_LABEL "Purge Line"
#define DT_PROFILE_QUICK2_GCODE "PURGE_LINE"
#define DT_PROFILE_CHAMBER_SENSOR "enclosure"
```

It is picked up automatically and is gitignored, so a personal setup never
lands in a commit. Nothing in the profile is required: a macro the printer
does not define just fails when pressed, and the error says why.

## Setup

On first boot the device brings up a `DragonTouch_XXXX` access point and
serves a setup portal. Join it to provide Wi-Fi credentials and the Moonraker
host. A hostname is preferable to an IP — it survives DHCP changes.

## Documentation

- [docs/HARDWARE.md](docs/HARDWARE.md) — pin maps and display timing. Read
  before changing either.
- [docs/UI.md](docs/UI.md) — UI contract and the desktop LVGL simulator.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — runtime and component layout.
- [docs/BRINGUP.md](docs/BRINGUP.md) — board bring-up gates.
- [docs/GATE0-RECOVERY.md](docs/GATE0-RECOVERY.md) — back up a device's stock
  flash and partition table before the first flash.

## Reference boundary

PaxxTouch is GPL-3.0-labelled software and is used only as a behavioral and
hardware reference. DragonTouch does not copy its implementation. See
[docs/PAXXTOUCH-NOTES.md](docs/PAXXTOUCH-NOTES.md) for provenance and
extracted facts.

## License

License selection is intentionally deferred until the project owners choose
one.
