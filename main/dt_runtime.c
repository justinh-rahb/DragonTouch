#include "dt_runtime.h"
#include "dt_portal.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "dc_moonraker.h"
#include "dc_wifi.h"

#include "dt_ui.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "nvs.h"


static const char *TAG = "dt_runtime";


/*
 * RUNTIME_HEAP_DIAG
 * Temporary Stage 3 allocator diagnostics.
 */
static void runtime_heap_diag(const char *where)
{
    const size_t free_internal =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    const size_t largest_internal =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    const size_t free_dma =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_DMA
        );

    const size_t largest_dma =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_DMA
        );

    ESP_LOGI(
        TAG,
        "RUNTIME_HEAP %s internal=%u largest=%u dma=%u dma_largest=%u psram=%u psram_largest=%u",
        where,
        (unsigned)free_internal,
        (unsigned)largest_internal,
        (unsigned)free_dma,
        (unsigned)largest_dma,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
    );
}

#define DT_STATUS_PERIOD_MS   1000
#define DT_HTTP_TIMEOUT_MS    3500
#define DT_HTTP_RESPONSE_MAX  12288
#define DT_ACTION_QUEUE_LEN   12


typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buffer_t;


typedef struct {
    char device_name[128];
    char filename[192];

    dt_ui_connection_t connection;
    dt_ui_job_state_t job;

    uint8_t progress_percent;
    uint8_t fan_percent;

    uint32_t elapsed_seconds;
    uint32_t remaining_seconds;

    float nozzle_c;
    float nozzle_target_c;
    float bed_c;
    float bed_target_c;

    float x;
    float y;
    float z;

    bool homed_x;
    bool homed_y;
    bool homed_z;

    bool can_extrude;
} runtime_snapshot_t;


static QueueHandle_t s_action_queue;

static char s_moonraker_host[128];
static char s_base_url[160];
static char s_api_key[160];

static bool s_have_previous;
static runtime_snapshot_t s_previous;


static esp_err_t http_event(
    esp_http_client_event_t *event
)
{
    if (
        event->event_id !=
        HTTP_EVENT_ON_DATA
    ) {
        return ESP_OK;
    }

    http_buffer_t *buffer =
        event->user_data;

    if (
        buffer == NULL ||
        event->data_len <= 0
    ) {
        return ESP_OK;
    }

    if (
        buffer->len +
        event->data_len + 1 >
        buffer->cap
    ) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(
        buffer->data + buffer->len,
        event->data,
        event->data_len
    );

    buffer->len +=
        event->data_len;

    buffer->data[buffer->len] =
        '\0';

    return ESP_OK;
}


static bool load_nvs_string(
    const char *key,
    char *out,
    size_t out_len
)
{
    if (out_len == 0) {
        return false;
    }

    out[0] = '\0';

    nvs_handle_t handle;

    if (
        nvs_open(
            "app_nvs",
            NVS_READONLY,
            &handle
        ) != ESP_OK
    ) {
        return false;
    }

    size_t needed = out_len;

    esp_err_t err =
        nvs_get_str(
            handle,
            key,
            out,
            &needed
        );

    nvs_close(handle);

    if (
        err != ESP_OK ||
        out[0] == '\0'
    ) {
        out[0] = '\0';
        return false;
    }

    return true;
}


static void load_moonraker_config(void)
{
    dc_moonraker_config_t cfg = {0};

    esp_err_t err =
        dc_moonraker_get_config(
            &cfg
        );

    if (
        err != ESP_OK ||
        cfg.host[0] == '\0'
    ) {
        s_moonraker_host[0] = '\0';
        s_base_url[0] = '\0';
        s_api_key[0] = '\0';
        return;
    }

    if (cfg.port == 0) {
        cfg.port = 7125;
    }

    snprintf(
        s_moonraker_host,
        sizeof(s_moonraker_host),
        "%s",
        cfg.host
    );

    snprintf(
        s_api_key,
        sizeof(s_api_key),
        "%s",
        cfg.api_key
    );

    snprintf(
        s_base_url,
        sizeof(s_base_url),
        "http://%s:%u",
        cfg.host,
        (unsigned)cfg.port
    );
}


