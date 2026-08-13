# Bring-up gates

## Gate 0 — preserve recovery

Follow the read-only identification and dual-backup runbook in
[`GATE0-RECOVERY.md`](GATE0-RECOVERY.md). It deliberately stops before any erase,
write, or restore attempt.

- Identify the exact device revision and USB bridge.
- Read chip/security information.
- Back up the complete flash twice and compare hashes.
- Save the factory partition table and boot log.
- Prove the restore procedure before writing any DragonTouch image.

## Gate 1 — non-display scaffold

- Build with ESP-IDF 5.3 for ESP32-S3.
- Confirm 16 MB flash and expected PSRAM at runtime.
- Confirm the effective flash mode; PaxxTouch selects QIO, while the initial native ESP-IDF scaffold emits DIO unless explicitly configured.
- Confirm serial boot remains stable through repeated power cycles.

## Gate 2 — safe panel bring-up

- Hold backlight off during reset and RGB initialization.
- Start with the documented 14.8 MHz DE-mode timings.
- Show black, white, red, green, blue, checkerboard, and moving bars.
- Confirm channel order and rising-edge sampling.

## Gate 3 — contention

- Use partial render buffers in PSRAM and an internal-SRAM RGB bounce buffer.
- Run sustained Wi-Fi downloads while animating the pattern.
- Observe for horizontal shift, FIFO underflow, color corruption, or stalls.
- Compare modem sleep enabled/disabled; retain the less aggressive setting only if
  the test proves it necessary.

## Gate 4 — touch

- Detect GT911 address without baking in the reference choice first.
- Record raw corner/center points and derive orientation.
- Run a full-screen grid and edge-drag test.

## Gate 5 — Dragon integration

- Add `dragon-core` at one tagged revision.
- Provision same-LAN Wi-Fi without losing recovery access.
- Discover one sibling, display its descriptor, and consume its SSE state read-only.
- Only then add authenticated control actions and multi-device grouping.
