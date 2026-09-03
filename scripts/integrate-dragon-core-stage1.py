#!/usr/bin/env python3

from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path.cwd().resolve()
CORE = ROOT.parent / "dragon-core"

MANIFEST = ROOT / "main/idf_component.yml"
CMAKE = ROOT / "main/CMakeLists.txt"
APP = ROOT / "main/app_main.c"
RUNTIME_C = ROOT / "main/dt_runtime.c"
RUNTIME_H = ROOT / "main/dt_runtime.h"
UI = ROOT / "components/dt_ui/dt_ui.c"

if not (ROOT / "components/dt_board").is_dir():
    sys.exit("ERROR: run from the DragonTouch repository root")

if not CORE.is_dir():
    sys.exit(
        f"ERROR: sibling dragon-core checkout missing: {CORE}"
    )

for p in (MANIFEST, CMAKE, APP, UI):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

# ------------------------------------------------------------
# Backup
# ------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = ROOT / "backups" / f"pre-dragon-core-stage1-{stamp}"

for p in (MANIFEST, CMAKE, APP, UI):
    dst = backup / p.relative_to(ROOT)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

for p in (RUNTIME_C, RUNTIME_H):
    if p.exists():
        dst = backup / p.relative_to(ROOT)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst)

print(f"Backup: {backup}")

# ------------------------------------------------------------
# Discover local dragon-core dependencies recursively.
#
# This keeps all selected dc_* components on the SAME sibling
# checkout instead of mixing local code with managed versions.
# ------------------------------------------------------------

def discover_core_deps(seeds):
    found = set()
    pending = list(seeds)

    while pending:
        name = pending.pop()

        if name in found:
            continue

        comp = CORE / "components" / name

        if not comp.is_dir():
            sys.exit(
                f"ERROR: required dragon-core component missing: {comp}"
            )

        found.add(name)

        text = ""

        for f in (
            comp / "CMakeLists.txt",
            comp / "idf_component.yml"
        ):
            if f.exists():
                text += "\n" + f.read_text()

        for dep in re.findall(
            r"\bdc_[a-z0-9_]+\b",
            text
        ):
            if (
                dep not in found and
                (CORE / "components" / dep).is_dir()
            ):
                pending.append(dep)

    return sorted(found)


core_deps = discover_core_deps(
    ["dc_wifi", "dc_moonraker"]
)

print(
    "dragon-core components:",
    ", ".join(core_deps)
)

# ------------------------------------------------------------
# main/idf_component.yml
# ------------------------------------------------------------

manifest = MANIFEST.read_text()

# Remove existing definitions for components we're replacing
# with local sibling-path dependencies.
for dep in core_deps:
    manifest = re.sub(
        rf"(?ms)^  {re.escape(dep)}:\n"
        rf"(?:    .*\n)*",
        "",
        manifest
    )

if not re.search(
    r"(?m)^dependencies:\s*$",
    manifest
):
    manifest = "dependencies:\n" + manifest

if not manifest.endswith("\n"):
    manifest += "\n"

for dep in core_deps:
    manifest += (
        f"  {dep}:\n"
        f"    path: ../../dragon-core/components/{dep}\n"
    )

MANIFEST.write_text(manifest)

# ------------------------------------------------------------
# main/CMakeLists.txt
# ------------------------------------------------------------

cmake = CMAKE.read_text()

if "dt_runtime.c" not in cmake:
    cmake = re.sub(
        r'(SRCS\s+"app_main\.c")',
        r'\1 "dt_runtime.c"',
        cmake,
        count=1
    )

required = [
    "dt_board",
    "dt_ui",
    "esp_lvgl_port",
    "nvs_flash",
    "esp_wifi",
    "dc_wifi",
    "dc_moonraker",
]

m = re.search(
    r"REQUIRES(?P<body>[^)]*)",
    cmake,
    re.S
)

if not m:
    sys.exit(
        "ERROR: couldn't find REQUIRES in main/CMakeLists.txt"
    )

body = m.group("body")
tokens = set(
    re.findall(
        r"[A-Za-z0-9_]+",
        body
    )
)

for dep in required:
    if dep not in tokens:
        body += f" {dep}"

cmake = (
    cmake[:m.start("body")] +
    body +
    cmake[m.end("body"):]
)