static esp_err_t http_request(
    esp_http_client_method_t method,
    const char *path,
    const char *json_body,
    char **response_out,
    int *http_status_out
)
{
    if (response_out != NULL) {
        *response_out = NULL;
    }

    if (
        s_base_url[0] == '\0' ||
        path == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    char *url =
        heap_caps_malloc(
            768,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    char *response =
        heap_caps_malloc(
            DT_HTTP_RESPONSE_MAX,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (
        url == NULL ||
        response == NULL
    ) {
        free(url);
        free(response);
        return ESP_ERR_NO_MEM;
    }

    snprintf(
        url,
        768,
        "%s%s",
        s_base_url,
        path
    );

    http_buffer_t buffer = {
        .data = response,
        .len = 0,
        .cap = DT_HTTP_RESPONSE_MAX,
    };

    response[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &buffer,
        .timeout_ms = DT_HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        free(url);
        free(response);
        return ESP_FAIL;
    }

    esp_http_client_set_method(
        client,
        method
    );

    if (s_api_key[0] != '\0') {
        esp_http_client_set_header(
            client,
            "X-Api-Key",
            s_api_key
        );
    }

    if (json_body != NULL) {
        esp_http_client_set_header(
            client,
            "Content-Type",
            "application/json"
        );

        esp_http_client_set_post_field(
            client,
            json_body,
            strlen(json_body)
        );
    }

    esp_err_t err =
        esp_http_client_perform(
            client
        );

    int status = -1;

    if (err == ESP_OK) {
        status =
            esp_http_client_get_status_code(
                client
            );

        if (
            status < 200 ||
            status >= 300
        ) {
            err = ESP_FAIL;
        }
    }

    if (http_status_out != NULL) {
        *http_status_out =
            status;
    }

    esp_http_client_cleanup(
        client
    );

    free(url);

    if (err != ESP_OK) {
        free(response);
        return err;
    }

    if (response_out != NULL) {
        *response_out =
            response;
    } else {
        free(response);
    }

    return ESP_OK;
}


static cJSON *moonraker_payload(
    cJSON *root
)
{
    if (root == NULL) {
        return NULL;
    }

    cJSON *result =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "result"
        );

    return result != NULL
        ? result
        : root;
}


static bool json_number(
    cJSON *object,
    const char *name,
    float *value
)
{
    if (
        object == NULL ||
        name == NULL ||
        value == NULL
    ) {
        return false;
    }

    cJSON *item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            name
        );

    if (!cJSON_IsNumber(item)) {
        return false;
    }

    *value =
        (float)item->valuedouble;

    return true;
}


static bool query_status(
    runtime_snapshot_t *snap
)
{
    memset(
        snap,
        0,
        sizeof(*snap)
    );

    snap->nozzle_c = NAN;
    snap->nozzle_target_c = NAN;
    snap->bed_c = NAN;
    snap->bed_target_c = NAN;
    snap->fan_percent = 255;

    snprintf(
        snap->device_name,
        sizeof(snap->device_name),
        "%s",
        s_moonraker_host[0] != '\0'
            ? s_moonraker_host
            : "Klipper"
    );

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_GET,
            "/printer/objects/query?"
            "webhooks=state,message&"
            "print_stats=filename,state,print_duration,total_duration,message&"
            "virtual_sdcard=progress,is_active&"
            "extruder=temperature,target,can_extrude&"
            "heater_bed=temperature,target&"
            "fan=speed&"
            "toolhead=position,homed_axes",
            NULL,
            &response,
            NULL
        );

    if (err != ESP_OK) {
        snap->connection =
            s_base_url[0] != '\0'
                ? DT_UI_CONNECTION_CONNECTING
                : DT_UI_CONNECTION_OFFLINE;

        snap->job =
            DT_UI_JOB_IDLE;

        return false;
    }

    cJSON *root =
        cJSON_Parse(response);

    free(response);

    if (root == NULL) {
        snap->connection =
            DT_UI_CONNECTION_CONNECTING;
        return false;
    }

    cJSON *payload =
        moonraker_payload(root);

    cJSON *status =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "status"
        );

    if (!cJSON_IsObject(status)) {
        /*
         * Some Moonraker HTTP responses are already the status payload.
         */
        status = payload;
    }

    cJSON *webhooks =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "webhooks"
        );

    const cJSON *web_state =
        cJSON_GetObjectItemCaseSensitive(
            webhooks,
            "state"
        );

    const bool ready =
        cJSON_IsString(web_state) &&
        strcmp(
            web_state->valuestring,
            "ready"
        ) == 0;

    snap->connection =
        ready
            ? DT_UI_CONNECTION_ONLINE
            : DT_UI_CONNECTION_CONNECTING;

    cJSON *print_stats =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "print_stats"
        );

    cJSON *filename =
        cJSON_GetObjectItemCaseSensitive(
            print_stats,
            "filename"
        );

    if (
        cJSON_IsString(filename) &&
        filename->valuestring != NULL
    ) {
        snprintf(
            snap->filename,
            sizeof(snap->filename),
            "%s",
            filename->valuestring
        );
    }

    cJSON *print_state =
        cJSON_GetObjectItemCaseSensitive(
            print_stats,
            "state"
        );

    const char *state =
        cJSON_IsString(print_state)
            ? print_state->valuestring
            : "standby";

    if (strcmp(state, "printing") == 0) {
        snap->job =
            DT_UI_JOB_PRINTING;
    } else if (
        strcmp(state, "paused") == 0
    ) {
        snap->job =
            DT_UI_JOB_PAUSED;
    } else if (
        strcmp(state, "complete") == 0
    ) {
        snap->job =
            DT_UI_JOB_COMPLETE;
    } else if (
        strcmp(state, "error") == 0
    ) {
        snap->job =
            DT_UI_JOB_ERROR;
    } else {
        snap->job =
            DT_UI_JOB_IDLE;
    }

    float elapsed = 0.0f;

    json_number(
        print_stats,
        "print_duration",
        &elapsed
    );

    snap->elapsed_seconds =
        elapsed > 0.0f
            ? (uint32_t)elapsed
            : 0;

    cJSON *vsd =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "virtual_sdcard"
        );

    float progress = 0.0f;

    if (
        json_number(
            vsd,
            "progress",
            &progress
        )
    ) {
        if (progress < 0.0f) {
            progress = 0.0f;
        }

        if (progress > 1.0f) {
            progress = 1.0f;
        }

        snap->progress_percent =
            (uint8_t)(
                progress * 100.0f +
                0.5f
            );

        if (
            progress > 0.01f &&
            progress < 0.999f &&
            elapsed > 0.0f
        ) {
            float total_estimate =
                elapsed / progress;

            float remaining =
                total_estimate -
                elapsed;

            if (remaining > 0.0f) {
                snap->remaining_seconds =
                    (uint32_t)remaining;
            }
        }
    }

    cJSON *extruder =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "extruder"
        );

    json_number(
        extruder,
        "temperature",
        &snap->nozzle_c
    );

    json_number(
        extruder,
        "target",
        &snap->nozzle_target_c
    );

    cJSON *can_extrude =
        cJSON_GetObjectItemCaseSensitive(
            extruder,
            "can_extrude"
        );

    snap->can_extrude =
        cJSON_IsTrue(can_extrude);

    cJSON *bed =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "heater_bed"
        );

    json_number(
        bed,
        "temperature",
        &snap->bed_c
    );

    json_number(
        bed,
        "target",
        &snap->bed_target_c
    );

    cJSON *fan =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "fan"
        );

    float fan_speed = NAN;

    if (
        json_number(
            fan,
            "speed",
            &fan_speed
        ) &&
        isfinite(fan_speed)
    ) {
        if (fan_speed < 0.0f) {
            fan_speed = 0.0f;
        }

        if (fan_speed > 1.0f) {
            fan_speed = 1.0f;
        }

        snap->fan_percent =
            (uint8_t)(
                fan_speed * 100.0f +
                0.5f
            );
    }

    cJSON *toolhead =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "toolhead"
        );

    cJSON *position =
        cJSON_GetObjectItemCaseSensitive(
            toolhead,
            "position"
        );

    if (
        cJSON_IsArray(position) &&
        cJSON_GetArraySize(position) >= 3
    ) {
        cJSON *px =
            cJSON_GetArrayItem(
                position,
                0
            );

        cJSON *py =
            cJSON_GetArrayItem(
                position,
                1
            );

        cJSON *pz =
            cJSON_GetArrayItem(
                position,
                2
            );

        if (cJSON_IsNumber(px)) {
            snap->x =
                (float)px->valuedouble;
        }

        if (cJSON_IsNumber(py)) {
            snap->y =
                (float)py->valuedouble;
        }

        if (cJSON_IsNumber(pz)) {
            snap->z =
                (float)pz->valuedouble;
        }
    }

    cJSON *homed =
        cJSON_GetObjectItemCaseSensitive(
            toolhead,
            "homed_axes"
        );

    if (
        cJSON_IsString(homed) &&
        homed->valuestring != NULL
    ) {
        snap->homed_x =
            strchr(
                homed->valuestring,
                'x'
            ) != NULL;

        snap->homed_y =
            strchr(
                homed->valuestring,
                'y'
            ) != NULL;

        snap->homed_z =
            strchr(
                homed->valuestring,
                'z'
            ) != NULL;
    }

    cJSON_Delete(root);

    return ready;
}


