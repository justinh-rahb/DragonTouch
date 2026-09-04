// SPDX-License-Identifier: MIT
#pragma once
//
// dt_console — a headless "emulated screen" for DragonTouch bring-up boards without a
// panel. It reads the dc_registry device table and presents it two ways: a small
// self-hosted web page (GET / and GET /api/devices) and a periodic console dump. This
// is the same single-pane family view the LVGL panel will render later, minus the
// display driver — it proves discovery + status end to end on real hardware now.
//
// Read-only: it renders discovered devices; it never commands them.

#include "esp_err.h"

// Start the web server + the periodic console dump. Call after dc_registry_start().
esp_err_t dt_console_start(void);