CMAKE.write_text(cmake)

# ------------------------------------------------------------
# Make dt_ui explicitly support unavailable telemetry.
#
# dc_moonraker's current shared status API is known to expose
# bed state and print state. We do NOT invent nozzle/fan values.
# NAN / 255 mean "not yet exposed by the backend".
# ------------------------------------------------------------

ui = UI.read_text()

if '#include <math.h>' not in ui:
    ui = ui.replace(
        '#include <stdio.h>\n',
        '#include <stdio.h>\n'
        '#include <math.h>\n',
        1
    )

old = '''    if (model->connection == DT_UI_CONNECTION_ONLINE) {
        format_temperature(temperature, sizeof(temperature),
                           model->nozzle_c, model->nozzle_target_c);
        lv_label_set_text(s_ui.nozzle_text, temperature);
        format_temperature(temperature, sizeof(temperature), model->bed_c, model->bed_target_c);
        lv_label_set_text(s_ui.bed_text, temperature);
        lv_label_set_text_fmt(s_ui.fan_text, "%u%%", model->fan_percent);
    } else {
        lv_label_set_text(s_ui.nozzle_text, "Unavailable");
        lv_label_set_text(s_ui.bed_text, "Unavailable");
        lv_label_set_text(s_ui.fan_text, "Unavailable");
    }'''

new = '''    if (model->connection == DT_UI_CONNECTION_ONLINE) {
        if (isfinite(model->nozzle_c) &&
            isfinite(model->nozzle_target_c)) {
            format_temperature(
                temperature,
                sizeof(temperature),
                model->nozzle_c,
                model->nozzle_target_c
            );
            lv_label_set_text(
                s_ui.nozzle_text,
                temperature
            );
        } else {
            lv_label_set_text(
                s_ui.nozzle_text,
                "Unavailable"
            );
        }

        if (isfinite(model->bed_c) &&
            isfinite(model->bed_target_c)) {
            format_temperature(
                temperature,
                sizeof(temperature),
                model->bed_c,
                model->bed_target_c
            );
            lv_label_set_text(
                s_ui.bed_text,
                temperature
            );
        } else {
            lv_label_set_text(
                s_ui.bed_text,
                "Unavailable"
            );
        }

        if (model->fan_percent <= 100) {
            lv_label_set_text_fmt(
                s_ui.fan_text,
                "%u%%",
                model->fan_percent
            );
        } else {
            lv_label_set_text(
                s_ui.fan_text,
                "Unavailable"
            );
        }
    } else {
        lv_label_set_text(
            s_ui.nozzle_text,
            "Unavailable"
        );
        lv_label_set_text(
            s_ui.bed_text,
            "Unavailable"
        );
        lv_label_set_text(
            s_ui.fan_text,
            "Unavailable"
        );
    }'''

if old in ui:
    ui = ui.replace(
        old,
        new,
        1
    )
elif "isfinite(model->nozzle_c)" not in ui:
    sys.exit(
        "ERROR: dt_ui_update telemetry block differs from "
        "the expected source. Nothing further modified."
    )

UI.write_text(ui)

# ------------------------------------------------------------
# Runtime API
# ------------------------------------------------------------

RUNTIME_H.write_text(
r'''#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dt_runtime_start(void);

#ifdef __cplusplus
}
#endif
'''
)

