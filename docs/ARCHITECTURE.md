# Architecture direction

DragonTouch is a same-LAN control surface for Dragon-family devices. It is not a copy
of any printer touchscreen and does not need an embedded browser.

## Proposed layers

```text
LVGL product shell and device pages
        |
Dragon device registry + selected-device state
        |
HTTP/SSE product clients and capability descriptors
        |
dragon-core Wi-Fi, discovery, identity, event log
        |
DragonTouch board support: RGB panel, GT911, backlight
        |
ESP-IDF 5.3+ / ESP32-S3 / PSRAM
```

The UI should discover sibling devices on the same LAN, let the user explicitly pair
them into a group, and render only capabilities each device declares. Each sibling
remains authoritative for its own safety policy and configuration. DragonTouch is a
client and coordinator, never the sole safety controller.

## Near-term boundaries

- Native ESP-IDF first; no Arduino compatibility layer unless a measured blocker
  justifies it.
- LVGL shell development may proceed against a simulator or unbound display contract;
  board integration and enabled machine controls still follow hardware bring-up.
- Prefer HTTP plus SSE for live state; do not introduce WebSockets without a protocol
  that actually requires them.
- Reuse `dragon-core` rather than cloning Wi-Fi, discovery, event-log, or device
  descriptor behavior into this repository.
- Keep board drivers product-local until another product proves they are common.

## Decisions deliberately left open

- Whether discovery belongs in `dragon-core` before DragonTouch consumes it.
- Whether the physical UI reuses the SPA's schema, shares only capability vocabulary,
  or has a separate LVGL view-model contract.
- Exact local provisioning UX and whether DragonTouch also hosts the shared SPA.
- Partition table and OTA/recovery scheme, pending a factory backup.