static bool snapshot_equal(
    const runtime_snapshot_t *a,
    const runtime_snapshot_t *b
)
{
    return
        a->connection == b->connection &&
        a->job == b->job &&
        a->progress_percent ==
            b->progress_percent &&
        a->fan_percent ==
            b->fan_percent &&
        a->elapsed_seconds ==
            b->elapsed_seconds &&
        a->remaining_seconds ==
            b->remaining_seconds &&
        fabsf(
            a->nozzle_c -
            b->nozzle_c
        ) < 0.05f &&
        fabsf(
            a->nozzle_target_c -
            b->nozzle_target_c
        ) < 0.05f &&
        fabsf(
            a->bed_c -
            b->bed_c
        ) < 0.05f &&
        fabsf(
            a->bed_target_c -
            b->bed_target_c
        ) < 0.05f &&
        fabsf(a->x - b->x) < 0.05f &&
        fabsf(a->y - b->y) < 0.05f &&
        fabsf(a->z - b->z) < 0.05f &&
        a->homed_x == b->homed_x &&
        a->homed_y == b->homed_y &&
        a->homed_z == b->homed_z &&
        a->can_extrude ==
            b->can_extrude &&
        strcmp(
            a->device_name,
            b->device_name
        ) == 0 &&
        strcmp(
            a->filename,
            b->filename
        ) == 0;
}


