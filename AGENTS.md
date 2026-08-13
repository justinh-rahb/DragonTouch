# DragonTouch agent notes

- Treat this as clean-room firmware. PaxxTouch is a factual reference, not a source
  tree to transplant.
- Do not flash real K-Touch/PandaTouch hardware until factory flash backup and restore
  are demonstrated.
- Use native ESP-IDF 5.3+ and target `esp32s3`.
- Preserve the documented 14.8 MHz RGB timing baseline until hardware testing supports
  a change.
- Keep internal SRAM available for RGB DMA; large UI/image buffers belong in PSRAM.
- Add hardware support in measurable gates described in `docs/BRINGUP.md`.
- Shared networking/discovery contracts belong in `dragon-core`; this repository owns
  physical board support and the DragonTouch product UI.
- Prefer HTTP/SSE for Dragon-family live state. Do not assume WebSockets.
- Preserve user credentials and NVS keys across changes once a persistence schema ships.
