#include "dt_runtime.h"
#include "dt_printers.h"
#include "dt_printer_profile.h"
#include "dt_portal.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

#include "dc_moonraker.h"
#include "dc_wifi.h"

#include "dt_ui.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
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

/* DT_STAGE5_RUN_GCODE_FORWARD_DECL */
static esp_err_t run_gcode(const char *script);


#define DT_STATUS_PERIOD_MS   1000
#define DT_HTTP_TIMEOUT_MS    3500
#define DT_HTTP_RESPONSE_MAX  12288
#define DT_HTTP_RESPONSE_LARGE_MAX 32768
#define DT_ACTION_QUEUE_LEN   12
#define DT_FILE_QUEUE_LEN     4
#define DT_CAPABILITY_PERIOD_MS 30000
#define DT_SYSTEM_PERIOD_MS 5000
#define DT_AFC_STATUS_PERIOD_MS 2000
#define DT_FILAMENT_QUEUE_LEN 4
#define DT_TEMPERATURE_QUEUE_LEN 4 /* DT_TEMP_ENTRY */
#define DT_MOVE_QUEUE_LEN 8 /* DT_MOVE_STEP */


typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buffer_t;


typedef struct {
    char device_name[128];
    char filename[192];

    dt_ui_connection_t connection;

    /* DT_KLIPPER_ERROR */
    char status_message[160];
    dt_ui_job_state_t job;

    uint8_t progress_percent;
    uint8_t fan_percent;
    uint32_t aux_fan_revision;

    uint32_t elapsed_seconds;
    uint32_t remaining_seconds;

    float nozzle_c;
    float nozzle_target_c;
    float bed_c;
    float bed_target_c;

    /* DT_CHAMBER_TEMP: [temperature_sensor <profile>], no target */
    float chamber_c;

    float x;
    float y;
    float z;

    bool homed_x;
    bool homed_y;
    bool homed_z;

    bool can_extrude;
} runtime_snapshot_t;


typedef struct {
    dt_ui_file_request_t request;
    char path[DT_UI_FILE_PATH_MAX];
} dt_runtime_file_request_t;


typedef struct {
    dt_ui_filament_request_t request;
    int lane_number;
} dt_runtime_filament_request_t;


/* DT_TEMP_ENTRY */
typedef struct {
    dt_ui_heater_t heater;
    int celsius;
} dt_runtime_temperature_request_t;


/* DT_MOVE_STEP */
typedef struct {
    dt_ui_move_axis_t axis;
    int delta_mm;
    int speed_mms;
} dt_runtime_move_request_t;


typedef struct {
    char object_name[96];
    char display_name[64];
    dt_ui_fan_kind_t kind;
    uint8_t percent;
    bool speed_known;
    bool controllable;
} dt_runtime_aux_fan_t;


static QueueHandle_t s_action_queue;
static QueueHandle_t s_file_queue;
static QueueHandle_t s_filament_queue;
static QueueHandle_t s_temperature_queue;
static QueueHandle_t s_move_queue;
static char s_afc_lane_objects[DT_UI_AFC_MAX_LANES][64];
static size_t s_afc_lane_object_count;
static dt_ui_files_model_t *s_files_model;
static dt_ui_filament_model_t *s_filament_model;

/* DT_DYNAMIC_AUX_FANS */
static dt_runtime_aux_fan_t *s_aux_fans;
static size_t s_aux_fan_count;
static uint32_t s_aux_fan_revision = 1;

static char s_selected_file[DT_UI_FILE_PATH_MAX];

/* DT_FILE_THUMBNAIL_PREVIEW */
static uint8_t *s_file_thumbnail_data;

static char s_moonraker_host[128];
static char s_base_url[160];
static char s_api_key[160];

static bool s_have_previous;
static runtime_snapshot_t s_previous;

/*
 * DT_PRINTER_REBIND
 *
 * Set on the LVGL task when the user picks a different printer, consumed by
 * the runtime task at the top of its loop. A plain bool is enough: one
 * writer, one reader, and a missed edge would only defer the rebind by one
 * iteration -- but volatile, since both tasks may sit on different cores.
 */
static volatile bool s_rebind_requested;


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


static esp_err_t http_request_ex(
    esp_http_client_method_t method,
    const char *path,
    const char *json_body,
    char **response_out,
    int *http_status_out,
    size_t response_cap,
    size_t *response_len_out
)
{
    if (response_out != NULL) {
        *response_out = NULL;
    }

    if (response_len_out != NULL) {
        *response_len_out = 0;
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
            response_cap,
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
        .cap = response_cap,
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

    if (response_len_out != NULL) {
        *response_len_out =
            buffer.len;
    }

    if (response_out != NULL) {
        *response_out =
            response;
    } else {
        free(response);
    }

    return ESP_OK;
}

static esp_err_t http_request_with_cap(
    esp_http_client_method_t method,
    const char *path,
    const char *json_body,
    char **response_out,
    int *http_status_out,
    size_t response_cap
)
{
    return http_request_ex(
        method,
        path,
        json_body,
        response_out,
        http_status_out,
        response_cap,
        NULL
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
    return http_request_with_cap(
        method,
        path,
        json_body,
        response_out,
        http_status_out,
        DT_HTTP_RESPONSE_MAX
    );
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


static bool file_name_is_gcode(const char *name)
{
    if (name == NULL) {
        return false;
    }

    const char *dot = strrchr(name, '.');

    if (dot == NULL) {
        return false;
    }

    return
        strcasecmp(dot, ".gcode") == 0 ||
        strcasecmp(dot, ".gco") == 0 ||
        strcasecmp(dot, ".g") == 0;
}

static bool url_encode_query_value(
    const char *input,
    char *output,
    size_t output_size
)
{
    static const char hex[] = "0123456789ABCDEF";

    if (
        input == NULL ||
        output == NULL ||
        output_size == 0
    ) {
        return false;
    }

    size_t out = 0;

    for (
        const unsigned char *p = (const unsigned char *)input;
        *p != '\0';
        ++p
    ) {
        const bool unreserved =
            (*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '-' ||
            *p == '_' ||
            *p == '.' ||
            *p == '~';

        if (unreserved) {
            if (out + 1 >= output_size) {
                return false;
            }
            output[out++] = (char)*p;
        } else {
            if (out + 3 >= output_size) {
                return false;
            }
            output[out++] = '%';
            output[out++] = hex[(*p >> 4) & 0x0f];
            output[out++] = hex[*p & 0x0f];
        }
    }

    output[out] = '\0';
    return true;
}

static const char *relative_gcode_path(const char *path)
{
    if (path == NULL) {
        return "";
    }

    return strncmp(path, "gcodes/", 7) == 0
        ? path + 7
        : path;
}

static void push_files_ui(void)
{
    if (s_files_model == NULL) {
        return;
    }

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(
            TAG,
            "LVGL lock timeout; skipping file UI update"
        );
        return;
    }

    esp_err_t err = dt_ui_update_files(s_files_model);

    lvgl_port_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dt_ui_update_files: %s",
            esp_err_to_name(err)
        );
    }
}


/*
 * DT_UI_DETACH_BEFORE_FREE
 *
 * LVGL draws straight out of the runtime's thumbnail and snapshot buffers, so
 * a buffer must be detached from the model AND that detach pushed to the UI
 * before it is freed. push_files_ui() gives up after 200 ms, and a busy redraw
 * -- a page switch, or the CONTAIN-scaled webcam pane -- routinely holds the
 * lock for longer than that. A skipped push there is not a cosmetic miss: it
 * leaves LVGL reading memory the next line frees.
 *
 * So this waits (0 means portMAX_DELAY). There is no deadlock risk: the LVGL
 * task never blocks on the runtime task. Returns false only defensively; a
 * caller that sees it must leak the buffer rather than free one still in use.
 */
static bool push_files_ui_blocking(void)
{
    if (s_files_model == NULL) {
        return true;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(
            TAG,
            "LVGL lock unavailable; leaking a thumbnail rather than "
            "freeing it while in use"
        );

        return false;
    }

    (void)dt_ui_update_files(s_files_model);

    lvgl_port_unlock();

    return true;
}

static void files_set_error(const char *message)
{
    if (s_files_model == NULL) {
        return;
    }

    s_files_model->loading = false;

    snprintf(
        s_files_model->error,
        sizeof(s_files_model->error),
        "%s",
        message != NULL ? message : "File request failed"
    );

    push_files_ui();
}

static size_t files_count_eligible(cJSON *dirs, cJSON *files)
{
    size_t total = 0;
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, dirs) {
        cJSON *dirname =
            cJSON_GetObjectItemCaseSensitive(item, "dirname");

        if (
            cJSON_IsString(dirname) &&
            dirname->valuestring != NULL &&
            dirname->valuestring[0] != '.'
        ) {
            total++;
        }
    }

    cJSON_ArrayForEach(item, files) {
        cJSON *filename =
            cJSON_GetObjectItemCaseSensitive(item, "filename");

        if (
            cJSON_IsString(filename) &&
            filename->valuestring != NULL &&
            file_name_is_gcode(filename->valuestring)
        ) {
            total++;
        }
    }

    return total;
}

/*
 * DT_STAGE4_BOUNDED_PATH_COPY
 *
 * Avoid format-truncation and never create a silently truncated
 * Moonraker path. Overlong entries are skipped instead.
 */
static bool files_fill_entry(
    dt_ui_file_entry_t *entry,
    const char *name,
    bool is_directory,
    uint32_t size
)
{
    if (
        entry == NULL ||
        name == NULL ||
        s_files_model == NULL
    ) {
        return false;
    }

    const size_t directory_len =
        strlen(s_files_model->directory);

    const size_t name_len =
        strlen(name);

    if (
        directory_len + 1U + name_len + 1U >
        sizeof(entry->path)
    ) {
        ESP_LOGW(
            TAG,
            "skipping overlong file path: dir=%u name=%u",
            (unsigned)directory_len,
            (unsigned)name_len
        );

        return false;
    }

    memset(entry, 0, sizeof(*entry));

    const size_t visible_name_len =
        name_len < sizeof(entry->name) - 1U
            ? name_len
            : sizeof(entry->name) - 1U;

    memcpy(
        entry->name,
        name,
        visible_name_len
    );

    entry->name[visible_name_len] = '\0';

    memcpy(
        entry->path,
        s_files_model->directory,
        directory_len
    );

    entry->path[directory_len] = '/';

    memcpy(
        entry->path + directory_len + 1U,
        name,
        name_len
    );

    entry->path[
        directory_len + 1U + name_len
    ] = '\0';

    entry->is_directory = is_directory;
    entry->size_bytes = size;

    return true;
}

