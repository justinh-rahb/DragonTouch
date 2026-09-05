// SPDX-License-Identifier: MIT
#pragma once
//
// dt_console — a headless "emulated screen" for DragonTouch bring-up boards without a
// panel. It renders the dc_registry device table as a small web page (GET /devices)
// and JSON (GET /api/devices), plus a periodic console dump. This is the same
// single-pane family view the LVGL panel will render later, minus the display driver.
//
// It does NOT own an HTTP server: its routes are handed to dc_portal (which owns port
// 80, the setup SPA, provisioning and OTA), so the console is a normal, provisionable
// Dragon device. Read-only: it renders discovered devices, it never commands them.

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

// Device-list routes to hand to dc_portal via product_routes/product_route_count.
void dt_console_get_routes(const httpd_uri_t **routes, size_t *count);

// Start the periodic console dump of the device table (works with no browser).
esp_err_t dt_console_start_logger(void);