RUNTIME_C.write_text(
r'''#include "dt_runtime.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dc_moonraker.h"
#include "dc_wifi.h"

#include "dt_ui.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"

static const char *TAG = "dt_runtime";

#define DT_RUNTIME_PERIOD_MS 500

typedef struct {
    dt_ui_connection_t connection;
    dt_ui_job_state_t job;

    float bed_c;
    float bed_target_c;

    char printer_name[64];
} dt_runtime_snapshot_t;

static bool s_moonraker_started;
static bool s_have_previous;

static dt_runtime_snapshot_t s_previous;
static char s_printer_name[64] = "Klipper";


static void load_printer_name(void)
{
    nvs_handle_t handle;

    if (
        nvs_open(
            "app_nvs",
            NVS_READONLY,
            &handle
        ) != ESP_OK
    ) {
        return;
    }

    size_t size = sizeof(s_printer_name);

    if (
        nvs_get_str(
            handle,
            "mk_host",
            s_printer_name,
            &size
        ) != ESP_OK ||
        s_printer_name[0] == '\0'
    ) {
        snprintf(
            s_printer_name,
            sizeof(s_printer_name),
            "Klipper"
        );
    }

    nvs_close(handle);
}


static bool snapshot_changed(
    const dt_runtime_snapshot_t *a,
    const dt_runtime_snapshot_t *b
)
{
    if (a->connection != b->connection) {
        return true;
    }

    if (a->job != b->job) {
        return true;
    }

    if (strcmp(
        a->printer_name,
        b->printer_name
    ) != 0) {
        return true;
    }

    if (
        fabsf(a->bed_c - b->bed_c) >= 0.05f ||
        fabsf(
            a->bed_target_c -
            b->bed_target_c
        ) >= 0.05f
    ) {
        return true;
    }

    return false;
}


static void push_ui(
    const dt_runtime_snapshot_t *snap
)
{
    dt_ui_model_t model = {
        .device_name = snap->printer_name,

        .connection = snap->connection,
        .job_state = snap->job,

        /*
         * Stage 1 deliberately uses only fields already proven
         * present in dc_moonraker's public status API.
         */
        .filename = "",
        .progress_percent = 0,

        .elapsed_seconds = 0,
        .remaining_seconds = 0,

        .nozzle_c = NAN,
        .nozzle_target_c = NAN,

        .bed_c = snap->bed_c,
        .bed_target_c = snap->bed_target_c,

        /*
         * 255 is our explicit "not currently exposed" sentinel.
         */
        .fan_percent = 255,

        /*
         * Machine-affecting actions remain disabled until the
         * command path is explicitly wired.
         */
        .can_pause = false,
        .can_resume = false,
        .can_cancel = false,
    };

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(
            TAG,
            "LVGL lock timeout"
        );
        return;
    }

    esp_err_t err =
        dt_ui_update(&model);

    lvgl_port_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dt_ui_update: %s",
            esp_err_to_name(err)
        );
    }
}


static void runtime_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "runtime task started on CPU%d",
        xPortGetCoreID()
    );

    for (;;) {
        dc_moonraker_status_t status = {0};

        if (s_moonraker_started) {
            dc_moonraker_get_status(
                &status
            );
        }

        dt_runtime_snapshot_t snap = {0};

        snprintf(
            snap.printer_name,
            sizeof(snap.printer_name),
            "%s",
            s_printer_name
        );

        if (!s_moonraker_started) {
            snap.connection =
                DT_UI_CONNECTION_OFFLINE;

            snap.job =
                DT_UI_JOB_IDLE;

            snap.bed_c = NAN;
            snap.bed_target_c = NAN;

        } else if (
            status.state ==
            DC_MK_SUBSCRIBED
        ) {
            snap.connection =
                DT_UI_CONNECTION_ONLINE;

            /*
             * dc_moonraker_status_t::printing is already used
             * by DragonBreath and therefore part of the proven
             * shared-core status contract.
             */
            snap.job =
                status.printing
                    ? DT_UI_JOB_PRINTING
                    : DT_UI_JOB_IDLE;

            snap.bed_c =
                status.bed_temp;

            snap.bed_target_c =
                status.bed_target;

        } else {
            snap.connection =
                DT_UI_CONNECTION_CONNECTING;

            snap.job =
                DT_UI_JOB_IDLE;

            snap.bed_c = NAN;
            snap.bed_target_c = NAN;
        }

        if (
            !s_have_previous ||
            snapshot_changed(
                &snap,
                &s_previous
            )
        ) {
            push_ui(&snap);

            s_previous = snap;
            s_have_previous = true;

            ESP_LOGI(
                TAG,
                "state conn=%d print=%d bed=%.1f/%.1f",
                (int)snap.connection,
                (int)snap.job,
                snap.bed_c,
                snap.bed_target_c
            );
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                DT_RUNTIME_PERIOD_MS
            )
        );
    }
}


esp_err_t dt_runtime_start(void)
{
    load_printer_name();

    const dc_wifi_identity_t identity = {
        .hostname = "dragontouch",
        .instance_name = "DragonTouch",
        .ap_ssid_prefix = "DragonTouch_",
        .ap_password =
            DC_WIFI_DEFAULT_AP_PASSWORD,
    };

    esp_err_t err =
        dc_wifi_set_identity(
            &identity
        );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dc_wifi_set_identity: %s",
            esp_err_to_name(err)
        );
    }

    err = dc_wifi_start();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "dc_wifi_start: %s; "
            "local UI remains available",
            esp_err_to_name(err)
        );
    } else {
        /*
         * Wall-powered console: favor deterministic network
         * latency over modem power saving.
         *
         * This also finally exercises the deferred
         * RGB-under-Wi-Fi contention gate.
         */
        (void)esp_wifi_set_ps(
            WIFI_PS_NONE
        );
    }

    err = dc_moonraker_start();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dc_moonraker_start: %s",
            esp_err_to_name(err)
        );

        s_moonraker_started = false;
    } else {
        s_moonraker_started = true;
    }

    BaseType_t result =
        xTaskCreatePinnedToCore(
            runtime_task,
            "dt_runtime",
            6144,
            NULL,
            4,
            NULL,
            1
        );

    if (result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
'''
)

