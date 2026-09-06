/*
 * DT_PRINTER_LIST -- see dt_printers.h for why this lives here rather than in
 * dragon-core.
 */

#include "dt_printers.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "dc_moonraker.h"

static const char *TAG = "dt_printers";

/*
 * Own namespace rather than dragon-core's shared "app_nvs", so this can never
 * collide with a key another product adds there.
 */
#define DT_PRINTERS_NVS_NS "dt_prn"
#define DT_PRINTERS_NVS_KEY "list"

/*
 * The blob is versioned: a future layout change can be detected and discarded
 * instead of being read as garbage.
 */
#define DT_PRINTERS_BLOB_VERSION 1u

typedef struct {
    uint8_t version;
    uint8_t count;
    dt_printer_entry_t entries[DT_PRINTER_MAX];
} dt_printers_blob_t;


/*
 * DT_PRINTERS_FLASH_TASK
 *
 * Writing NVS freezes the flash cache, and ESP-IDF asserts
 * (s_task_stack_is_sane_when_cache_frozen) unless the task performing the
 * write has its stack in internal RAM. Every caller here is on a PSRAM
 * stack -- both the LVGL port task and the runtime worker are created with
 * MALLOC_CAP_SPIRAM -- so a write from either aborts the firmware.
 *
 * Reads are fine; only writes stop the cache. So each mutating operation is
 * marshalled onto a short-lived task with a default (internal) stack and
 * waited on. The WHOLE operation runs there, not just nvs_set_blob(), so the
 * list and blob it builds live on that internal stack too.
 *
 * This is also why the first write of a session is the one that trips:
 * esp_ota_get_running_partition() caches its result, and NVS skips a write
 * whose value is unchanged -- so the boot-time sync is silent and the first
 * genuinely new entry is what aborts.
 */
#define DT_PRINTERS_TASK_STACK 6144
#define DT_PRINTERS_TASK_PRIO 5

typedef struct {
    esp_err_t (*fn)(void *arg);
    void *arg;
    esp_err_t result;
    SemaphoreHandle_t done;
} printers_job_t;

typedef struct {
    const char *host;
    uint16_t port;
    const char *api_key;
} printers_add_args_t;

typedef struct {
    size_t index;
} printers_index_args_t;


static void printers_job_task(void *param)
{
    printers_job_t *job = (printers_job_t *)param;

    job->result = job->fn(job->arg);

    xSemaphoreGive(job->done);

    vTaskDelete(NULL);
}


static esp_err_t printers_run_isolated(
    esp_err_t (*fn)(void *),
    void *arg
)
{
    printers_job_t job = {
        .fn = fn,
        .arg = arg,
        .result = ESP_FAIL,
        .done = xSemaphoreCreateBinary(),
    };

    if (job.done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (
        xTaskCreate(
            printers_job_task,
            "dtp_nvs",
            DT_PRINTERS_TASK_STACK,
            &job,
            DT_PRINTERS_TASK_PRIO,
            NULL
        ) != pdPASS
    ) {
        vSemaphoreDelete(job.done);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);

    return job.result;
}


static esp_err_t printers_save(
    const dt_printer_list_t *list
)
{
    dt_printers_blob_t blob = {0};

    blob.version = DT_PRINTERS_BLOB_VERSION;

    blob.count =
        list->count > DT_PRINTER_MAX
            ? DT_PRINTER_MAX
            : list->count;

    memcpy(
        blob.entries,
        list->entries,
        sizeof(blob.entries)
    );

    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            DT_PRINTERS_NVS_NS,
            NVS_READWRITE,
            &handle
        );

    if (err != ESP_OK) {
        return err;
    }

    err =
        nvs_set_blob(
            handle,
            DT_PRINTERS_NVS_KEY,
            &blob,
            sizeof(blob)
        );

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    return err;
}


esp_err_t dt_printers_load(
    dt_printer_list_t *out
)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;

    esp_err_t err =
        nvs_open(
            DT_PRINTERS_NVS_NS,
            NVS_READONLY,
            &handle
        );

    if (err != ESP_OK) {
        /* No namespace yet: an empty list, not a failure. */
        return ESP_OK;
    }

    dt_printers_blob_t blob = {0};
    size_t length = sizeof(blob);

    err =
        nvs_get_blob(
            handle,
            DT_PRINTERS_NVS_KEY,
            &blob,
            &length
        );

    nvs_close(handle);

    if (
        err != ESP_OK ||
        length != sizeof(blob) ||
        blob.version != DT_PRINTERS_BLOB_VERSION
    ) {
        if (err == ESP_OK) {
            ESP_LOGW(
                TAG,
                "discarding printer list: version %u length %u",
                (unsigned)blob.version,
                (unsigned)length
            );
        }

        return ESP_OK;
    }

    out->count =
        blob.count > DT_PRINTER_MAX
            ? DT_PRINTER_MAX
            : blob.count;

    memcpy(
        out->entries,
        blob.entries,
        sizeof(out->entries)
    );

    return ESP_OK;
}