static void push_ui(
    const runtime_snapshot_t *snap
)
{
    const bool online =
        snap->connection ==
        DT_UI_CONNECTION_ONLINE;

    const bool printing =
        snap->job ==
        DT_UI_JOB_PRINTING;

    const bool paused =
        snap->job ==
        DT_UI_JOB_PAUSED;

    dt_ui_model_t model = {
        .device_name =
            snap->device_name,

        .connection =
            snap->connection,

        .job_state =
            snap->job,

        .filename =
            snap->filename,

        .progress_percent =
            snap->progress_percent,

        .elapsed_seconds =
            snap->elapsed_seconds,

        .remaining_seconds =
            snap->remaining_seconds,

        .nozzle_c =
            snap->nozzle_c,

        .nozzle_target_c =
            snap->nozzle_target_c,

        .bed_c =
            snap->bed_c,

        .bed_target_c =
            snap->bed_target_c,

        .fan_percent =
            snap->fan_percent,

        .x = snap->x,
        .y = snap->y,
        .z = snap->z,

        .homed_x =
            snap->homed_x,

        .homed_y =
            snap->homed_y,

        .homed_z =
            snap->homed_z,

        .can_pause =
            online && printing,

        .can_resume =
            online && paused,

        .can_cancel =
            online &&
            (printing || paused),

        .can_home = online,

        .can_jog =
            online &&
            snap->homed_x &&
            snap->homed_y &&
            snap->homed_z &&
            !printing,

        .can_heat = online,

        .can_extrude =
            online &&
            snap->can_extrude,

        .can_fan = online,
    };

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(
            TAG,
            "LVGL lock timeout; "
            "skipping one status update"
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


static esp_err_t post_endpoint(
    const char *path
)
{
    return http_request(
        HTTP_METHOD_POST,
        path,
        NULL,
        NULL,
        NULL
    );
}


static esp_err_t run_gcode(
    const char *script
)
{
    cJSON *root =
        cJSON_CreateObject();

    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(
        root,
        "script",
        script
    );

    char *body =
        cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err =
        http_request(
            HTTP_METHOD_POST,
            "/printer/gcode/script",
            body,
            NULL,
            NULL
        );

    cJSON_free(body);

    return err;
}


static esp_err_t execute_action(
    dt_ui_action_t action
)
{
    switch (action) {
    case DT_UI_ACTION_PAUSE:
        return post_endpoint(
            "/printer/print/pause"
        );

    case DT_UI_ACTION_RESUME:
        return post_endpoint(
            "/printer/print/resume"
        );

    case DT_UI_ACTION_CANCEL:
        return post_endpoint(
            "/printer/print/cancel"
        );

    case DT_UI_ACTION_HOME_ALL:
        return run_gcode("G28");

    case DT_UI_ACTION_JOG_X_NEG:
        return run_gcode(
            "G91\n"
            "G1 X-10 F6000\n"
            "G90"
        );

    case DT_UI_ACTION_JOG_X_POS:
        return run_gcode(
            "G91\n"
            "G1 X10 F6000\n"
            "G90"
        );

    case DT_UI_ACTION_JOG_Y_NEG:
        return run_gcode(
            "G91\n"
            "G1 Y-10 F6000\n"
            "G90"
        );

    case DT_UI_ACTION_JOG_Y_POS:
        return run_gcode(
            "G91\n"
            "G1 Y10 F6000\n"
            "G90"
        );

    case DT_UI_ACTION_JOG_Z_NEG:
        return run_gcode(
            "G91\n"
            "G1 Z-1 F600\n"
            "G90"
        );

    case DT_UI_ACTION_JOG_Z_POS:
        return run_gcode(
            "G91\n"
            "G1 Z1 F600\n"
            "G90"
        );

    case DT_UI_ACTION_NOZZLE_220:
        return run_gcode(
            "SET_HEATER_TEMPERATURE "
            "HEATER=extruder TARGET=220"
        );

    case DT_UI_ACTION_BED_OFF:
        return run_gcode(
            "SET_HEATER_TEMPERATURE "
            "HEATER=heater_bed TARGET=0"
        );

    case DT_UI_ACTION_BED_60:
        return run_gcode(
            "SET_HEATER_TEMPERATURE "
            "HEATER=heater_bed TARGET=60"
        );

    case DT_UI_ACTION_BED_100:
        return run_gcode(
            "SET_HEATER_TEMPERATURE "
            "HEATER=heater_bed TARGET=100"
        );

    case DT_UI_ACTION_EXTRUDE_10:
        return run_gcode(
            "M83\n"
            "G1 E10 F300"
        );

    case DT_UI_ACTION_RETRACT_10:
        return run_gcode(
            "M83\n"
            "G1 E-10 F300"
        );

    case DT_UI_ACTION_FAN_OFF:
        return run_gcode(
            "M106 S0"
        );

    case DT_UI_ACTION_FAN_50:
        return run_gcode(
            "M106 S128"
        );

    case DT_UI_ACTION_FAN_100:
        return run_gcode(
            "M106 S255"
        );

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}


static void ui_action_handler(
    dt_ui_action_t action,
    void *ctx
)
{
    (void)ctx;

    if (s_action_queue == NULL) {
        return;
    }

    if (
        xQueueSend(
            s_action_queue,
            &action,
            0
        ) != pdTRUE
    ) {
        ESP_LOGW(
            TAG,
            "command queue full; "
            "dropping action %d",
            (int)action
        );
    }
}


static void runtime_task(void *arg)
{
    (void)arg;

    /* DT_RUNTIME_SINGLE_WORKER */
    ESP_LOGI(
        TAG,
        "runtime worker started on CPU%d",
        xPortGetCoreID()
    );

    unsigned failure_count = 0;
    TickType_t last_status =
        xTaskGetTickCount() -
        pdMS_TO_TICKS(DT_STATUS_PERIOD_MS);

    for (;;) {
        dt_ui_action_t action;

        while (
            xQueueReceive(
                s_action_queue,
                &action,
                0
            ) == pdTRUE
        ) {
            ESP_LOGI(
                TAG,
                "executing UI action %d",
                (int)action
            );

            esp_err_t err =
                execute_action(action);

            if (err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "UI action %d failed: %s",
                    (int)action,
                    esp_err_to_name(err)
                );
            } else {
                ESP_LOGI(
                    TAG,
                    "UI action %d accepted",
                    (int)action
                );
            }
        }

        TickType_t now = xTaskGetTickCount();

        if (
            now - last_status >=
            pdMS_TO_TICKS(DT_STATUS_PERIOD_MS)
        ) {
            last_status = now;

            runtime_snapshot_t snap;
            const bool online = query_status(&snap);

            if (online) {
                failure_count = 0;
            } else {
                failure_count++;
                if (
                    failure_count == 1 ||
                    failure_count % 30 == 0
                ) {
                    ESP_LOGW(
                        TAG,
                        "Moonraker status unavailable (attempt %u)",
                        failure_count
                    );
                }
            }

            if (
                !s_have_previous ||
                !snapshot_equal(&snap, &s_previous)
            ) {
                push_ui(&snap);
                s_previous = snap;
                s_have_previous = true;

                ESP_LOGI(
                    TAG,
                    "state conn=%d job=%d progress=%u "
                    "nozzle=%.1f/%.1f bed=%.1f/%.1f "
                    "xyz=%.1f,%.1f,%.1f",
                    (int)snap.connection,
                    (int)snap.job,
                    (unsigned)snap.progress_percent,
                    snap.nozzle_c,
                    snap.nozzle_target_c,
                    snap.bed_c,
                    snap.bed_target_c,
                    snap.x,
                    snap.y,
                    snap.z
                );
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}






static bool s_network_phase_attempted;
static esp_err_t s_network_phase_result = ESP_FAIL;

esp_err_t dt_runtime_network_start(void)
{
    if (s_network_phase_attempted) {
        return s_network_phase_result;
    }

    s_network_phase_attempted = true;

    const dc_wifi_identity_t identity = {
        .hostname =
            "dragontouch",

        .instance_name =
            "DragonTouch",

        .ap_ssid_prefix =
            "DragonTouch_",

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

    runtime_heap_diag("early-before-wifi");

    err = dc_wifi_start();

    runtime_heap_diag("early-after-wifi");

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "dc_wifi_start: %s",
            esp_err_to_name(err)
        );

        s_network_phase_result = err;
        return err;
    }

    (void)esp_wifi_set_ps(
        WIFI_PS_NONE
    );

    runtime_heap_diag("moonraker-client-skipped");
    runtime_heap_diag("early-before-portal");

    err = dt_portal_start();

    runtime_heap_diag("early-after-portal");

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "dt_portal_start: %s",
            esp_err_to_name(err)
        );

        s_network_phase_result = err;
        return err;
    }

    s_network_phase_result = ESP_OK;
    return ESP_OK;
}


esp_err_t dt_runtime_start(void)
{
    load_moonraker_config();

    esp_err_t err =
        dt_runtime_network_start();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "network/portal unavailable: %s; "
            "runtime worker will continue",
            esp_err_to_name(err)
        );
    }

    runtime_heap_diag("network-phase-complete");

    s_action_queue =
        xQueueCreate(
            DT_ACTION_QUEUE_LEN,
            sizeof(dt_ui_action_t)
        );

    if (s_action_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(
        dt_ui_set_action_handler(
            ui_action_handler,
            NULL
        )
    );

    runtime_heap_diag("before-worker");

    /*
     * DT_RUNTIME_PSRAM_WORKER
     *
     * This worker performs sockets/HTTP only. NVS config is loaded before
     * task creation; portal writes reboot before new config is consumed.
     */
    BaseType_t worker_created =
        xTaskCreatePinnedToCoreWithCaps(
            runtime_task,
            "dt_runtime",
            7168,
            NULL,
            4,
            NULL,
            1,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    runtime_heap_diag("after-worker");

    if (worker_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Stage 2 runtime active; "
        "Moonraker=%s",
        s_base_url[0] != '\0'
            ? s_base_url
            : "<not configured>"
    );

    return ESP_OK;
}
