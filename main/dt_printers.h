#pragma once

/*
 * DT_PRINTER_LIST
 *
 * A small list of saved Moonraker instances, owned by DragonTouch alone.
 *
 * dragon-core's dc_moonraker stores exactly one {host, port, api_key} in NVS
 * and is shared with the other firmwares built on dragon-core, so its schema is
 * deliberately left untouched. This list lives in DragonTouch's own NVS
 * namespace and uses dc_moonraker_set_config() purely to *apply* whichever
 * entry is selected -- the device stays bound to exactly one printer at a time,
 * which is the invariant dc_source documents.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define DT_PRINTER_MAX 6
#define DT_PRINTER_HOST_MAX 64
#define DT_PRINTER_API_KEY_MAX 65

typedef struct {
    char host[DT_PRINTER_HOST_MAX];
    uint16_t port;
    char api_key[DT_PRINTER_API_KEY_MAX];
} dt_printer_entry_t;

typedef struct {
    uint8_t count;
    dt_printer_entry_t entries[DT_PRINTER_MAX];
} dt_printer_list_t;

/*
 * Reads the saved list. A missing or unreadable blob is not an error -- it
 * yields an empty list, which is the correct state for a fresh device.
 */
esp_err_t dt_printers_load(dt_printer_list_t *out);

/*
 * Adds an entry, or updates the port/API key of one that already matches on
 * host. Returns ESP_ERR_NO_MEM once DT_PRINTER_MAX entries are stored.
 */
esp_err_t dt_printers_add(
    const char *host,
    uint16_t port,
    const char *api_key
);

/*
 * Forgets one entry, shuffling the tail down. The applied config is left
 * alone: removing the entry you are connected to does not disconnect you,
 * it only stops offering it in the picker.
 */
esp_err_t dt_printers_remove(size_t index);

/*
 * Folds whatever dc_moonraker is currently configured with into the list, so
 * the printer already in use appears without the user re-entering it. Safe to
 * call on every boot; it is a no-op once that host is known.
 */
esp_err_t dt_printers_sync_active(void);

/*
 * Copies the currently applied host into out_host (empty string when
 * unconfigured), so the caller can mark which list entry is live.
 */
esp_err_t dt_printers_active_host(char *out_host, size_t length);

/*
 * Applies an entry via dc_moonraker_set_config(). This does NOT restart --
 * the caller decides, because rebinding a running runtime is a separate
 * problem (see dragon_core.md: load_moonraker_config() runs once at init and
 * discovery is not re-entrant yet).
 */
esp_err_t dt_printers_apply(size_t index);