# ------------------------------------------------------------
# Production-shaped app boot:
#
# 1. NVS
# 2. proven local display/touch UI
# 3. network/runtime best-effort
#
# A broken network config must never prevent the local console
# itself from starting.
# ------------------------------------------------------------

APP.write_text(
r'''#include <inttypes.h>

#include "dt_board.h"
#include "dt_runtime.h"
#include "dt_ui.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "nvs_flash.h"

static const char *TAG = "dragon_touch";


static void init_nvs(void)
{
    esp_err_t err =
        nvs_flash_init();

    if (
        err ==
            ESP_ERR_NVS_NO_FREE_PAGES ||
        err ==
            ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ESP_ERROR_CHECK(
            nvs_flash_init()
        );

    } else {
        ESP_ERROR_CHECK(err);
    }
}


void app_main(void)
{
    esp_chip_info_t chip = {0};
    uint32_t flash_bytes = 0;

    esp_chip_info(&chip);

    ESP_ERROR_CHECK(
        esp_flash_get_size(
            NULL,
            &flash_bytes
        )
    );

    ESP_LOGI(
        TAG,
        "DragonTouch hardware UI boot"
    );

    ESP_LOGI(
        TAG,
        "chip cores=%u revision=%u "
        "flash=%" PRIu32 " bytes",
        chip.cores,
        chip.revision,
        flash_bytes
    );

    ESP_LOGI(
        TAG,
        "PSRAM=%u bytes; "
        "internal heap=%u bytes",
        (unsigned)
            esp_psram_get_size(),
        (unsigned)
            heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL
            )
    );

    init_nvs();

    lv_display_t *display = NULL;

    ESP_ERROR_CHECK(
        dt_board_lvgl_init(
            &display
        )
    );

    ESP_ERROR_CHECK(
        dt_ui_create(
            display
        )
    );

    ESP_ERROR_CHECK(
        dt_board_lvgl_start()
    );

    ESP_LOGI(
        TAG,
        "DragonTouch UI running"
    );

    /*
     * Network/runtime starts only after the local console is
     * healthy. Network failure is non-fatal.
     */
    esp_err_t runtime_err =
        dt_runtime_start();

    if (runtime_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "runtime start failed: %s; "
            "local UI remains available",
            esp_err_to_name(
                runtime_err
            )
        );
    }
}
'''
)

try:
    sha = subprocess.check_output(
        [
            "git",
            "-C",
            str(CORE),
            "rev-parse",
            "HEAD"
        ],
        text=True,
        stderr=subprocess.DEVNULL
    ).strip()
except Exception:
    sha = "<unknown>"

print()
print("DragonTouch runtime Stage 1 installed.")
print(f"dragon-core: {CORE}")
print(f"core commit: {sha}")

print()
print("Changed:")
print("  main/idf_component.yml")
print("  main/CMakeLists.txt")
print("  main/app_main.c")
print("  main/dt_runtime.c")
print("  main/dt_runtime.h")
print("  components/dt_ui/dt_ui.c")

print()
print("UNTOUCHED:")
print("  components/dt_board/")
print("  RGB configuration")
print("  GT911 implementation")
print("  nav icon hit-test fix")
