# PaxxTouch reference notes

Reference: <https://github.com/TechJeeper/PaxxTouch>

- Inspected commit: `dcdc8bf57ed7aaa0eb6e25123801f7b2ee85c552`
- Tag at inspection: `v0.3.1`
- Inspection date: 2026-08-13
- Upstream README declares GPL-3.0; GitHub metadata did not expose a license file.

## What it establishes

PaxxTouch demonstrates that K-Touch/PandaTouch can run an Arduino-on-ESP-IDF 5.x
application with LVGL 9.3, an 800×480 RGB panel, GT911 touch, Wi-Fi, HTTP workloads,
and image decode. Its implementation also documents practical stability constraints:
14.8 MHz PCLK, rising-edge sampling, a 20-line SRAM RGB bounce buffer, PSRAM-backed
LVGL buffers, Wi-Fi power-save disabled, RGB565 red/blue swapping, and persistent
backlight handling.

Its U1 remote mode is a useful workload model, not DragonTouch's product architecture:
it polls a 480×320 snapshot every 100 ms with ETag/304 handling, decodes only changed
frames, pipelines polling and decode across cores, and sends touch asynchronously.
That is an excellent stress test for our eventual display/Wi-Fi layer.

## Clean-room rule

Do not copy source, comments, UI assets, or documentation from PaxxTouch. Hardware
pin assignments, protocol behavior, and measured constraints may be restated as facts
with provenance. DragonTouch drivers and tests must be independently authored against
ESP-IDF APIs and our own hardware observations.

Before release, re-check the upstream repository for an actual license file and record
any other primary hardware source we obtain from BIGTREETECH.
