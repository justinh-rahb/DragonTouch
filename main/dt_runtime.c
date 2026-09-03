#include "dt_runtime.h"
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
#define DT_FILE_QUEUE_LEN     4
#define DT_CAPABILITY_PERIOD_MS 30000
#define DT_AFC_STATUS_PERIOD_MS 2000
#define DT_FILAMENT_QUEUE_LEN 4


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


typedef struct {
    dt_ui_file_request_t request;
    char path[DT_UI_FILE_PATH_MAX];
} dt_runtime_file_request_t;


typedef struct {
    dt_ui_filament_request_t request;
    int lane_number;
} dt_runtime_filament_request_t;


static QueueHandle_t s_action_queue;
static QueueHandle_t s_file_queue;
static QueueHandle_t s_filament_queue;
static char s_afc_lane_objects[DT_UI_AFC_MAX_LANES][64];
static size_t s_afc_lane_object_count;
static dt_ui_files_model_t *s_files_model;
static dt_ui_filament_model_t *s_filament_model;
static char s_selected_file[DT_UI_FILE_PATH_MAX];

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

    s_files_model->online = s_base_url[0] != '\0';
    s_files_model->loading = true;
    s_files_model->error[0] = '\0';
    s_files_model->entry_count = 0;

    memset(
        s_files_model->entries,
        0,
        sizeof(s_files_model->entries)
    );

    push_files_ui();

    if (!s_files_model->online) {
        files_set_error("Printer is not configured");
        return ESP_ERR_INVALID_STATE;
    }

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

    push_files_ui();

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
    }

    cJSON_Delete(root);

    if (s_filament_model->afc_detected) {
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
        "filament capabilities load=%d unload=%d m600=%d afc=%d mmu=%d toolchanger=%d bt_change=%d bt_eject=%d lanes=%u",
        s_filament_model->has_load_macro,
        s_filament_model->has_unload_macro,
        s_filament_model->has_m600,
        s_filament_model->afc_detected,
        s_filament_model->mmu_detected,
        s_filament_model->toolchanger_detected,
        s_filament_model->has_bt_change_tool,
        s_filament_model->has_bt_lane_eject,
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

    const bool print_active =
        job_state == DT_UI_JOB_PRINTING ||
        job_state == DT_UI_JOB_PAUSED;

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
        s_filament_model == NULL ||
        request->lane_number <= 0
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_filament_model->afc_actions_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    char script[96] = {0};

    switch (request->request) {
    case DT_UI_FILAMENT_REQUEST_CHANGE_TOOL:
        if (!s_filament_model->has_bt_change_tool) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        snprintf(
            script,
            sizeof(script),
            "BT_CHANGE_TOOL LANE=%d",
            request->lane_number
        );
        break;

    case DT_UI_FILAMENT_REQUEST_EJECT_LANE:
        if (!s_filament_model->has_bt_lane_eject) {
            return ESP_ERR_NOT_SUPPORTED;
        }

        snprintf(
            script,
            sizeof(script),
            "BT_LANE_EJECT LANE=%d",
            request->lane_number
        );
        break;

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(
        TAG,
        "AFC lane request: %s",
        script
    );

    return run_gcode(script);
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



static esp_err_t start_selected_file(void)
{
    if (s_selected_file[0] == '\0') {
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
    if (s_filament_model == NULL) {
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

    case DT_UI_ACTION_FILE_START_SELECTED:
        return start_selected_file();

    case DT_UI_ACTION_FILAMENT_LOAD:
        return execute_filament_macro(true);

    case DT_UI_ACTION_FILAMENT_UNLOAD:
        return execute_filament_macro(false);

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

    if (s_files_model != NULL) {
        (void)files_refresh_directory();
    }

    if (s_filament_model != NULL) {
        (void)discover_filament_capabilities();
    }

    TickType_t last_capability_check =
        xTaskGetTickCount();

    TickType_t last_afc_status =
        xTaskGetTickCount() -
        pdMS_TO_TICKS(DT_AFC_STATUS_PERIOD_MS);

    for (;;) {
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

            if (s_filament_model != NULL) {
                s_filament_model->online = online;
                s_filament_model->nozzle_c = snap.nozzle_c;
                s_filament_model->nozzle_target_c =
                    snap.nozzle_target_c;
                s_filament_model->can_extrude =
                    snap.can_extrude;

                push_filament_ui();
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