static esp_err_t files_refresh_directory(void)
{
    if (s_files_model == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * DT_STAGE7C_LIVE_FILES_STATE
     *
     * "Configured" and "online" are different states. s_files_model->online
     * is maintained only by the live status poll.
     */
    if (s_base_url[0] == '\0') {
        s_files_model->online = false;
        files_set_error("Printer is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_files_model->online) {
        files_set_error("Printer offline");
        return ESP_ERR_INVALID_STATE;
    }

    s_files_model->loading = true;
    s_files_model->error[0] = '\0';
    s_files_model->entry_count = 0;

    memset(
        s_files_model->entries,
        0,
        sizeof(s_files_model->entries)
    );

    push_files_ui();

    char encoded[640] = {0};

    if (
        !url_encode_query_value(
            s_files_model->directory,
            encoded,
            sizeof(encoded)
        )
    ) {
        files_set_error("Directory path is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char request_path[768] = {0};

    snprintf(
        request_path,
        sizeof(request_path),
        "/server/files/directory?path=%s&extended=false",
        encoded
    );

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_GET,
            request_path,
            NULL,
            &response,
            NULL
        );

    if (err != ESP_OK) {
        files_set_error("Moonraker directory request failed");
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);

    if (root == NULL) {
        files_set_error("Invalid directory response");
        return ESP_FAIL;
    }

    cJSON *payload = moonraker_payload(root);

    cJSON *dirs =
        cJSON_GetObjectItemCaseSensitive(payload, "dirs");

    cJSON *files =
        cJSON_GetObjectItemCaseSensitive(payload, "files");

    if (!cJSON_IsArray(dirs) || !cJSON_IsArray(files)) {
        cJSON_Delete(root);
        files_set_error("Moonraker directory data missing");
        return ESP_FAIL;
    }

    const size_t total =
        files_count_eligible(dirs, files);

    s_files_model->total_entries = total;

    if (total == 0) {
        s_files_model->offset = 0;
    } else if (s_files_model->offset >= total) {
        s_files_model->offset =
            ((total - 1) / DT_UI_FILE_ENTRY_MAX) *
            DT_UI_FILE_ENTRY_MAX;
    }

    size_t logical_index = 0;
    size_t filled = 0;
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, dirs) {
        cJSON *dirname =
            cJSON_GetObjectItemCaseSensitive(item, "dirname");

        if (
            !cJSON_IsString(dirname) ||
            dirname->valuestring == NULL ||
            dirname->valuestring[0] == '.'
        ) {
            continue;
        }

        if (
            logical_index >= s_files_model->offset &&
            filled < DT_UI_FILE_ENTRY_MAX
        ) {
            cJSON *size =
                cJSON_GetObjectItemCaseSensitive(item, "size");

            if (
                files_fill_entry(
                    &s_files_model->entries[filled],
                    dirname->valuestring,
                    true,
                    cJSON_IsNumber(size)
                        ? (uint32_t)size->valuedouble
                        : 0U
                )
            ) {
                filled++;
            }
        }

        logical_index++;
    }

    cJSON_ArrayForEach(item, files) {
        cJSON *filename =
            cJSON_GetObjectItemCaseSensitive(item, "filename");

        if (
            !cJSON_IsString(filename) ||
            filename->valuestring == NULL ||
            !file_name_is_gcode(filename->valuestring)
        ) {
            continue;
        }

        if (
            logical_index >= s_files_model->offset &&
            filled < DT_UI_FILE_ENTRY_MAX
        ) {
            cJSON *size =
                cJSON_GetObjectItemCaseSensitive(item, "size");

            if (
                files_fill_entry(
                    &s_files_model->entries[filled],
                    filename->valuestring,
                    false,
                    cJSON_IsNumber(size)
                        ? (uint32_t)size->valuedouble
                        : 0U
                )
            ) {
                filled++;
            }
        }

        logical_index++;
    }

    cJSON_Delete(root);

    s_files_model->entry_count = filled;
    s_files_model->has_previous =
        s_files_model->offset > 0;
    s_files_model->has_next =
        s_files_model->offset + filled < total;
    s_files_model->loading = false;
    s_files_model->error[0] = '\0';

    push_files_ui();

    ESP_LOGI(
        TAG,
        "files dir=%s total=%u offset=%u shown=%u",
        s_files_model->directory,
        (unsigned)total,
        (unsigned)s_files_model->offset,
        (unsigned)filled
    );

    return ESP_OK;
}


/*
 * DT_FILE_THUMBNAIL_PREVIEW
 *
 * Moonraker metadata supplies PNG byte size, dimensions, and a path relative
 * to the selected G-code. Only thumbnails fitting the existing ordinary HTTP
 * response buffer are accepted, so the stable HTTP worker stays unchanged.
 */
static bool thumbnail_path_is_png(
    const char *path
)
{
    if (path == NULL) {
        return false;
    }

    const size_t length = strlen(path);

    return
        length >= 4 &&
        strcasecmp(path + length - 4, ".png") == 0;
}


static bool thumbnail_url_encode_path(
    const char *input,
    char *output,
    size_t output_size
)
{
    static const char hex[] =
        "0123456789ABCDEF";

    if (
        input == NULL ||
        output == NULL ||
        output_size == 0
    ) {
        return false;
    }

    size_t used = 0;

    for (
        const unsigned char *p =
            (const unsigned char *)input;
        *p != '\0';
        ++p
    ) {
        const bool alpha =
            (*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z');

        const bool digit =
            *p >= '0' && *p <= '9';

        const bool passthrough =
            alpha ||
            digit ||
            *p == '-' ||
            *p == '_' ||
            *p == '.' ||
            *p == '~' ||
            *p == '/';

        const size_t needed =
            passthrough ? 1U : 3U;

        if (
            used + needed + 1U >
            output_size
        ) {
            return false;
        }

        if (passthrough) {
            output[used++] = (char)*p;
        } else {
            output[used++] = '%';
            output[used++] =
                hex[(*p >> 4) & 0x0f];
            output[used++] =
                hex[*p & 0x0f];
        }
    }

    output[used] = '\0';
    return true;
}


/* DT_FILE_THUMBNAIL_NOINLINE_FIX */
static __attribute__((noinline)) esp_err_t files_load_thumbnail(
    cJSON *metadata,
    const char *relative_gcode
)
{
    if (
        s_files_model == NULL ||
        metadata == NULL ||
        relative_gcode == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *thumbnails =
        cJSON_GetObjectItemCaseSensitive(
            metadata,
            "thumbnails"
        );

    if (!cJSON_IsArray(thumbnails)) {
        return ESP_ERR_NOT_FOUND;
    }

    const char *best_relative = NULL;
    uint32_t best_size = 0;
    uint16_t best_width = 0;
    uint16_t best_height = 0;
    unsigned best_score = UINT_MAX;

    cJSON *item = NULL;

    cJSON_ArrayForEach(item, thumbnails) {
        cJSON *relative_path =
            cJSON_GetObjectItemCaseSensitive(
                item,
                "relative_path"
            );

        cJSON *size =
            cJSON_GetObjectItemCaseSensitive(
                item,
                "size"
            );

        cJSON *width =
            cJSON_GetObjectItemCaseSensitive(
                item,
                "width"
            );

        cJSON *height =
            cJSON_GetObjectItemCaseSensitive(
                item,
                "height"
            );

        if (
            !cJSON_IsString(relative_path) ||
            relative_path->valuestring == NULL ||
            !thumbnail_path_is_png(
                relative_path->valuestring
            ) ||
            !cJSON_IsNumber(size) ||
            !cJSON_IsNumber(width) ||
            !cJSON_IsNumber(height)
        ) {
            continue;
        }

        const uint32_t candidate_size =
            size->valuedouble > 0
                ? (uint32_t)size->valuedouble
                : 0U;

        const uint32_t candidate_width =
            width->valuedouble > 0
                ? (uint32_t)width->valuedouble
                : 0U;

        const uint32_t candidate_height =
            height->valuedouble > 0
                ? (uint32_t)height->valuedouble
                : 0U;

        /*
         * http_event reserves one trailing NUL byte. Embedded NULs inside PNG
         * payloads are fine because decoding uses metadata's declared size.
         */
        if (
            candidate_size < 24U ||
            candidate_size >= DT_HTTP_RESPONSE_MAX ||
            candidate_width == 0U ||
            candidate_height == 0U ||
            candidate_width > UINT16_MAX ||
            candidate_height > UINT16_MAX
        ) {
            continue;
        }

        const uint32_t max_dimension =
            candidate_width > candidate_height
                ? candidate_width
                : candidate_height;

        /*
         * DT_FILE_THUMBNAIL_150_PREVIEW
         *
         * Select the embedded PNG whose largest dimension is closest to 150.
         * This prefers Orca's existing 48x48 thumbnail over its 300x300
         * thumbnail, keeping PNG decode memory small.
         */
        const unsigned score =
            max_dimension > 150U
                ? (unsigned)(max_dimension - 150U)
                : (unsigned)(150U - max_dimension);

        if (
            best_relative == NULL ||
            score < best_score ||
            (
                score == best_score &&
                candidate_size > best_size
            )
        ) {
            best_relative =
                relative_path->valuestring;
            best_size = candidate_size;
            best_width =
                (uint16_t)candidate_width;
            best_height =
                (uint16_t)candidate_height;
            best_score = score;
        }
    }

    if (best_relative == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char thumbnail_path[512] = {0};
    const char *slash =
        strrchr(relative_gcode, '/');

    if (slash != NULL) {
        const size_t parent_length =
            (size_t)(slash - relative_gcode);

        if (
            parent_length +
            1U +
            strlen(best_relative) +
            1U >
            sizeof(thumbnail_path)
        ) {
            return ESP_ERR_INVALID_SIZE;
        }

        snprintf(
            thumbnail_path,
            sizeof(thumbnail_path),
            "%.*s/%s",
            (int)parent_length,
            relative_gcode,
            best_relative
        );
    } else {
        if (
            strlen(best_relative) + 1U >
            sizeof(thumbnail_path)
        ) {
            return ESP_ERR_INVALID_SIZE;
        }

        snprintf(
            thumbnail_path,
            sizeof(thumbnail_path),
            "%s",
            best_relative
        );
    }

    char encoded_path[640] = {0};

    if (
        !thumbnail_url_encode_path(
            thumbnail_path,
            encoded_path,
            sizeof(encoded_path)
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    char request_path[768] = {0};

    int written =
        snprintf(
            request_path,
            sizeof(request_path),
            "/server/files/gcodes/%s",
            encoded_path
        );

    if (
        written < 0 ||
        (size_t)written >=
            sizeof(request_path)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *response = NULL;
    size_t response_len = 0;

    /*
     * DT_FILE_THUMBNAIL_ACTUAL_LEN
     *
     * Use http_request_ex() (not http_request()) so we learn the ACTUAL
     * number of bytes received for the PNG, rather than trusting Moonraker
     * metadata's declared "size". The two can disagree (stale metadata,
     * a server-side re-slice, etc.), and LVGL's LodePNG decoder needs the
     * real byte count in data_size to decode correctly -- feeding it a
     * wrong length is a likely cause of thumbnails silently failing to
     * decode/display.
     */
    esp_err_t err =
        http_request_ex(
            HTTP_METHOD_GET,
            request_path,
            NULL,
            &response,
            NULL,
            DT_HTTP_RESPONSE_MAX,
            &response_len
        );

    if (err != ESP_OK) {
        free(response);
        return err;
    }

    static const uint8_t png_magic[] = {
        0x89, 0x50, 0x4e, 0x47,
        0x0d, 0x0a, 0x1a, 0x0a,
    };

    if (
        response == NULL ||
        memcmp(
            response,
            png_magic,
            sizeof(png_magic)
        ) != 0
    ) {
        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /*
     * http_request allocates response data in PSRAM. Retain that allocation
     * directly as the LVGL variable-source PNG.
     */
    if (response_len < 24U) {
        ESP_LOGW(
            TAG,
            "thumbnail response too small: actual=%u metadata=%u path=%s",
            (unsigned)response_len,
            (unsigned)best_size,
            thumbnail_path
        );

        free(response);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (response_len != (size_t)best_size) {
        ESP_LOGW(
            TAG,
            "thumbnail size mismatch: metadata=%u actual=%u path=%s (using actual)",
            (unsigned)best_size,
            (unsigned)response_len,
            thumbnail_path
        );
    }

    s_file_thumbnail_data =
        (uint8_t *)response;

    s_files_model->thumbnail_data =
        s_file_thumbnail_data;

    s_files_model->thumbnail_size =
        (uint32_t)response_len;

    s_files_model->thumbnail_width =
        best_width;

    s_files_model->thumbnail_height =
        best_height;

    ESP_LOGD(
        TAG,
        "thumbnail loaded %ux%u bytes=%u path=%s",
        (unsigned)best_width,
        (unsigned)best_height,
        (unsigned)best_size,
        thumbnail_path
    );

    return ESP_OK;
}


static esp_err_t files_select(const char *full_path)
{
    if (
        s_files_model == NULL ||
        full_path == NULL ||
        full_path[0] == '\0'
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *relative =
        relative_gcode_path(full_path);

    snprintf(
        s_selected_file,
        sizeof(s_selected_file),
        "%s",
        relative
    );

    const char *name = strrchr(relative, '/');
    name = name != NULL ? name + 1 : relative;

    s_files_model->selected = true;

    const size_t selected_name_len =
        strlen(name);

    const size_t selected_visible_len =
        selected_name_len <
            sizeof(s_files_model->selected_name) - 1U
            ? selected_name_len
            : sizeof(s_files_model->selected_name) - 1U;

    memcpy(
        s_files_model->selected_name,
        name,
        selected_visible_len
    );

    s_files_model->selected_name[
        selected_visible_len
    ] = '\0';

    snprintf(
        s_files_model->selected_path,
        sizeof(s_files_model->selected_path),
        "%s",
        full_path
    );

    s_files_model->estimated_seconds = 0;
    s_files_model->filament_weight_g = 0.0f;
    s_files_model->filament_length_mm = 0.0f;
    s_files_model->layer_height_mm = 0.0f;

    /*
     * DT_FILE_THUMBNAIL_PREVIEW
     * Detach the old LVGL source before freeing its runtime-owned bytes.
     */
    uint8_t *old_thumbnail =
        s_file_thumbnail_data;

    s_file_thumbnail_data = NULL;
    s_files_model->thumbnail_data = NULL;
    s_files_model->thumbnail_size = 0;
    s_files_model->thumbnail_width = 0;
    s_files_model->thumbnail_height = 0;
    s_files_model->slicer[0] = '\0';
    s_files_model->filament_type[0] = '\0';

    for (size_t i = 0; i < s_files_model->entry_count; ++i) {
        if (
            strcmp(
                s_files_model->entries[i].path,
                full_path
            ) == 0
        ) {
            s_files_model->selected_size_bytes =
                s_files_model->entries[i].size_bytes;
            break;
        }
    }

    snprintf(
        s_files_model->detail_error,
        sizeof(s_files_model->detail_error),
        "Loading metadata..."
    );

    /* DT_UI_DETACH_BEFORE_FREE: the detach must land before the free. */
    if (push_files_ui_blocking()) {
        free(old_thumbnail);
    }

    char encoded[640] = {0};

    if (
        !url_encode_query_value(
            relative,
            encoded,
            sizeof(encoded)
        )
    ) {
        snprintf(
            s_files_model->detail_error,
            sizeof(s_files_model->detail_error),
            "Filename is too long"
        );
        push_files_ui();
        return ESP_ERR_INVALID_SIZE;
    }

    char request_path[768] = {0};

    snprintf(
        request_path,
        sizeof(request_path),
        "/server/files/metadata?filename=%s",
        encoded
    );

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_GET,
            request_path,
            NULL,
            &response,
            NULL
        );

    if (err != ESP_OK) {
        snprintf(
            s_files_model->detail_error,
            sizeof(s_files_model->detail_error),
            "Metadata unavailable"
        );
        push_files_ui();
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);

    if (root == NULL) {
        snprintf(
            s_files_model->detail_error,
            sizeof(s_files_model->detail_error),
            "Invalid metadata response"
        );
        push_files_ui();
        return ESP_FAIL;
    }

    cJSON *payload = moonraker_payload(root);

    cJSON *size =
        cJSON_GetObjectItemCaseSensitive(payload, "size");
    if (cJSON_IsNumber(size)) {
        s_files_model->selected_size_bytes =
            (uint32_t)size->valuedouble;
    }

    cJSON *estimated =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "estimated_time"
        );
    if (cJSON_IsNumber(estimated)) {
        s_files_model->estimated_seconds =
            estimated->valuedouble > 0.0
                ? (uint32_t)estimated->valuedouble
                : 0U;
    }

    cJSON *weight =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "filament_weight_total"
        );
    if (cJSON_IsNumber(weight)) {
        s_files_model->filament_weight_g =
            (float)weight->valuedouble;
    }

    cJSON *length =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "filament_total"
        );
    if (cJSON_IsNumber(length)) {
        s_files_model->filament_length_mm =
            (float)length->valuedouble;
    }

    cJSON *layer =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "layer_height"
        );
    if (cJSON_IsNumber(layer)) {
        s_files_model->layer_height_mm =
            (float)layer->valuedouble;
    }

    cJSON *slicer =
        cJSON_GetObjectItemCaseSensitive(payload, "slicer");
    if (
        cJSON_IsString(slicer) &&
        slicer->valuestring != NULL
    ) {
        snprintf(
            s_files_model->slicer,
            sizeof(s_files_model->slicer),
            "%s",
            slicer->valuestring
        );
    }

    cJSON *filament_type =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "filament_type"
        );
    if (
        cJSON_IsString(filament_type) &&
        filament_type->valuestring != NULL
    ) {
        snprintf(
            s_files_model->filament_type,
            sizeof(s_files_model->filament_type),
            "%s",
            filament_type->valuestring
        );
    }

    /*
     * Missing or oversized thumbnails are non-fatal; metadata still renders.
     */
    esp_err_t thumbnail_err =
        files_load_thumbnail(
            payload,
            relative
        );

    if (
        thumbnail_err != ESP_OK &&
        thumbnail_err != ESP_ERR_NOT_FOUND
    ) {
        ESP_LOGW(
            TAG,
            "thumbnail unavailable: %s",
            esp_err_to_name(thumbnail_err)
        );
    }

    cJSON_Delete(root);

    s_files_model->detail_error[0] = '\0';
    push_files_ui();

    return ESP_OK;
}

static esp_err_t files_handle_request(
    const dt_runtime_file_request_t *request
)
{
    if (request == NULL || s_files_model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        request->request != DT_UI_FILE_REQUEST_REFRESH &&
        !s_files_model->online
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (request->request) {
    case DT_UI_FILE_REQUEST_REFRESH:
        return files_refresh_directory();

    case DT_UI_FILE_REQUEST_UP: {
        if (strcmp(s_files_model->directory, "gcodes") == 0) {
            return ESP_OK;
        }

        char *slash =
            strrchr(s_files_model->directory, '/');

        if (slash == NULL) {
            snprintf(
                s_files_model->directory,
                sizeof(s_files_model->directory),
                "gcodes"
            );
        } else {
            *slash = '\0';
        }

        s_files_model->offset = 0;
        s_files_model->selected = false;
        s_selected_file[0] = '\0';

        return files_refresh_directory();
    }

    case DT_UI_FILE_REQUEST_PREVIOUS:
        if (s_files_model->offset >= DT_UI_FILE_ENTRY_MAX) {
            s_files_model->offset -= DT_UI_FILE_ENTRY_MAX;
        } else {
            s_files_model->offset = 0;
        }
        return files_refresh_directory();

    case DT_UI_FILE_REQUEST_NEXT:
        if (s_files_model->has_next) {
            s_files_model->offset += DT_UI_FILE_ENTRY_MAX;
        }
        return files_refresh_directory();

    case DT_UI_FILE_REQUEST_OPEN_DIRECTORY:
        if (request->path[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }

        snprintf(
            s_files_model->directory,
            sizeof(s_files_model->directory),
            "%s",
            request->path
        );

        s_files_model->offset = 0;
        s_files_model->selected = false;
        s_selected_file[0] = '\0';

        return files_refresh_directory();

    case DT_UI_FILE_REQUEST_SELECT_FILE:
        return files_select(request->path);

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}






static bool string_contains_ci(
    const char *text,
    const char *needle
)
{
    if (
        text == NULL ||
        needle == NULL ||
        needle[0] == '\0'
    ) {
        return false;
    }

    const size_t needle_len =
        strlen(needle);

    for (
        const char *p = text;
        *p != '\0';
        ++p
    ) {
        if (
            strncasecmp(
                p,
                needle,
                needle_len
            ) == 0
        ) {
            return true;
        }
    }

    return false;
}


static void push_filament_ui(void)
{
    if (s_filament_model == NULL) {
        return;
    }

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(
            TAG,
            "LVGL lock timeout; skipping filament UI update"
        );
        return;
    }

    esp_err_t err =
        dt_ui_update_filament(
            s_filament_model
        );

    lvgl_port_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dt_ui_update_filament: %s",
            esp_err_to_name(err)
        );
    }
}







/*
 * DT_STAGE7A_LARGE_RAW_HELP
 *
 * /printer/gcode/help can be much larger than normal Moonraker responses.
 * For AFC capability discovery we only need exact command-key presence, so
 * keep the raw response in PSRAM and avoid constructing a large cJSON tree.
 */
static bool json_raw_has_object_key(
    const char *json,
    const char *key
)
{
    if (json == NULL || key == NULL || key[0] == '\0') {
        return false;
    }

    char token[96] = {0};

    const int written = snprintf(
        token,
        sizeof(token),
        "\"%s\"",
        key
    );

    if (
        written <= 0 ||
        (size_t)written >= sizeof(token)
    ) {
        return false;
    }

    const size_t token_len = (size_t)written;
    const char *cursor = json;

    while (
        (cursor = strstr(cursor, token)) != NULL
    ) {
        const char *after =
            cursor + token_len;

        while (
            *after == ' ' ||
            *after == '\t' ||
            *after == '\r' ||
            *after == '\n'
        ) {
            ++after;
        }

        if (*after == ':') {
            return true;
        }

        cursor += token_len;
    }

    return false;
}


static esp_err_t discover_afc_registered_commands(void)
{
    if (
        s_filament_model == NULL ||
        !s_filament_model->afc_detected
    ) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char *response = NULL;

    esp_err_t err =
        http_request_with_cap(
            HTTP_METHOD_GET,
            "/printer/gcode/help",
            NULL,
            &response,
            NULL,
            DT_HTTP_RESPONSE_LARGE_MAX
        );

    if (err != ESP_OK) {
        return err;
    }

    s_filament_model->has_afc_clear_message =
        json_raw_has_object_key(
            response,
            "AFC_CLEAR_MESSAGE"
        );

    s_filament_model->has_afc_lane_reset =
        json_raw_has_object_key(
            response,
            "AFC_LANE_RESET"
        );

    /*
     * DT_AFC_UNLOAD_LOADED_LANE
     * TOOL_UNLOAD is a natively-registered AFC command (like the two
     * above), not a user-defined [gcode_macro] -- it only shows up in
     * /printer/gcode/help, never in the "gcode_macro <name>" config
     * object scan that BT_CHANGE_TOOL/BT_LANE_EJECT/BT_RESUME come from.
     */
    s_filament_model->has_afc_tool_unload =
        json_raw_has_object_key(
            response,
            "TOOL_UNLOAD"
        );

    free(response);

    return ESP_OK;
}



static esp_err_t run_gcode(const char *script);

/*
 * DT_DYNAMIC_AUX_FANS
 *
 * Discover fan_generic objects as manually controllable. Automatic Klipper fan
 * classes are status-only: DragonTouch must not override their policy.
 */
static bool aux_fan_command_name_safe(
    const char *name
)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (const unsigned char *p =
             (const unsigned char *)name;
         *p != '\0';
         ++p) {
        const bool alpha =
            (*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z');

        const bool digit =
            *p >= '0' && *p <= '9';

        if (
            !alpha &&
            !digit &&
            *p != '_' &&
            *p != '-'
        ) {
            return false;
        }
    }

    return true;
}


static bool aux_fan_url_encode(
    const char *input,
    char *output,
    size_t output_size
)
{
    static const char hex[] =
        "0123456789ABCDEF";

    if (
        input == NULL ||
        output == NULL ||
        output_size == 0
    ) {
        return false;
    }

    size_t used = 0;

    for (
        const unsigned char *p =
            (const unsigned char *)input;
        *p != '\0';
        ++p
    ) {
        const bool alpha =
            (*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z');

        const bool digit =
            *p >= '0' && *p <= '9';

        const bool unreserved =
            alpha ||
            digit ||
            *p == '-' ||
            *p == '_' ||
            *p == '.' ||
            *p == '~';

        const size_t needed =
            unreserved ? 1u : 3u;

        if (
            used + needed + 1 >
            output_size
        ) {
            return false;
        }

        if (unreserved) {
            output[used++] =
                (char)*p;
        } else {
            output[used++] = '%';
            output[used++] =
                hex[(*p >> 4) & 0x0f];
            output[used++] =
                hex[*p & 0x0f];
        }
    }

    output[used] = '\0';
    return true;
}


static void aux_fans_mark_offline(void)
{
    bool changed = false;

    for (size_t i = 0; i < s_aux_fan_count; ++i) {
        if (s_aux_fans[i].speed_known) {
            s_aux_fans[i].speed_known = false;
            s_aux_fans[i].percent = 255;
            changed = true;
        }
    }

    if (changed) {
        ++s_aux_fan_revision;
    }
}


static esp_err_t discover_aux_fans(void)
{
    if (s_aux_fans == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_GET,
            "/printer/objects/list",
            NULL,
            &response,
            NULL
        );

    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);

    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *payload = moonraker_payload(root);
    cJSON *objects =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "objects"
        );

    if (!cJSON_IsArray(objects)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    dt_runtime_aux_fan_t
        discovered[DT_UI_AUX_FAN_MAX];

    memset(
        discovered,
        0,
        sizeof(discovered)
    );

    size_t count = 0;

    /*
     * Pass 0: manually controllable fan_generic entries.
     * Pass 1: automatic/status-only fan classes.
     */
    for (int pass = 0; pass < 2; ++pass) {
        cJSON *item = NULL;

        cJSON_ArrayForEach(item, objects) {
            if (
                count >= DT_UI_AUX_FAN_MAX ||
                !cJSON_IsString(item) ||
                item->valuestring == NULL
            ) {
                continue;
            }

            const char *object = item->valuestring;
            const char *display = NULL;
            dt_ui_fan_kind_t kind;
            bool controllable = false;
            bool matched = false;

            if (
                strncmp(
                    object,
                    "fan_generic ",
                    12
                ) == 0
            ) {
                if (pass != 0) {
                    continue;
                }

                display = object + 12;
                kind = DT_UI_FAN_KIND_GENERIC;
                controllable =
                    aux_fan_command_name_safe(
                        display
                    );
                matched = true;
            } else if (pass == 1) {
                if (
                    strncmp(
                        object,
                        "controller_fan ",
                        15
                    ) == 0
                ) {
                    display = object + 15;
                    kind =
                        DT_UI_FAN_KIND_CONTROLLER;
                    matched = true;
                } else if (
                    strncmp(
                        object,
                        "heater_fan ",
                        11
                    ) == 0
                ) {
                    display = object + 11;
                    kind =
                        DT_UI_FAN_KIND_HEATER;
                    matched = true;
                } else if (
                    strncmp(
                        object,
                        "temperature_fan ",
                        16
                    ) == 0
                ) {
                    display = object + 16;
                    kind =
                        DT_UI_FAN_KIND_TEMPERATURE;
                    matched = true;
                }
            }

            if (!matched || display == NULL) {
                continue;
            }

            dt_runtime_aux_fan_t *fan =
                &discovered[count];

            snprintf(
                fan->object_name,
                sizeof(fan->object_name),
                "%s",
                object
            );

            snprintf(
                fan->display_name,
                sizeof(fan->display_name),
                "%s",
                display
            );

            fan->kind = kind;
            fan->controllable = controllable;
            fan->percent = 255;
            fan->speed_known = false;

            for (
                size_t old = 0;
                old < s_aux_fan_count;
                ++old
            ) {
                if (
                    strcmp(
                        s_aux_fans[old].object_name,
                        fan->object_name
                    ) == 0
                ) {
                    fan->percent =
                        s_aux_fans[old].percent;
                    fan->speed_known =
                        s_aux_fans[old].speed_known;
                    break;
                }
            }

            ++count;
        }
    }

    bool changed =
        count != s_aux_fan_count;

    if (!changed) {
        for (size_t i = 0; i < count; ++i) {
            if (
                strcmp(
                    discovered[i].object_name,
                    s_aux_fans[i].object_name
                ) != 0 ||
                discovered[i].kind !=
                    s_aux_fans[i].kind ||
                discovered[i].controllable !=
                    s_aux_fans[i].controllable
            ) {
                changed = true;
                break;
            }
        }
    }

    memset(
        s_aux_fans,
        0,
        sizeof(*s_aux_fans) *
            DT_UI_AUX_FAN_MAX
    );

    memcpy(
        s_aux_fans,
        discovered,
        sizeof(discovered)
    );

    s_aux_fan_count = count;

    if (changed) {
        ++s_aux_fan_revision;
    }

    ESP_LOGI(
        TAG,
        "fan capabilities auxiliary=%u",
        (unsigned)s_aux_fan_count
    );

    for (size_t i = 0; i < s_aux_fan_count; ++i) {
        ESP_LOGD(
            TAG,
            "fan[%u] object='%s' control=%d",
            (unsigned)i,
            s_aux_fans[i].object_name,
            s_aux_fans[i].controllable
                ? 1
                : 0
        );
    }

    cJSON_Delete(root);
    return ESP_OK;
}


static esp_err_t query_aux_fan_status(void)
{
    if (
        s_aux_fans == NULL ||
        s_aux_fan_count == 0
    ) {
        return ESP_OK;
    }

    const size_t endpoint_cap = 2048;

    char *endpoint =
        heap_caps_calloc(
            1,
            endpoint_cap,
            MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
        );

    if (endpoint == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t used =
        (size_t)snprintf(
            endpoint,
            endpoint_cap,
            "/printer/objects/query?"
        );

    for (size_t i = 0; i < s_aux_fan_count; ++i) {
        char encoded[256] = {0};

        if (
            !aux_fan_url_encode(
                s_aux_fans[i].object_name,
                encoded,
                sizeof(encoded)
            )
        ) {
            free(endpoint);
            return ESP_ERR_INVALID_SIZE;
        }

        int written =
            snprintf(
                endpoint + used,
                endpoint_cap - used,
                "%s%s",
                i == 0 ? "" : "&",
                encoded
            );

        if (
            written < 0 ||
            (size_t)written >=
                endpoint_cap - used
        ) {
            free(endpoint);
            return ESP_ERR_INVALID_SIZE;
        }

        used += (size_t)written;
    }

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_GET,
            endpoint,
            NULL,
            &response,
            NULL
        );

    free(endpoint);

    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(response);
    free(response);

    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *payload = moonraker_payload(root);
    cJSON *status =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "status"
        );

    if (!cJSON_IsObject(status)) {
        status = payload;
    }

    bool changed = false;

    for (size_t i = 0; i < s_aux_fan_count; ++i) {
        cJSON *fan =
            cJSON_GetObjectItemCaseSensitive(
                status,
                s_aux_fans[i].object_name
            );

        float speed = NAN;
        bool known =
            json_number(
                fan,
                "speed",
                &speed
            ) &&
            isfinite(speed);

        uint8_t percent = 255;

        if (known) {
            if (speed < 0.0f) {
                speed = 0.0f;
            }

            if (speed > 1.0f) {
                speed = 1.0f;
            }

            percent =
                (uint8_t)(
                    speed * 100.0f +
                    0.5f
                );
        }

        if (
            s_aux_fans[i].speed_known !=
                known ||
            s_aux_fans[i].percent !=
                percent
        ) {
            s_aux_fans[i].speed_known =
                known;
            s_aux_fans[i].percent =
                percent;
            changed = true;
        }
    }

    if (changed) {
        ++s_aux_fan_revision;
    }

    cJSON_Delete(root);
    return ESP_OK;
}


static esp_err_t execute_aux_fan_action(
    dt_ui_action_t action
)
{
    /*
     * DT_DYNAMIC_AUX_FAN_SLIDERS
     *
     * Each discovered fan slot owns 101 contiguous action values, one for
     * every integer percentage from 0 through 100.
     */
    if (
        action < DT_UI_ACTION_AUX_FAN_BASE ||
        action > DT_UI_ACTION_AUX_FAN_LAST
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    const unsigned offset =
        (unsigned)(
            action -
            DT_UI_ACTION_AUX_FAN_BASE
        );

    const size_t slot =
        offset /
        DT_UI_AUX_FAN_LEVEL_COUNT;

    const unsigned percent =
        offset %
        DT_UI_AUX_FAN_LEVEL_COUNT;

    if (
        s_aux_fans == NULL ||
        slot >= s_aux_fan_count ||
        percent > 100 ||
        !s_aux_fans[slot].controllable
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    char speed[8] = {0};

    if (percent >= 100) {
        snprintf(speed, sizeof(speed), "1.00");
    } else {
        snprintf(
            speed,
            sizeof(speed),
            "0.%02u",
            percent
        );
    }

    char command[192] = {0};

    int written =
        snprintf(
            command,
            sizeof(command),
            "SET_FAN_SPEED FAN=%s SPEED=%s",
            s_aux_fans[slot].display_name,
            speed
        );

    if (
        written < 0 ||
        (size_t)written >= sizeof(command)
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    return run_gcode(command);
}



static esp_err_t discover_filament_capabilities(void)
{
    if (s_filament_model == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_GET,
            "/printer/objects/list",
            NULL,
            &response,
            NULL
        );

    if (err != ESP_OK) {
        s_filament_model->capabilities_known = false;
        push_filament_ui();
        return err;
    }

    cJSON *root =
        cJSON_Parse(response);

    free(response);

    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *payload =
        moonraker_payload(root);

    cJSON *objects =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "objects"
        );

    if (!cJSON_IsArray(objects)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    s_filament_model->has_load_macro = false;
    s_filament_model->has_unload_macro = false;
    s_filament_model->has_m600 = false;
    s_filament_model->afc_detected = false;
    s_filament_model->mmu_detected = false;
    s_filament_model->toolchanger_detected = false;
    s_filament_model->has_bt_change_tool = false;
    s_filament_model->has_bt_lane_eject = false;
    s_filament_model->has_bt_resume = false;
    s_filament_model->has_afc_tool_unload = false;
    s_filament_model->has_afc_clear_message = false;
    s_filament_model->has_afc_lane_reset = false;
    s_filament_model->load_macro[0] = '\0';
    s_filament_model->unload_macro[0] = '\0';

    s_afc_lane_object_count = 0;
    memset(
        s_afc_lane_objects,
        0,
        sizeof(s_afc_lane_objects)
    );

    cJSON *item = NULL;

    cJSON_ArrayForEach(item, objects) {
        if (
            !cJSON_IsString(item) ||
            item->valuestring == NULL
        ) {
            continue;
        }

        const char *object =
            item->valuestring;

        const char *afc_lane_prefix =
            "AFC_stepper ";

        const size_t afc_lane_prefix_len =
            strlen(afc_lane_prefix);

        if (
            strncasecmp(
                object,
                afc_lane_prefix,
                afc_lane_prefix_len
            ) == 0 &&
            s_afc_lane_object_count <
                DT_UI_AFC_MAX_LANES
        ) {
            snprintf(
                s_afc_lane_objects[
                    s_afc_lane_object_count
                ],
                sizeof(
                    s_afc_lane_objects[
                        s_afc_lane_object_count
                    ]
                ),
                "%s",
                object
            );

            s_afc_lane_object_count++;
        }

        if (
            string_contains_ci(object, "afc") ||
            string_contains_ci(object, "boxturtle")
        ) {
            s_filament_model->afc_detected = true;
        }

        if (
            string_contains_ci(object, "mmu") ||
            string_contains_ci(object, "ercf") ||
            string_contains_ci(object, "happy_hare")
        ) {
            s_filament_model->mmu_detected = true;
        }

        if (
            string_contains_ci(object, "toolchanger") ||
            string_contains_ci(object, "tool_changer") ||
            string_contains_ci(object, "stealthchanger") ||
            string_contains_ci(object, "tapchanger")
        ) {
            s_filament_model->toolchanger_detected = true;
        }

        const char *prefix =
            "gcode_macro ";

        const size_t prefix_len =
            strlen(prefix);

        if (
            strncasecmp(
                object,
                prefix,
                prefix_len
            ) != 0
        ) {
            continue;
        }

        const char *macro =
            object + prefix_len;

        if (
            strcasecmp(
                macro,
                "LOAD_FILAMENT"
            ) == 0
        ) {
            s_filament_model->has_load_macro = true;

            snprintf(
                s_filament_model->load_macro,
                sizeof(s_filament_model->load_macro),
                "LOAD_FILAMENT"
            );
        } else if (
            strcasecmp(
                macro,
                "UNLOAD_FILAMENT"
            ) == 0
        ) {
            s_filament_model->has_unload_macro = true;

            snprintf(
                s_filament_model->unload_macro,
                sizeof(s_filament_model->unload_macro),
                "UNLOAD_FILAMENT"
            );
        } else if (
            strcasecmp(
                macro,
                "M600"
            ) == 0
        ) {
            s_filament_model->has_m600 = true;
        } else if (
            strcasecmp(
                macro,
                "BT_CHANGE_TOOL"
            ) == 0
        ) {
            s_filament_model->has_bt_change_tool = true;
        } else if (
            strcasecmp(
                macro,
                "BT_LANE_EJECT"
            ) == 0
        ) {
            s_filament_model->has_bt_lane_eject = true;
        }
        else if (
            strcasecmp(
                macro,
                "BT_RESUME"
            ) == 0
        ) {
            s_filament_model->has_bt_resume = true;
        }
    }

    cJSON_Delete(root);

    if (s_filament_model->afc_detected) {
        (void)discover_afc_registered_commands();

        snprintf(
            s_filament_model->mode,
            sizeof(s_filament_model->mode),
            "AFC / managed filament"
        );
    } else if (s_filament_model->mmu_detected) {
        snprintf(
            s_filament_model->mode,
            sizeof(s_filament_model->mode),
            "MMU / managed filament"
        );
    } else if (s_filament_model->toolchanger_detected) {
        snprintf(
            s_filament_model->mode,
            sizeof(s_filament_model->mode),
            "Toolchanger / multi-tool"
        );
    } else if (
        s_filament_model->has_load_macro ||
        s_filament_model->has_unload_macro
    ) {
        snprintf(
            s_filament_model->mode,
            sizeof(s_filament_model->mode),
            "Printer filament macros"
        );
    } else {
        snprintf(
            s_filament_model->mode,
            sizeof(s_filament_model->mode),
            "Manual extruder"
        );
    }

    s_filament_model->capabilities_known = true;

    ESP_LOGI(
        TAG,
        "filament capabilities load=%d unload=%d m600=%d afc=%d mmu=%d toolchanger=%d bt_change=%d bt_eject=%d bt_resume=%d clear=%d lane_reset=%d tool_unload=%d lanes=%u",
        s_filament_model->has_load_macro,
        s_filament_model->has_unload_macro,
        s_filament_model->has_m600,
        s_filament_model->afc_detected,
        s_filament_model->mmu_detected,
        s_filament_model->toolchanger_detected,
        s_filament_model->has_bt_change_tool,
        s_filament_model->has_bt_lane_eject,
        s_filament_model->has_bt_resume,
        s_filament_model->has_afc_clear_message,
        s_filament_model->has_afc_lane_reset,
        s_filament_model->has_afc_tool_unload,
        (unsigned)s_afc_lane_object_count
    );

    push_filament_ui();

    return ESP_OK;
}


static void json_copy_string(
    cJSON *object,
    const char *name,
    char *dest,
    size_t dest_size
)
{
    if (
        object == NULL ||
        name == NULL ||
        dest == NULL ||
        dest_size == 0
    ) {
        return;
    }

    cJSON *item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            name
        );

    if (
        cJSON_IsString(item) &&
        item->valuestring != NULL
    ) {
        snprintf(
            dest,
            dest_size,
            "%s",
            item->valuestring
        );
    } else {
        dest[0] = '\0';
    }
}


static bool json_bool_value(
    cJSON *object,
    const char *name
)
{
    cJSON *item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            name
        );

    return cJSON_IsTrue(item);
}


/* ---------------------------------------------------------------------- */
/* DT_WEBCAM_SNAPSHOT                                                      */
/*                                                                          */
/* On-demand JPEG snapshot, not a live stream -- the ESP32-S3 has no       */
/* hardware video/JPEG codec, so a continuous decode loop isn't a good     */
/* fit. Discovers the configured webcam via Moonraker's own webcam         */
/* management API (/server/webcams/list) rather than assuming a port/path */
/* -- same "ask the printer, don't guess" approach AFC capabilities use.   */
/* ---------------------------------------------------------------------- */

#define DT_WEBCAM_HTTP_TIMEOUT_MS    8000
#define DT_WEBCAM_SNAPSHOT_MAX_BYTES (400 * 1024)

static dt_ui_webcam_model_t s_webcam_model;
static uint8_t *s_webcam_jpeg_data;
static char s_webcam_snapshot_url[256];


static void push_webcam_ui(void)
{
    if (!lvgl_port_lock(200)) {
        ESP_LOGW(
            TAG,
            "LVGL lock timeout; skipping webcam UI update"
        );
        return;
    }

    esp_err_t err =
        dt_ui_update_webcam(&s_webcam_model);

    lvgl_port_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dt_ui_update_webcam failed: %s",
            esp_err_to_name(err)
        );
    }
}


/* DT_UI_DETACH_BEFORE_FREE -- see push_files_ui_blocking(). */
static bool push_webcam_ui_blocking(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(
            TAG,
            "LVGL lock unavailable; leaking a snapshot rather than "
            "freeing it while in use"
        );

        return false;
    }

    (void)dt_ui_update_webcam(&s_webcam_model);

    lvgl_port_unlock();

    return true;
}


/*
 * Like http_request_ex(), but against an arbitrary absolute URL instead of
 * s_base_url + path -- the webcam server is very often a different host
 * and/or port than Moonraker's own API. No X-Api-Key header is sent here
 * deliberately: that key belongs to Moonraker, not whatever server the
 * discovered snapshot URL happens to point at.
 */
static esp_err_t http_fetch_url_raw(
    const char *url,
    uint8_t **response_out,
    size_t *response_len_out,
    size_t response_cap,
    uint32_t timeout_ms
)
{
    if (response_out != NULL) {
        *response_out = NULL;
    }

    if (response_len_out != NULL) {
        *response_len_out = 0;
    }

    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *response =
        heap_caps_malloc(
            response_cap,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (response == NULL) {
        return ESP_ERR_NO_MEM;
    }

    http_buffer_t buffer = {
        .data = (char *)response,
        .len = 0,
        .cap = response_cap,
    };

    response[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &buffer,
        .timeout_ms = timeout_ms,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    if (client == NULL) {
        free(response);
        return ESP_FAIL;
    }

    esp_http_client_set_method(
        client,
        HTTP_METHOD_GET
    );

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

    esp_http_client_cleanup(
        client
    );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "webcam snapshot fetch failed url=%s status=%d err=%s",
            url,
            status,
            esp_err_to_name(err)
        );

        free(response);
        return err;
    }

    if (response_len_out != NULL) {
        *response_len_out = buffer.len;
    }

    if (response_out != NULL) {
        *response_out = response;
    } else {
        free(response);
    }

    return ESP_OK;
}


static esp_err_t discover_webcam(void)
{
    if (s_base_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_GET,
            "/server/webcams/list",
            NULL,
            &response,
            NULL
        );

    if (err != ESP_OK) {
        free(response);
        return err;
    }

    cJSON *root =
        cJSON_Parse(response);

    free(response);

    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *payload =
        moonraker_payload(root);

    cJSON *webcams =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "webcams"
        );

    s_webcam_model.configured = false;
    s_webcam_model.name[0] = '\0';
    s_webcam_snapshot_url[0] = '\0';

    if (cJSON_IsArray(webcams)) {
        cJSON *first =
            cJSON_GetArrayItem(
                webcams,
                0
            );

        if (cJSON_IsObject(first)) {
            json_copy_string(
                first,
                "name",
                s_webcam_model.name,
                sizeof(s_webcam_model.name)
            );

            cJSON *snapshot_url =
                cJSON_GetObjectItemCaseSensitive(
                    first,
                    "snapshot_url"
                );

            if (
                cJSON_IsString(snapshot_url) &&
                snapshot_url->valuestring != NULL &&
                snapshot_url->valuestring[0] != '\0'
            ) {
                const char *raw =
                    snapshot_url->valuestring;

                if (
                    strncasecmp(raw, "http://", 7) == 0 ||
                    strncasecmp(raw, "https://", 8) == 0
                ) {
                    snprintf(
                        s_webcam_snapshot_url,
                        sizeof(s_webcam_snapshot_url),
                        "%s",
                        raw
                    );
                } else {
                    /*
                     * A relative snapshot_url is meant to be resolved
                     * against wherever the web UI (Mainsail/Fluidd) is
                     * served from -- normally the standard nginx
                     * front-end on port 80 -- not Moonraker's own API
                     * port. s_base_url targets the API port directly,
                     * so this is built separately rather than reused.
                     */
                    snprintf(
                        s_webcam_snapshot_url,
                        sizeof(s_webcam_snapshot_url),
                        "http://%s%s%s",
                        s_moonraker_host,
                        raw[0] == '/' ? "" : "/",
                        raw
                    );
                }

                s_webcam_model.configured = true;
            }
        }
    }

    cJSON_Delete(root);

    return ESP_OK;
}


/*
 * DT_WEBCAM_JFIF_SHIM
 *
 * LVGL's TJpgDec binding decides whether a buffer is a JPEG with a strict
 * 10-byte signature test: FF D8 FF E0 00 10 "JFIF". mjpg-streamer -- what
 * crowsnest serves for the "mjpegstreamer-adaptive" service -- emits a bare
 * baseline JPEG that goes straight from SOI to SOF0 (FF D8 FF C0 ...) with no
 * APP0 segment at all. That fails the signature test, so TJpgDec declines the
 * image entirely, and LVGL's built-in bin_decoder claims it instead (it accepts
 * any source whose color format isn't UNKNOWN, and LV_COLOR_FORMAT_RAW isn't).
 * bin_decoder then "succeeds" by echoing back our own zeroed header, which is
 * why the decode reported OK at 0x0 and nothing ever drew.
 *
 * Splice a standard JFIF APP0 segment in right after SOI. APP0 is metadata --
 * TJpgDec skips it as an unknown segment -- and the result is a structurally
 * valid JFIF file that passes the signature test.
 */
static esp_err_t webcam_ensure_jfif(
    uint8_t **data,
    size_t *size
)
{
    static const uint8_t jfif_signature[10] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46
    };

    static const uint8_t jfif_app0[18] = {
        0xFF, 0xE0,             /* APP0 marker */
        0x00, 0x10,             /* segment length (16, covers itself) */
        0x4A, 0x46, 0x49, 0x46, /* "JFIF" */
        0x00,                   /* NUL terminator */
        0x01, 0x01,             /* version 1.01 */
        0x00,                   /* density units: none */
        0x00, 0x01,             /* X density */
        0x00, 0x01,             /* Y density */
        0x00,                   /* thumbnail width */
        0x00                    /* thumbnail height */
    };

    uint8_t *src = *data;
    size_t src_size = *size;

    if (src == NULL || src_size < 4) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (src[0] != 0xFF || src[1] != 0xD8) {
        ESP_LOGW(
            TAG,
            "webcam snapshot is not a JPEG (starts %02X %02X)",
            (unsigned)src[0],
            (unsigned)src[1]
        );

        return ESP_ERR_INVALID_RESPONSE;
    }

    if (
        src_size >= sizeof(jfif_signature) &&
        memcmp(src, jfif_signature, sizeof(jfif_signature)) == 0
    ) {
        /* Already carries the APP0 segment LVGL insists on. */
        return ESP_OK;
    }

    size_t padded_size = src_size + sizeof(jfif_app0);

    uint8_t *padded =
        heap_caps_malloc(
            padded_size,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    if (padded == NULL) {
        return ESP_ERR_NO_MEM;
    }

    padded[0] = 0xFF; /* SOI */
    padded[1] = 0xD8;

    memcpy(
        padded + 2,
        jfif_app0,
        sizeof(jfif_app0)
    );

    memcpy(
        padded + 2 + sizeof(jfif_app0),
        src + 2,
        src_size - 2
    );

    free(src);

    *data = padded;
    *size = padded_size;

    return ESP_OK;
}


static esp_err_t webcam_refresh(void)
{
    if (s_base_url[0] == '\0') {
        snprintf(
            s_webcam_model.status_text,
            sizeof(s_webcam_model.status_text),
            "Printer offline."
        );

        push_webcam_ui();

        return ESP_ERR_INVALID_STATE;
    }

    s_webcam_model.loading = true;
    s_webcam_model.error = false;
    s_webcam_model.status_text[0] = '\0';

    push_webcam_ui();

    esp_err_t err = discover_webcam();

    if (
        err != ESP_OK ||
        !s_webcam_model.configured
    ) {
        s_webcam_model.loading = false;
        s_webcam_model.has_image = false;
        s_webcam_model.error = true;

        snprintf(
            s_webcam_model.status_text,
            sizeof(s_webcam_model.status_text),
            err != ESP_OK
                ? "Could not reach Moonraker's webcam list."
                : "Moonraker reports no webcam configured."
        );

        push_webcam_ui();

        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    uint8_t *jpeg_data = NULL;
    size_t jpeg_size = 0;

    err =
        http_fetch_url_raw(
            s_webcam_snapshot_url,
            &jpeg_data,
            &jpeg_size,
            DT_WEBCAM_SNAPSHOT_MAX_BYTES,
            DT_WEBCAM_HTTP_TIMEOUT_MS
        );

    s_webcam_model.loading = false;

    if (
        err != ESP_OK ||
        jpeg_size < 4
    ) {
        free(jpeg_data);

        s_webcam_model.has_image = false;
        s_webcam_model.error = true;

        snprintf(
            s_webcam_model.status_text,
            sizeof(s_webcam_model.status_text),
            "Snapshot fetch failed (%s).",
            esp_err_to_name(err)
        );

        push_webcam_ui();

        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    err =
        webcam_ensure_jfif(
            &jpeg_data,
            &jpeg_size
        );

    if (err != ESP_OK) {
        free(jpeg_data);

        s_webcam_model.has_image = false;
        s_webcam_model.error = true;

        snprintf(
            s_webcam_model.status_text,
            sizeof(s_webcam_model.status_text),
            "Snapshot is not a usable JPEG."
        );

        push_webcam_ui();

        return err;
    }

    free(s_webcam_jpeg_data);
    s_webcam_jpeg_data = jpeg_data;

    s_webcam_model.jpeg_data = s_webcam_jpeg_data;
    s_webcam_model.jpeg_size = (uint32_t)jpeg_size;

    /* DT_WEBCAM_REVISION: new bytes, so the UI must decode them. */
    s_webcam_model.revision++;
    s_webcam_model.has_image = true;
    s_webcam_model.error = false;
    s_webcam_model.status_text[0] = '\0';

    push_webcam_ui();

    return ESP_OK;
}


static esp_err_t query_afc_lane_state(
    dt_ui_job_state_t job_state
)
{
    if (
        s_filament_model == NULL ||
        !s_filament_model->afc_detected ||
        s_afc_lane_object_count == 0
    ) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    cJSON *request =
        cJSON_CreateObject();

    cJSON *objects =
        request != NULL
            ? cJSON_AddObjectToObject(
                request,
                "objects"
            )
            : NULL;

    if (objects == NULL) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNullToObject(
        objects,
        "AFC"
    );

    for (
        size_t i = 0;
        i < s_afc_lane_object_count;
        ++i
    ) {
        cJSON_AddNullToObject(
            objects,
            s_afc_lane_objects[i]
        );
    }

    char *body =
        cJSON_PrintUnformatted(request);

    cJSON_Delete(request);

    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_POST,
            "/printer/objects/query",
            body,
            &response,
            NULL
        );

    cJSON_free(body);

    if (err != ESP_OK) {
        return err;
    }

    cJSON *root =
        cJSON_Parse(response);

    free(response);

    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *payload =
        moonraker_payload(root);

    cJSON *status =
        cJSON_GetObjectItemCaseSensitive(
            payload,
            "status"
        );

    if (!cJSON_IsObject(status)) {
        status = payload;
    }

    cJSON *afc =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "AFC"
        );

    if (cJSON_IsObject(afc)) {
        json_copy_string(
            afc,
            "current_state",
            s_filament_model->afc_state,
            sizeof(s_filament_model->afc_state)
        );

        json_copy_string(
            afc,
            "current_load",
            s_filament_model->afc_current_load,
            sizeof(
                s_filament_model->afc_current_load
            )
        );

        s_filament_model->afc_error =
            json_bool_value(
                afc,
                "error_state"
            );

        s_filament_model->afc_message[0] = '\0';

        cJSON *message =
            cJSON_GetObjectItemCaseSensitive(
                afc,
                "message"
            );

        if (cJSON_IsObject(message)) {
            json_copy_string(
                message,
                "message",
                s_filament_model->afc_message,
                sizeof(
                    s_filament_model->afc_message
                )
            );

            cJSON *message_type =
                cJSON_GetObjectItemCaseSensitive(
                    message,
                    "type"
                );

            if (
                cJSON_IsString(message_type) &&
                message_type->valuestring != NULL &&
                strcasecmp(
                    message_type->valuestring,
                    "error"
                ) == 0
            ) {
                s_filament_model->afc_error = true;
            }
        }
    }

    memset(
        s_filament_model->afc_lanes,
        0,
        sizeof(s_filament_model->afc_lanes)
    );

    size_t lane_count = 0;

    for (
        size_t i = 0;
        i < s_afc_lane_object_count &&
        lane_count < DT_UI_AFC_MAX_LANES;
        ++i
    ) {
        cJSON *lane_object =
            cJSON_GetObjectItemCaseSensitive(
                status,
                s_afc_lane_objects[i]
            );

        if (!cJSON_IsObject(lane_object)) {
            continue;
        }

        dt_ui_afc_lane_t *lane =
            &s_filament_model
                ->afc_lanes[lane_count];

        json_copy_string(
            lane_object,
            "name",
            lane->name,
            sizeof(lane->name)
        );

        json_copy_string(
            lane_object,
            "map",
            lane->map,
            sizeof(lane->map)
        );

        json_copy_string(
            lane_object,
            "material",
            lane->material,
            sizeof(lane->material)
        );

        json_copy_string(
            lane_object,
            "color",
            lane->color,
            sizeof(lane->color)
        );

        json_copy_string(
            lane_object,
            "status",
            lane->status,
            sizeof(lane->status)
        );

        json_copy_string(
            lane_object,
            "filament_status",
            lane->filament_status,
            sizeof(lane->filament_status)
        );

        json_copy_string(
            lane_object,
            "unit",
            lane->unit,
            sizeof(lane->unit)
        );

        cJSON *lane_number =
            cJSON_GetObjectItemCaseSensitive(
                lane_object,
                "lane"
            );

        if (cJSON_IsNumber(lane_number)) {
            lane->lane_number =
                (int)lane_number->valuedouble;
        }

        cJSON *weight =
            cJSON_GetObjectItemCaseSensitive(
                lane_object,
                "weight"
            );

        if (cJSON_IsNumber(weight)) {
            lane->weight_g =
                (float)weight->valuedouble;
        }

        lane->prep =
            json_bool_value(
                lane_object,
                "prep"
            );

        lane->load =
            json_bool_value(
                lane_object,
                "load"
            );

        lane->loaded_to_hub =
            json_bool_value(
                lane_object,
                "loaded_to_hub"
            );

        lane->tool_loaded =
            json_bool_value(
                lane_object,
                "tool_loaded"
            );

        lane_count++;
    }

    s_filament_model->afc_lane_count =
        lane_count;

    s_filament_model->printer_printing =
        job_state == DT_UI_JOB_PRINTING;

    s_filament_model->printer_paused =
        job_state == DT_UI_JOB_PAUSED;

    const bool print_active =
        s_filament_model->printer_printing ||
        s_filament_model->printer_paused;

    const bool afc_idle =
        s_filament_model->afc_state[0] == '\0' ||
        strcasecmp(
            s_filament_model->afc_state,
            "Idle"
        ) == 0;

    s_filament_model->afc_actions_enabled =
        s_filament_model->online &&
        !print_active &&
        !s_filament_model->afc_error &&
        afc_idle;

    cJSON_Delete(root);

    push_filament_ui();

    return ESP_OK;
}


static esp_err_t execute_afc_lane_request(
    const dt_runtime_filament_request_t *request
)
{
    if (
        request == NULL ||
        s_filament_model == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_filament_model->online) {
        return ESP_ERR_INVALID_STATE;
    }

    char script[128] = {0};

    switch (request->request) {
    case DT_UI_FILAMENT_REQUEST_CHANGE_TOOL:
        if (
            request->lane_number <= 0 ||
            !s_filament_model->afc_actions_enabled ||
            !s_filament_model->has_bt_change_tool
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        snprintf(
            script,
            sizeof(script),
            "BT_CHANGE_TOOL LANE=%d",
            request->lane_number
        );
        break;

    case DT_UI_FILAMENT_REQUEST_EJECT_LANE:
        if (
            request->lane_number <= 0 ||
            !s_filament_model->afc_actions_enabled ||
            !s_filament_model->has_bt_lane_eject
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        snprintf(
            script,
            sizeof(script),
            "BT_LANE_EJECT LANE=%d",
            request->lane_number
        );
        break;

    case DT_UI_FILAMENT_REQUEST_CLEAR_MESSAGE:
        if (
            !s_filament_model->has_afc_clear_message ||
            s_filament_model->afc_message[0] == '\0'
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        snprintf(
            script,
            sizeof(script),
            "AFC_CLEAR_MESSAGE"
        );
        break;

    case DT_UI_FILAMENT_REQUEST_RESET_LANE: {
        if (
            request->lane_number <= 0 ||
            s_filament_model->printer_printing ||
            !s_filament_model->has_afc_lane_reset
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        const dt_ui_afc_lane_t *selected = NULL;

        for (
            size_t i = 0;
            i < s_filament_model->afc_lane_count;
            ++i
        ) {
            if (
                s_filament_model->afc_lanes[i].lane_number ==
                request->lane_number
            ) {
                selected =
                    &s_filament_model->afc_lanes[i];
                break;
            }
        }

        if (
            selected == NULL ||
            selected->name[0] == '\0' ||
            !(
                selected->prep ||
                selected->load ||
                selected->loaded_to_hub ||
                selected->tool_loaded
            )
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        snprintf(
            script,
            sizeof(script),
            "AFC_LANE_RESET LANE=%s",
            selected->name
        );
        break;
    }

    case DT_UI_FILAMENT_REQUEST_RESUME:
        if (
            !s_filament_model->has_bt_resume ||
            !s_filament_model->printer_paused ||
            s_filament_model->afc_error
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        snprintf(
            script,
            sizeof(script),
            "BT_RESUME"
        );
        break;

    case DT_UI_FILAMENT_REQUEST_UNLOAD_LANE: {
        /* DT_AFC_UNLOAD_LOADED_LANE */
        if (
            request->lane_number <= 0 ||
            !s_filament_model->afc_actions_enabled ||
            !s_filament_model->has_afc_tool_unload
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        const dt_ui_afc_lane_t *selected = NULL;

        for (
            size_t i = 0;
            i < s_filament_model->afc_lane_count;
            ++i
        ) {
            if (
                s_filament_model->afc_lanes[i].lane_number ==
                request->lane_number
            ) {
                selected =
                    &s_filament_model->afc_lanes[i];
                break;
            }
        }

        if (
            selected == NULL ||
            !selected->tool_loaded
        ) {
            return ESP_ERR_INVALID_STATE;
        }

        snprintf(
            script,
            sizeof(script),
            "TOOL_UNLOAD"
        );
        break;
    }

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(
        TAG,
        "AFC request: %s",
        script
    );

    return run_gcode(script);
}






static void runtime_copy_text(
    char *dest,
    size_t dest_size,
    const char *src
)
{
    if (
        dest == NULL ||
        dest_size == 0
    ) {
        return;
    }

    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    const size_t src_len =
        strlen(src);

    const size_t copy_len =
        src_len < dest_size - 1U
            ? src_len
            : dest_size - 1U;

    memcpy(
        dest,
        src,
        copy_len
    );

    dest[copy_len] = '\0';
}


static void collect_system_model(
    dt_ui_system_model_t *model
)
{
    memset(
        model,
        0,
        sizeof(*model)
    );

    model->wifi_rssi = 0;

    if (s_have_previous) {
        model->printer_connection =
            s_previous.connection;
    } else {
        model->printer_connection =
            s_base_url[0] != '\0'
                ? DT_UI_CONNECTION_CONNECTING
                : DT_UI_CONNECTION_OFFLINE;
    }

    runtime_copy_text(
        model->moonraker_url,
        sizeof(model->moonraker_url),
        s_base_url
    );

    if (s_filament_model != NULL) {
        runtime_copy_text(
            model->filament_mode,
            sizeof(model->filament_mode),
            s_filament_model->mode
        );

        model->afc_lane_count =
            s_filament_model->afc_lane_count;
    }

    wifi_ap_record_t ap = {0};

    if (
        esp_wifi_sta_get_ap_info(
            &ap
        ) == ESP_OK
    ) {
        const size_t ssid_len =
            strnlen(
                (const char *)ap.ssid,
                sizeof(ap.ssid)
            );

        const size_t copy_len =
            ssid_len <
                sizeof(model->wifi_ssid) - 1U
                ? ssid_len
                : sizeof(model->wifi_ssid) - 1U;

        memcpy(
            model->wifi_ssid,
            ap.ssid,
            copy_len
        );

        model->wifi_ssid[copy_len] = '\0';

        model->wifi_rssi =
            (int)ap.rssi;
    }

    esp_netif_t *sta =
        esp_netif_get_handle_from_ifkey(
            "WIFI_STA_DEF"
        );

    if (sta != NULL) {
        esp_netif_ip_info_t ip_info = {0};

        if (
            esp_netif_get_ip_info(
                sta,
                &ip_info
            ) == ESP_OK &&
            ip_info.ip.addr != 0
        ) {
            snprintf(
                model->local_ip,
                sizeof(model->local_ip),
                IPSTR,
                IP2STR(&ip_info.ip)
            );
        }
    }

    const esp_app_desc_t *app =
        esp_app_get_description();

    if (app != NULL) {
        runtime_copy_text(
            model->firmware_version,
            sizeof(model->firmware_version),
            app->version
        );
    }

    runtime_copy_text(
        model->idf_version,
        sizeof(model->idf_version),
        esp_get_idf_version()
    );

    model->internal_free =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL
        );

    model->internal_largest =
        (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL
        );

    model->psram_free =
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM
        );

    model->psram_largest =
        (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_SPIRAM
        );
}


static void push_system_ui(void)
{
    dt_ui_system_model_t model;

    collect_system_model(
        &model
    );

    if (!lvgl_port_lock(200)) {
        ESP_LOGW(
            TAG,
            "LVGL lock timeout; skipping system UI update"
        );
        return;
    }

    esp_err_t err =
        dt_ui_update_system(
            &model
        );

    lvgl_port_unlock();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dt_ui_update_system: %s",
            esp_err_to_name(err)
        );
    }
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
    snap->chamber_c = NAN;
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
            "temperature_sensor%20" DT_PROFILE_CHAMBER_SENSOR
            "=temperature&"
            "fan=speed&"
            "toolhead=position,homed_axes",
            NULL,
            &response,
            NULL
        );

    /*
     * DT_STAGE7B_CONNECTION_SEMANTICS
     *
     * A transport/request failure means Moonraker is unreachable: Offline.
     * If Moonraker responds but Klippy is not ready, the webhooks-state path
     * below continues to report Connecting.
     */
    if (err != ESP_OK) {
        snap->connection =
            DT_UI_CONNECTION_OFFLINE;

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

    const char *web_state_text =
        cJSON_IsString(web_state)
            ? web_state->valuestring
            : "";

    const bool ready =
        strcmp(web_state_text, "ready") == 0;

    /*
     * DT_KLIPPER_ERROR
     *
     * Klipper's webhooks states are ready / startup / shutdown / error.
     * Only startup is genuinely "connecting"; the other two mean it has
     * stopped and will not recover on its own.
     */
    const bool halted =
        strcmp(web_state_text, "error") == 0 ||
        strcmp(web_state_text, "shutdown") == 0;

    snap->connection =
        ready
            ? DT_UI_CONNECTION_ONLINE
            : (halted
                ? DT_UI_CONNECTION_ERROR
                : DT_UI_CONNECTION_CONNECTING);

    snap->status_message[0] = '\0';

    if (halted) {
        const cJSON *web_message =
            cJSON_GetObjectItemCaseSensitive(
                webhooks,
                "message"
            );

        if (
            cJSON_IsString(web_message) &&
            web_message->valuestring != NULL &&
            web_message->valuestring[0] != '\0'
        ) {
            snprintf(
                snap->status_message,
                sizeof(snap->status_message),
                "%s",
                web_message->valuestring
            );
        } else {
            /*
             * webhooks.message is often null even when halted -- a config
             * parse failure, for instance, only shows up in
             * /printer/info.state_message. Fetch that, but only while
             * halted, so the normal poll stays one request.
             */
            char *info = NULL;

            if (
                http_request(
                    HTTP_METHOD_GET,
                    "/printer/info",
                    NULL,
                    &info,
                    NULL
                ) == ESP_OK &&
                info != NULL
            ) {
                cJSON *info_root = cJSON_Parse(info);

                if (info_root != NULL) {
                    const cJSON *info_payload =
                        moonraker_payload(info_root);

                    const cJSON *state_message =
                        cJSON_GetObjectItemCaseSensitive(
                            info_payload,
                            "state_message"
                        );

                    if (
                        cJSON_IsString(state_message) &&
                        state_message->valuestring != NULL
                    ) {
                        snprintf(
                            snap->status_message,
                            sizeof(snap->status_message),
                            "%s",
                            state_message->valuestring
                        );
                    }

                    cJSON_Delete(info_root);
                }
            }

            free(info);
        }

        /*
         * Klipper's message is multi-line; the card's detail label is a
         * single line with ellipsis, so fold it flat rather than letting
         * embedded newlines render as gaps.
         */
        size_t w = 0;

        for (size_t r = 0; snap->status_message[r] != '\0'; ++r) {
            char ch = snap->status_message[r];

            if (ch == '\n' || ch == '\r') {
                if (w == 0 || snap->status_message[w - 1] == ' ') {
                    continue;
                }

                ch = ' ';
            }

            snap->status_message[w++] = ch;
        }

        snap->status_message[w] = '\0';
    }

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

    /*
     * DT_CHAMBER_TEMP
     *
     * Klipper reports this object under its full configured name, spaces
     * and all -- the same string used in the query above.
     */
    cJSON *chamber =
        cJSON_GetObjectItemCaseSensitive(
            status,
            "temperature_sensor " DT_PROFILE_CHAMBER_SENSOR
        );

    json_number(
        chamber,
        "temperature",
        &snap->chamber_c
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


/*
 * DT_STAGE7A_NAN_STABLE_SNAPSHOT
 *
 * NAN is an intentional sentinel for unavailable telemetry. Two NAN values
 * therefore represent the same UI state and must compare equal.
 */
static bool runtime_float_equal(
    float a,
    float b
)
{
    if (isnan(a) && isnan(b)) {
        return true;
    }

    if (!isfinite(a) || !isfinite(b)) {
        return a == b;
    }

    return fabsf(a - b) < 0.05f;
}


static bool snapshot_equal(
    const runtime_snapshot_t *a,
    const runtime_snapshot_t *b
)
{
    return
        a->connection == b->connection &&
        strcmp(
            a->status_message,
            b->status_message
        ) == 0 &&
        a->job == b->job &&
        a->progress_percent ==
            b->progress_percent &&
        a->fan_percent ==
            b->fan_percent &&
        a->aux_fan_revision ==
            b->aux_fan_revision &&
        a->elapsed_seconds ==
            b->elapsed_seconds &&
        a->remaining_seconds ==
            b->remaining_seconds &&
        runtime_float_equal(
            a->nozzle_c,
            b->nozzle_c
        ) &&
        runtime_float_equal(
            a->nozzle_target_c,
            b->nozzle_target_c
        ) &&
        runtime_float_equal(
            a->bed_c,
            b->bed_c
        ) &&
        runtime_float_equal(
            a->bed_target_c,
            b->bed_target_c
        ) &&
        runtime_float_equal(
            a->chamber_c,
            b->chamber_c
        ) &&
        runtime_float_equal(
            a->x,
            b->x
        ) &&
        runtime_float_equal(
            a->y,
            b->y
        ) &&
        runtime_float_equal(
            a->z,
            b->z
        ) &&
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

        .chamber_c =
            snap->chamber_c,

        /* DT_KLIPPER_ERROR */
        .status_message =
            snap->status_message,

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

    model.aux_fan_count =
        s_aux_fan_count;

    if (
        model.aux_fan_count >
        DT_UI_AUX_FAN_MAX
    ) {
        model.aux_fan_count =
            DT_UI_AUX_FAN_MAX;
    }

    for (
        size_t i = 0;
        i < model.aux_fan_count;
        ++i
    ) {
        model.aux_fans[i].name =
            s_aux_fans[i].display_name;
        model.aux_fans[i].kind =
            s_aux_fans[i].kind;
        model.aux_fans[i].percent =
            s_aux_fans[i].percent;
        model.aux_fans[i].speed_known =
            s_aux_fans[i].speed_known;
        model.aux_fans[i].controllable =
            s_aux_fans[i].controllable;
    }

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


/*
 * DT_TOAST
 *
 * Every command the user can trigger goes through run_gcode() or
 * post_endpoint(), and nothing on the polling path does -- so raising the
 * toast here covers the lot without labelling 24 call sites, and can't fire
 * on background traffic.
 */
static void runtime_toast(
    dt_ui_toast_kind_t kind,
    const char *text
)
{
    if (lvgl_port_lock(200)) {
        dt_ui_toast(kind, text);
        lvgl_port_unlock();
    }
}


/*
 * Squash a multi-line script onto one line for display. "G91\nG1 X10
 * F6000\nG90" is three statements the user thinks of as one move.
 */
/*
 * DT_TOAST_RUNNING
 *
 * Moonraker's /printer/gcode/script does not answer until the gcode has
 * FINISHED, and DT_HTTP_TIMEOUT_MS is 3.5s -- so anything slow (G28,
 * Z_TILT_ADJUST, QUAD_GANTRY_LEVEL, BED_MESH_CALIBRATE) times out on the
 * client side while Klipper is still happily working. That is not a
 * failure: the request was delivered, we simply stopped waiting for the
 * result. Reporting it as an error is worse than saying nothing, because
 * the printer visibly does the thing while the screen claims it failed.
 */
static bool command_still_running(esp_err_t err)
{
    return
        err == ESP_ERR_HTTP_EAGAIN ||
        err == ESP_ERR_TIMEOUT;
}


static void toast_summarize(
    const char *script,
    char *out,
    size_t length
)
{
    size_t w = 0;

    for (
        const char *p = script;
        *p != '\0' && w + 1 < length;
        ++p
    ) {
        char ch = *p;

        if (ch == '\n' || ch == '\r') {
            /* Collapse runs of newlines into a single separator. */
            if (w == 0 || out[w - 1] == ' ') {
                continue;
            }

            ch = ' ';
        }

        out[w++] = ch;
    }

    out[w] = '\0';
}


/*
 * Moonraker answers a rejected command with
 * {"error": {"code": 400, "message": "..."}}. Fall back to the transport
 * error when the body is missing or shaped differently -- an unhelpful
 * message is still better than a silent failure.
 */
static void toast_failure_reason(
    const char *response,
    esp_err_t err,
    char *out,
    size_t length
)
{
    if (response != NULL) {
        cJSON *root = cJSON_Parse(response);

        if (root != NULL) {
            cJSON *error =
                cJSON_GetObjectItemCaseSensitive(root, "error");

            cJSON *message =
                cJSON_IsObject(error)
                    ? cJSON_GetObjectItemCaseSensitive(error, "message")
                    : cJSON_GetObjectItemCaseSensitive(root, "message");

            if (
                cJSON_IsString(message) &&
                message->valuestring != NULL &&
                message->valuestring[0] != '\0'
            ) {
                snprintf(out, length, "%s", message->valuestring);
                cJSON_Delete(root);
                return;
            }

            cJSON_Delete(root);
        }
    }

    snprintf(out, length, "%s", esp_err_to_name(err));
}


static esp_err_t post_endpoint(
    const char *path
)
{
    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_POST,
            path,
            NULL,
            &response,
            NULL
        );

    /* DT_TOAST: the endpoint name is what the user pressed, near enough. */
    if (err == ESP_OK) {
        runtime_toast(DT_UI_TOAST_SUCCESS, path);
    } else if (command_still_running(err)) {
        char line[160];

        snprintf(
            line,
            sizeof(line),
            "%s - running",
            path
        );

        runtime_toast(DT_UI_TOAST_INFO, line);
    } else {
        char reason[96];
        toast_failure_reason(response, err, reason, sizeof(reason));

        char line[160];
        snprintf(line, sizeof(line), "%s: %s", path, reason);

        runtime_toast(DT_UI_TOAST_ERROR, line);
    }

    free(response);

    return err;
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

    char *response = NULL;

    esp_err_t err =
        http_request(
            HTTP_METHOD_POST,
            "/printer/gcode/script",
            body,
            &response,
            NULL
        );

    cJSON_free(body);

    char summary[56];
    toast_summarize(script, summary, sizeof(summary));

    if (err == ESP_OK) {
        runtime_toast(DT_UI_TOAST_SUCCESS, summary);
    } else if (command_still_running(err)) {
        char line[160];

        snprintf(
            line,
            sizeof(line),
            "%s - running",
            summary
        );

        runtime_toast(DT_UI_TOAST_INFO, line);
    } else {
        char reason[96];
        toast_failure_reason(response, err, reason, sizeof(reason));

        char line[160];
        snprintf(line, sizeof(line), "%s: %s", summary, reason);

        runtime_toast(DT_UI_TOAST_ERROR, line);
    }

    free(response);

    return err;
}



static esp_err_t start_selected_file(void)
{
    if (s_selected_file[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        !s_have_previous ||
        s_previous.connection != DT_UI_CONNECTION_ONLINE
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        s_have_previous &&
        (
            s_previous.job == DT_UI_JOB_PRINTING ||
            s_previous.job == DT_UI_JOB_PAUSED
        )
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    char encoded[640] = {0};

    if (
        !url_encode_query_value(
            s_selected_file,
            encoded,
            sizeof(encoded)
        )
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    char path[768] = {0};

    snprintf(
        path,
        sizeof(path),
        "/printer/print/start?filename=%s",
        encoded
    );

    return post_endpoint(path);
}



static esp_err_t execute_filament_macro(
    bool load
)
{
    if (
        s_filament_model == NULL ||
        !s_filament_model->online
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool available =
        load
            ? s_filament_model->has_load_macro
            : s_filament_model->has_unload_macro;

    const char *macro =
        load
            ? s_filament_model->load_macro
            : s_filament_model->unload_macro;

    if (
        !available ||
        macro[0] == '\0'
    ) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return run_gcode(macro);
}


static esp_err_t execute_action(
    dt_ui_action_t action
)
{
    /*
     * DT_STAGE7C_ACTION_ONLINE_GUARD
     *
     * UI controls already disable printer actions when offline. This is the
     * worker-side fail-closed guard for queued/stale input.
     */
    if (
        action != DT_UI_ACTION_SYSTEM_REBOOT &&
        action != DT_UI_ACTION_SYSTEM_FACTORY_RESET &&
        (
            !s_have_previous ||
            s_previous.connection != DT_UI_CONNECTION_ONLINE
        )
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        action >= DT_UI_ACTION_AUX_FAN_BASE &&
        action <= DT_UI_ACTION_AUX_FAN_LAST
    ) {
        return execute_aux_fan_action(action);
    }

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

    /* DT_Z_TILT: native [z_tilt] command, not the config's Z_TILT macro */
    case DT_UI_ACTION_Z_TILT:
        return run_gcode(DT_PROFILE_LEVEL_GCODE);

    case DT_UI_ACTION_NOZZLE_220:
        return run_gcode(
            "SET_HEATER_TEMPERATURE "
            "HEATER=extruder TARGET=220"
        );

    case DT_UI_ACTION_BED_60:
        return run_gcode(
            "SET_HEATER_TEMPERATURE "
            "HEATER=heater_bed TARGET=60"
        );

    case DT_UI_ACTION_BED_110:
        return run_gcode(
            "SET_HEATER_TEMPERATURE "
            "HEATER=heater_bed TARGET=110"
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

    case DT_UI_ACTION_FILE_START_SELECTED:
        return start_selected_file();

    case DT_UI_ACTION_FILAMENT_LOAD:
        return execute_filament_macro(true);

    case DT_UI_ACTION_FILAMENT_UNLOAD:
        return execute_filament_macro(false);

    case DT_UI_ACTION_SYSTEM_REBOOT:
        ESP_LOGW(
            TAG,
            "reboot requested from UI"
        );
        vTaskDelay(
            pdMS_TO_TICKS(100)
        );
        esp_restart();
        return ESP_OK;

    case DT_UI_ACTION_SYSTEM_FACTORY_RESET:
        return dt_portal_factory_reset_from_ui();

    /* Home-page quick-access macros; see dt_printer_profile.h */
    case DT_UI_ACTION_QUICK_MACRO_1:
        return run_gcode(DT_PROFILE_QUICK1_GCODE);

    case DT_UI_ACTION_QUICK_MACRO_2:
        return run_gcode(DT_PROFILE_QUICK2_GCODE);

    /* DT_WEBCAM_SNAPSHOT */
    case DT_UI_ACTION_WEBCAM_REFRESH:
        return webcam_refresh();

    /*
     * DT_ESTOP
     *
     * Moonraker's own endpoint rather than an M112 through the gcode
     * queue -- it shuts Klipper down immediately instead of waiting for
     * the queue to drain.
     */
    case DT_UI_ACTION_EMERGENCY_STOP:
        return http_request(
            HTTP_METHOD_POST,
            "/printer/emergency_stop",
            NULL,
            NULL,
            NULL
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



static void ui_file_request_handler(
    dt_ui_file_request_t request,
    const char *path,
    void *ctx
)
{
    (void)ctx;

    if (s_file_queue == NULL) {
        return;
    }

    dt_runtime_file_request_t message = {
        .request = request,
    };

    if (path != NULL) {
        snprintf(
            message.path,
            sizeof(message.path),
            "%s",
            path
        );
    }

    if (
        xQueueSend(
            s_file_queue,
            &message,
            0
        ) != pdTRUE
    ) {
        ESP_LOGW(
            TAG,
            "file request queue full; dropping request %d",
            (int)request
        );
    }
}



static void ui_filament_request_handler(
    dt_ui_filament_request_t request,
    int lane_number,
    void *ctx
)
{
    (void)ctx;

    if (s_filament_queue == NULL) {
        return;
    }

    dt_runtime_filament_request_t message = {
        .request = request,
        .lane_number = lane_number,
    };

    if (
        xQueueSend(
            s_filament_queue,
            &message,
            0
        ) != pdTRUE
    ) {
        ESP_LOGW(
            TAG,
            "filament request queue full; "
            "dropping request %d lane %d",
            (int)request,
            lane_number
        );
    }
}


/*
 * DT_TEMP_ENTRY
 */
static void ui_temperature_request_handler(
    dt_ui_heater_t heater,
    int celsius,
    void *ctx
)
{
    (void)ctx;

    if (s_temperature_queue == NULL) {
        return;
    }

    dt_runtime_temperature_request_t message = {
        .heater = heater,
        .celsius = celsius,
    };

    if (
        xQueueSend(
            s_temperature_queue,
            &message,
            0
        ) != pdTRUE
    ) {
        ESP_LOGW(
            TAG,
            "temperature queue full; dropping heater %d target %d",
            (int)heater,
            celsius
        );
    }
}


static esp_err_t execute_temperature_request(
    const dt_runtime_temperature_request_t *request
)
{
    char script[96];

    snprintf(
        script,
        sizeof(script),
        "SET_HEATER_TEMPERATURE HEATER=%s TARGET=%d",
        request->heater == DT_UI_HEATER_BED
            ? "heater_bed"
            : "extruder",
        request->celsius
    );

    return run_gcode(script);
}


/*
 * DT_PRINTER_LIST
 */
static void push_printer_ui(void)
{
    dt_printer_list_t list;

    if (dt_printers_load(&list) != ESP_OK) {
        return;
    }

    char active[DT_PRINTER_HOST_MAX] = {0};

    (void)dt_printers_active_host(
        active,
        sizeof(active)
    );

    dt_ui_printer_model_t model = {0};

    model.count =
        list.count > DT_UI_PRINTER_MAX
            ? DT_UI_PRINTER_MAX
            : list.count;

    model.full = list.count >= DT_PRINTER_MAX;

    for (size_t i = 0; i < model.count; ++i) {
        snprintf(
            model.entries[i].host,
            sizeof(model.entries[i].host),
            "%s",
            list.entries[i].host
        );

        model.entries[i].port = list.entries[i].port;

        model.entries[i].active =
            active[0] != '\0' &&
            strcmp(active, list.entries[i].host) == 0;
    }

    if (lvgl_port_lock(200)) {
        dt_ui_update_printers(&model);
        lvgl_port_unlock();
    }
}


/*
 * DT_PRINTER_REBIND
 *
 * Re-point the runtime at a different Moonraker without restarting. Runs on
 * the runtime task, never the LVGL task: discovery makes several blocking
 * HTTP calls and would stall the UI for seconds.
 */
static void runtime_rebind(void)
{
    ESP_LOGW(TAG, "rebinding to the selected printer");

    load_moonraker_config();

    /*
     * Detach runtime-owned buffers from the models and PUSH that before
     * freeing them. LVGL draws straight out of these allocations, so
     * releasing one while it is still the widget's source is a use-after-free
     * -- the same ordering files_load_thumbnail() already observes.
     */
    uint8_t *old_thumbnail = NULL;

    if (s_files_model != NULL) {
        old_thumbnail = s_file_thumbnail_data;

        s_file_thumbnail_data = NULL;
        s_files_model->thumbnail_data = NULL;
        s_files_model->thumbnail_size = 0;
        s_files_model->thumbnail_width = 0;
        s_files_model->thumbnail_height = 0;
        s_files_model->entry_count = 0;
        s_files_model->selected = false;
        s_files_model->selected_name[0] = '\0';
        s_files_model->selected_path[0] = '\0';
        s_files_model->has_previous = false;

        /*
         * Back to the root, NOT blank. "gcodes" is seeded once when the
         * model is allocated and never re-seeded, so clearing it here left
         * files_refresh_directory() asking Moonraker for an empty path --
         * which it rejects, and the file list stays empty for good.
         */
        snprintf(
            s_files_model->directory,
            sizeof(s_files_model->directory),
            "gcodes"
        );
    }

    if (push_files_ui_blocking()) {
        free(old_thumbnail);
    }

    uint8_t *old_jpeg = s_webcam_jpeg_data;

    s_webcam_jpeg_data = NULL;
    s_webcam_model.jpeg_data = NULL;
    s_webcam_model.jpeg_size = 0;
    s_webcam_model.has_image = false;
    s_webcam_model.configured = false;
    s_webcam_model.error = false;
    s_webcam_model.loading = false;
    s_webcam_model.status_text[0] = '\0';
    s_webcam_model.name[0] = '\0';
    s_webcam_snapshot_url[0] = '\0';

    /*
     * revision is deliberately NOT reset: it only ever has to differ from
     * what the UI last decoded, and leaving it monotonic avoids handing back
     * a value the UI has already seen.
     */
    if (push_webcam_ui_blocking()) {
        free(old_jpeg);
    }

    /*
     * Wipe the change-detection snapshot so the next poll publishes whatever
     * it finds, rather than comparing the new printer against the old one's
     * readings and deciding nothing changed.
     */
    memset(&s_previous, 0, sizeof(s_previous));

    /*
     * These are already re-entrant -- discover_filament_capabilities() zeroes
     * s_afc_lane_object_count and the has_* flags, discover_aux_fans() is on
     * a periodic timer anyway -- so re-running them is all that rebinding
     * needs.
     */
    if (s_files_model != NULL) {
        (void)files_refresh_directory();
    }

    if (s_filament_model != NULL) {
        (void)discover_filament_capabilities();
        (void)discover_aux_fans();
    }

    push_printer_ui();

    ESP_LOGI(TAG, "rebind complete");
}


static void ui_printer_request_handler(
    dt_ui_printer_request_t request,
    int index,
    const char *host,
    uint16_t port,
    const char *api_key,
    void *ctx
)
{
    (void)ctx;

    if (request == DT_UI_PRINTER_REQUEST_ADD) {
        esp_err_t err =
            dt_printers_add(host, port, api_key);

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "could not remember printer %s: %s",
                host != NULL ? host : "(null)",
                esp_err_to_name(err)
            );
        }

        push_printer_ui();
        return;
    }

    if (index < 0) {
        return;
    }

    if (request == DT_UI_PRINTER_REQUEST_REMOVE) {
        esp_err_t err =
            dt_printers_remove((size_t)index);

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "could not forget printer %d: %s",
                index,
                esp_err_to_name(err)
            );
        }

        push_printer_ui();
        return;
    }

    esp_err_t err =
        dt_printers_apply((size_t)index);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "could not apply printer %d: %s",
            index,
            esp_err_to_name(err)
        );

        push_printer_ui();
        return;
    }

    /*
     * DT_PRINTER_REBIND
     *
     * Hand the work to the runtime task rather than doing it here: this runs
     * on the LVGL task, and rebinding blocks on several HTTP round trips.
     */
    push_printer_ui();

    s_rebind_requested = true;
}


/*
 * DT_MOVE_STEP
 */
static void ui_move_request_handler(
    dt_ui_move_axis_t axis,
    int delta_mm,
    int speed_mms,
    void *ctx
)
{
    (void)ctx;

    if (s_move_queue == NULL) {
        return;
    }

    dt_runtime_move_request_t message = {
        .axis = axis,
        .delta_mm = delta_mm,
        .speed_mms = speed_mms,
    };

    if (
        xQueueSend(
            s_move_queue,
            &message,
            0
        ) != pdTRUE
    ) {
        ESP_LOGW(
            TAG,
            "move queue full; dropping axis %d delta %d",
            (int)axis,
            delta_mm
        );
    }
}


static esp_err_t execute_move_request(
    const dt_runtime_move_request_t *request
)
{
    char script[96];

    if (request->axis == DT_UI_MOVE_AXIS_E) {
        snprintf(
            script,
            sizeof(script),
            "M83\n"
            "G1 E%d F%d",
            request->delta_mm,
            request->speed_mms > 0
                ? request->speed_mms * 60
                : 300
        );
    } else {
        const char axis =
            request->axis == DT_UI_MOVE_AXIS_X ? 'X' :
            request->axis == DT_UI_MOVE_AXIS_Y ? 'Y' : 'Z';

        /* Same feedrates the old fixed-step jog buttons used. */
        int feed =
            request->axis == DT_UI_MOVE_AXIS_Z
                ? 600
                : 6000;

        if (request->speed_mms > 0) {
            feed = request->speed_mms * 60;
        }

        snprintf(
            script,
            sizeof(script),
            "G91\n"
            "G1 %c%d F%d\n"
            "G90",
            axis,
            request->delta_mm,
            feed
        );
    }

    return run_gcode(script);
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
    bool ever_online = false;
    bool previous_online = false;
    TickType_t last_status =
        xTaskGetTickCount() -
        pdMS_TO_TICKS(DT_STATUS_PERIOD_MS);

    if (s_files_model != NULL) {
        (void)files_refresh_directory();
    }

    if (s_filament_model != NULL) {
        (void)discover_filament_capabilities();
                    (void)discover_aux_fans();
    }

    TickType_t last_capability_check =
        xTaskGetTickCount();

    TickType_t last_afc_status =
        xTaskGetTickCount() -
        pdMS_TO_TICKS(DT_AFC_STATUS_PERIOD_MS);

    TickType_t last_aux_fan_status =
        xTaskGetTickCount() -
        pdMS_TO_TICKS(2000);

    push_system_ui();

    TickType_t last_system_update =
        xTaskGetTickCount();

    for (;;) {
        /* DT_PRINTER_REBIND */
        if (s_rebind_requested) {
            s_rebind_requested = false;
            runtime_rebind();
        }

        /* DT_MOVE_STEP */
        dt_runtime_move_request_t move_request;

        while (
            s_move_queue != NULL &&
            xQueueReceive(
                s_move_queue,
                &move_request,
                0
            ) == pdTRUE
        ) {
            esp_err_t move_err =
                execute_move_request(&move_request);

            if (move_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "move request axis %d delta %d failed: %s",
                    (int)move_request.axis,
                    move_request.delta_mm,
                    esp_err_to_name(move_err)
                );
            }
        }

        /* DT_TEMP_ENTRY */
        dt_runtime_temperature_request_t temperature_request;

        while (
            s_temperature_queue != NULL &&
            xQueueReceive(
                s_temperature_queue,
                &temperature_request,
                0
            ) == pdTRUE
        ) {
            esp_err_t temperature_err =
                execute_temperature_request(
                    &temperature_request
                );

            if (temperature_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "temperature request heater %d target %d failed: %s",
                    (int)temperature_request.heater,
                    temperature_request.celsius,
                    esp_err_to_name(temperature_err)
                );
            }
        }

        dt_runtime_filament_request_t filament_request;

        while (
            s_filament_queue != NULL &&
            xQueueReceive(
                s_filament_queue,
                &filament_request,
                0
            ) == pdTRUE
        ) {
            esp_err_t filament_err =
                execute_afc_lane_request(
                    &filament_request
                );

            if (filament_err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "AFC lane request %d lane %d failed: %s",
                    (int)filament_request.request,
                    filament_request.lane_number,
                    esp_err_to_name(filament_err)
                );
            }
        }

        dt_runtime_file_request_t file_request;

        while (
            s_file_queue != NULL &&
            xQueueReceive(
                s_file_queue,
                &file_request,
                0
            ) == pdTRUE
        ) {
            esp_err_t file_err =
                files_handle_request(&file_request);

            if (
                file_err != ESP_OK &&
                file_err != ESP_ERR_INVALID_STATE
            ) {
                ESP_LOGW(
                    TAG,
                    "file request %d failed: %s",
                    (int)file_request.request,
                    esp_err_to_name(file_err)
                );
            }
        }

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

            if (err != ESP_OK && command_still_running(err)) {
                /*
                 * DT_TOAST_RUNNING
                 *
                 * Not an error: Klipper is still executing. Logged at
                 * warning so a slow command is still visible on the wire,
                 * but no longer indistinguishable from a rejection.
                 */
                ESP_LOGW(
                    TAG,
                    "UI action %d still running at timeout (%s)",
                    (int)action,
                    esp_err_to_name(err)
                );
            } else if (err != ESP_OK) {
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

            /*
             * DT_STAGE7B_REDISCOVER_ON_RECONNECT
             *
             * The initial capability discovery happens before the polling
             * loop. After DragonTouch has been online once, any later
             * offline/connecting -> online transition re-discovers printer
             * objects and AFC commands so a Klippy/Moonraker restart cannot
             * leave stale capabilities behind.
             */
            if (
                online &&
                !previous_online
            ) {
                if (
                    ever_online &&
                    s_filament_model != NULL
                ) {
                    ESP_LOGI(
                        TAG,
                        "printer reconnected; refreshing capabilities"
                    );

                    (void)discover_filament_capabilities();
                    (void)discover_aux_fans();
                }

                ever_online = true;
            }

            previous_online = online;

            if (
                online &&
                s_aux_fan_count > 0 &&
                now - last_aux_fan_status >=
                    pdMS_TO_TICKS(2000)
            ) {
                last_aux_fan_status = now;
                (void)query_aux_fan_status();
            } else if (!online) {
                aux_fans_mark_offline();
            }

            /*
             * query_status() zero-initializes the snapshot. Inject the current
             * auxiliary-fan generation after its independent status poll so a
             * fan-only change still reaches dt_ui_update().
             */
            snap.aux_fan_revision =
                s_aux_fan_revision;

            if (s_files_model != NULL) {
                const bool files_was_online =
                    s_files_model->online;

                s_files_model->online = online;

                if (!online) {
                    s_files_model->loading = false;

                    snprintf(
                        s_files_model->error,
                        sizeof(s_files_model->error),
                        "Printer offline"
                    );

                    if (files_was_online) {
                        push_files_ui();
                    }
                } else if (!files_was_online) {
                    /*
                     * The file list may be stale after Moonraker/Klippy was
                     * unavailable. Refresh the current directory once.
                     */
                    (void)files_refresh_directory();
                }
            }

            if (s_filament_model != NULL) {
                s_filament_model->online = online;
                s_filament_model->nozzle_c = snap.nozzle_c;
                s_filament_model->nozzle_target_c =
                    snap.nozzle_target_c;
                s_filament_model->can_extrude =
                    snap.can_extrude;

                push_filament_ui();
            }

            /* DT_WEBCAM_SNAPSHOT */
            if (s_webcam_model.online != online) {
                s_webcam_model.online = online;
                push_webcam_ui();

                /*
                 * DT_WEBCAM_HOME_PANE
                 *
                 * The home page carries a webcam pane and is resident from
                 * boot, so it can't rely on a page-entry refresh the way the
                 * webcam page does. Pull the first frame as soon as the
                 * printer becomes reachable instead.
                 */
                if (
                    online &&
                    !s_webcam_model.has_image &&
                    !s_webcam_model.loading
                ) {
                    webcam_refresh();
                }
            }


            if (
                online &&
                s_filament_model != NULL &&
                s_filament_model->afc_detected &&
                now - last_afc_status >=
                    pdMS_TO_TICKS(
                        DT_AFC_STATUS_PERIOD_MS
                    )
            ) {
                last_afc_status = now;

                (void)query_afc_lane_state(
                    snap.job
                );
            }

            /*
             * DT_KLIPPER_ERROR
             *
             * Only a genuine transport failure counts as unreachable. A
             * halted Klipper still has Moonraker answering every request,
             * so counting it here logged "status unavailable" about a
             * server that was replying perfectly well.
             */
            if (
                snap.connection ==
                DT_UI_CONNECTION_OFFLINE
            ) {
                failure_count++;

                if (
                    failure_count == 1 ||
                    failure_count % 30 == 0
                ) {
                    ESP_LOGW(
                        TAG,
                        "Moonraker unreachable (attempt %u)",
                        failure_count
                    );
                }
            } else {
                failure_count = 0;
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

        const TickType_t capability_now =
            xTaskGetTickCount();

        if (
            capability_now - last_capability_check >=
            pdMS_TO_TICKS(DT_CAPABILITY_PERIOD_MS)
        ) {
            last_capability_check = capability_now;

            if (
                s_filament_model != NULL &&
                s_base_url[0] != '\0'
            ) {
                (void)discover_filament_capabilities();
                    (void)discover_aux_fans();

                if (
                    s_filament_model->afc_detected
                ) {
                    (void)query_afc_lane_state(
                        s_have_previous
                            ? s_previous.job
                            : DT_UI_JOB_IDLE
                    );
                }
            }
        }

        const TickType_t system_now =
            xTaskGetTickCount();

        if (
            system_now - last_system_update >=
            pdMS_TO_TICKS(DT_SYSTEM_PERIOD_MS)
        ) {
            last_system_update =
                system_now;

            push_system_ui();
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


    err = dc_wifi_start();


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


    err = dt_portal_start();


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

    /*
     * DT_PRINTER_LIST
     *
     * Fold whatever is already configured into the list so the printer in
     * use shows up without being re-entered.
     */
    (void)dt_printers_sync_active();
    push_printer_ui();

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


    s_files_model =
        heap_caps_calloc(
            1,
            sizeof(*s_files_model),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (s_files_model == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(
        s_files_model->directory,
        sizeof(s_files_model->directory),
        "gcodes"
    );

    s_filament_model =
        heap_caps_calloc(
            1,
            sizeof(*s_filament_model),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

    if (s_filament_model == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_filament_model->nozzle_c = NAN;
    s_filament_model->nozzle_target_c = NAN;

    s_aux_fans =
        heap_caps_calloc(
            DT_UI_AUX_FAN_MAX,
            sizeof(*s_aux_fans),
            MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
        );

    if (s_aux_fans == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_file_queue =
        xQueueCreate(
            DT_FILE_QUEUE_LEN,
            sizeof(dt_runtime_file_request_t)
        );

    if (s_file_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }


    s_filament_queue =
        xQueueCreate(
            DT_FILAMENT_QUEUE_LEN,
            sizeof(dt_runtime_filament_request_t)
        );

    if (s_filament_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* DT_TEMP_ENTRY */
    s_temperature_queue =
        xQueueCreate(
            DT_TEMPERATURE_QUEUE_LEN,
            sizeof(dt_runtime_temperature_request_t)
        );

    if (s_temperature_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* DT_MOVE_STEP */
    s_move_queue =
        xQueueCreate(
            DT_MOVE_QUEUE_LEN,
            sizeof(dt_runtime_move_request_t)
        );

    if (s_move_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

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

    ESP_ERROR_CHECK(
        dt_ui_set_file_request_handler(
            ui_file_request_handler,
            NULL
        )
    );


    ESP_ERROR_CHECK(
        dt_ui_set_filament_request_handler(
            ui_filament_request_handler,
            NULL
        )
    );

    /* DT_TEMP_ENTRY */
    ESP_ERROR_CHECK(
        dt_ui_set_temperature_request_handler(
            ui_temperature_request_handler,
            NULL
        )
    );

    /* DT_MOVE_STEP */
    ESP_ERROR_CHECK(
        dt_ui_set_move_request_handler(
            ui_move_request_handler,
            NULL
        )
    );

    /* DT_PRINTER_LIST */
    ESP_ERROR_CHECK(
        dt_ui_set_printer_request_handler(
            ui_printer_request_handler,
            NULL
        )
    );


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