static esp_err_t printers_add_impl(void *arg)
{
    const printers_add_args_t *args =
        (const printers_add_args_t *)arg;

    const char *host = args->host;
    uint16_t port = args->port;
    const char *api_key = args->api_key;

    if (host == NULL || host[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    dt_printer_list_t list;

    esp_err_t err = dt_printers_load(&list);

    if (err != ESP_OK) {
        return err;
    }

    if (port == 0) {
        port = 7125;
    }

    /* Same host: update in place rather than accumulating duplicates. */
    for (size_t i = 0; i < list.count; ++i) {
        if (strcmp(list.entries[i].host, host) != 0) {
            continue;
        }

        list.entries[i].port = port;

        snprintf(
            list.entries[i].api_key,
            sizeof(list.entries[i].api_key),
            "%s",
            api_key != NULL ? api_key : ""
        );

        return printers_save(&list);
    }

    if (list.count >= DT_PRINTER_MAX) {
        return ESP_ERR_NO_MEM;
    }

    dt_printer_entry_t *entry =
        &list.entries[list.count];

    snprintf(
        entry->host,
        sizeof(entry->host),
        "%s",
        host
    );

    entry->port = port;

    snprintf(
        entry->api_key,
        sizeof(entry->api_key),
        "%s",
        api_key != NULL ? api_key : ""
    );

    list.count++;

    ESP_LOGI(
        TAG,
        "remembered printer %s:%u (%u saved)",
        entry->host,
        (unsigned)entry->port,
        (unsigned)list.count
    );

    return printers_save(&list);
}


static esp_err_t printers_remove_impl(void *arg)
{
    const size_t index =
        ((const printers_index_args_t *)arg)->index;

    dt_printer_list_t list;

    esp_err_t err = dt_printers_load(&list);

    if (err != ESP_OK) {
        return err;
    }

    if (index >= list.count) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(
        TAG,
        "forgetting printer %s",
        list.entries[index].host
    );

    for (size_t i = index + 1; i < list.count; ++i) {
        list.entries[i - 1] = list.entries[i];
    }

    list.count--;

    memset(
        &list.entries[list.count],
        0,
        sizeof(list.entries[list.count])
    );

    return printers_save(&list);
}


esp_err_t dt_printers_sync_active(void)
{
    dc_moonraker_config_t cfg = {0};

    if (
        dc_moonraker_get_config(&cfg) != ESP_OK ||
        cfg.host[0] == '\0'
    ) {
        return ESP_OK;
    }

    return dt_printers_add(
        cfg.host,
        cfg.port,
        cfg.api_key
    );
}


esp_err_t dt_printers_active_host(
    char *out_host,
    size_t length
)
{
    if (out_host == NULL || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out_host[0] = '\0';

    dc_moonraker_config_t cfg = {0};

    if (dc_moonraker_get_config(&cfg) != ESP_OK) {
        return ESP_OK;
    }

    snprintf(out_host, length, "%s", cfg.host);

    return ESP_OK;
}


static esp_err_t printers_apply_impl(void *arg)
{
    const size_t index =
        ((const printers_index_args_t *)arg)->index;

    dt_printer_list_t list;

    esp_err_t err = dt_printers_load(&list);

    if (err != ESP_OK) {
        return err;
    }

    if (index >= list.count) {
        return ESP_ERR_INVALID_ARG;
    }

    const dt_printer_entry_t *entry =
        &list.entries[index];

    dc_moonraker_config_t cfg = {0};

    snprintf(
        cfg.host,
        sizeof(cfg.host),
        "%s",
        entry->host
    );

    cfg.port =
        entry->port != 0
            ? entry->port
            : 7125;

    snprintf(
        cfg.api_key,
        sizeof(cfg.api_key),
        "%s",
        entry->api_key
    );

    ESP_LOGI(
        TAG,
        "applying printer %s:%u",
        cfg.host,
        (unsigned)cfg.port
    );

    return dc_moonraker_set_config(&cfg);
}


/*
 * DT_PRINTERS_FLASH_TASK
 *
 * Public entry points: every one of these mutates NVS, so each hands off to
 * an internal-stack task rather than writing from the caller's PSRAM stack.
 */
esp_err_t dt_printers_add(
    const char *host,
    uint16_t port,
    const char *api_key
)
{
    printers_add_args_t args = {
        .host = host,
        .port = port,
        .api_key = api_key,
    };

    return printers_run_isolated(printers_add_impl, &args);
}


esp_err_t dt_printers_remove(size_t index)
{
    printers_index_args_t args = { .index = index };

    return printers_run_isolated(printers_remove_impl, &args);
}


esp_err_t dt_printers_apply(size_t index)
{
    printers_index_args_t args = { .index = index };

    return printers_run_isolated(printers_apply_impl, &args);
}
