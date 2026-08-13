# K-Touch / PandaTouch hardware contract

Status: **reference-derived, not yet independently measured on our unit**.

The facts below come from PaxxTouch commit
`dcdc8bf57ed7aaa0eb6e25123801f7b2ee85c552` (tag `v0.3.1`, 2026-08-12).
They are isolated here so later measurement can promote, correct, or reject each item.

## Target envelope

| Item | Working contract | Confidence |
|---|---:|---|
| MCU | ESP32-S3 | high; reference builds and ships for it |
| Flash | 16 MB, QIO, 80 MHz | high; reference build and flasher |
| PSRAM | octal, 80 MHz | high; reference memory configuration |
| Panel | 800×480 RGB | high; reference board contract |
| Touch | GT911 over I2C0 at 400 kHz | high; reference board contract |
| USB bridge | CH340-family | medium; reference flasher notes |

## RGB panel

The interface uses DE mode: HSYNC and VSYNC GPIOs are not assigned.

| Signal | GPIO |
|---|---:|
| PCLK | 5 |
| DE | 38 |
| RESET | 46 |
| BACKLIGHT PWM | 21 |
| R3–R7 | 6, 7, 8, 9, 10 |
| G2–G7 | 11, 12, 13, 14, 15, 16 |
| B3–B7 | 17, 18, 48, 47, 39 |

Timing baseline: 14.8 MHz pixel clock; horizontal pulse/back/front `4/16/16`;
vertical pulse/back/front `4/32/32`; rising-edge sampling.

## Touch

| Signal | GPIO |
|---|---:|
| SDA | 2 |
| SCL | 1 |
| IRQ | 40 |
| RESET | 41 |

The reference uses the GT911 alternate address and inverted rotation. Both must be
verified by a touch-grid HIL test rather than adopted invisibly.

## Known traps to turn into tests

- A 12 MHz pixel clock reportedly produced a white screen; start at 14.8 MHz.
- RGB DMA can underflow or horizontally shift when PSRAM and Wi-Fi compete.
- The successful reference uses a 20-line internal-SRAM bounce buffer while keeping
  LVGL partial buffers in PSRAM.
- Wi-Fi power saving reportedly stalls the RGB panel. Validate with sustained traffic
  before deciding whether disabling modem sleep is required permanently.
- The panel expects red/blue-swapped RGB565 words.
- Backlight is 30 kHz PWM and reportedly needs a robust wake/reassert path.
- Never infer a successful boot from serial alone: exercise solid colors, checkerboard,
  motion, sustained HTTP traffic, and the entire touch grid.

## Unknowns requiring our hardware

- Exact panel controller identity (the reference discusses ST7701S, but the RGB bus
  initialization shown does not prove the controller or its startup sequence).
- PSRAM capacity and module SKU.
- Factory partition layout, secure-boot/flash-encryption state, and recovery mechanism.
- Battery, charging, speaker/buzzer, buttons, SD, and any unused peripheral wiring.
- Whether K-Touch and PandaTouch revisions are electrically identical.
