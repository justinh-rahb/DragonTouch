#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DT_UI_CONNECTION_OFFLINE = 0,
    DT_UI_CONNECTION_CONNECTING,
    DT_UI_CONNECTION_ONLINE,

    /*
     * DT_KLIPPER_ERROR
     *
     * Moonraker is answering, but Klipper is halted (webhooks.state of
     * "error" or "shutdown"). Distinct from CONNECTING: nothing is going to
     * change until someone intervenes, so saying "Connecting" forever is a
     * lie. Appended, not inserted, to leave the existing values alone.
     */
    DT_UI_CONNECTION_ERROR,
} dt_ui_connection_t;

typedef enum {
    DT_UI_JOB_IDLE = 0,
    DT_UI_JOB_PRINTING,
    DT_UI_JOB_PAUSED,
    DT_UI_JOB_COMPLETE,
    DT_UI_JOB_ERROR,
} dt_ui_job_state_t;

typedef enum {
    DT_UI_PAGE_HOME = 0,
    DT_UI_PAGE_CONTROL,
    DT_UI_PAGE_FILES,
    DT_UI_PAGE_FILAMENT,
    DT_UI_PAGE_DEVICES,

    /* DT_WEBCAM_SNAPSHOT */
    DT_UI_PAGE_WEBCAM,

    DT_UI_PAGE_SETTINGS,
} dt_ui_page_t;

#define DT_UI_FILE_ENTRY_MAX 6
#define DT_UI_FILE_NAME_MAX 72
#define DT_UI_FILE_PATH_MAX 192

typedef enum {
    DT_UI_FILE_REQUEST_REFRESH = 0,
    DT_UI_FILE_REQUEST_UP,
    DT_UI_FILE_REQUEST_PREVIOUS,
    DT_UI_FILE_REQUEST_NEXT,
    DT_UI_FILE_REQUEST_OPEN_DIRECTORY,
    DT_UI_FILE_REQUEST_SELECT_FILE,
} dt_ui_file_request_t;

typedef struct {
    char name[DT_UI_FILE_NAME_MAX];
    char path[DT_UI_FILE_PATH_MAX];
    bool is_directory;
    uint32_t size_bytes;
} dt_ui_file_entry_t;

typedef struct {
    bool online;
    bool loading;
    char error[96];

    char directory[DT_UI_FILE_PATH_MAX];
    size_t offset;
    size_t total_entries;
    size_t entry_count;
    bool has_previous;
    bool has_next;

    dt_ui_file_entry_t entries[DT_UI_FILE_ENTRY_MAX];

    bool selected;
    char selected_name[DT_UI_FILE_NAME_MAX];
    char selected_path[DT_UI_FILE_PATH_MAX];
    uint32_t selected_size_bytes;
    uint32_t estimated_seconds;
    float filament_weight_g;
    float filament_length_mm;
    float layer_height_mm;
    char slicer[48];
    char filament_type[48];

    /*
     * DT_FILE_THUMBNAIL_PREVIEW
     *
     * Runtime owns the encoded PNG buffer in PSRAM. The UI only references it.
     */
    const uint8_t *thumbnail_data;
    size_t thumbnail_size;
    uint16_t thumbnail_width;
    uint16_t thumbnail_height;

    char detail_error[96];
} dt_ui_files_model_t;

typedef void (*dt_ui_file_request_handler_t)(
    dt_ui_file_request_t request,
    const char *path,
    void *ctx
);


#define DT_UI_AFC_MAX_LANES 8
#define DT_UI_AFC_LANE_PAGE_SIZE 2

typedef enum {
    DT_UI_FILAMENT_REQUEST_CHANGE_TOOL = 0,
    DT_UI_FILAMENT_REQUEST_EJECT_LANE,
    DT_UI_FILAMENT_REQUEST_CLEAR_MESSAGE,
    DT_UI_FILAMENT_REQUEST_RESET_LANE,
    DT_UI_FILAMENT_REQUEST_RESUME,

    /* DT_AFC_UNLOAD_LOADED_LANE: unload whatever lane is currently in the toolhead */
    DT_UI_FILAMENT_REQUEST_UNLOAD_LANE,
} dt_ui_filament_request_t;

typedef struct {
    char name[24];
    char map[12];
    char material[32];
    char color[16];
    char status[32];
    char filament_status[32];
    char unit[24];

    int lane_number;
    float weight_g;

    bool prep;
    bool load;
    bool loaded_to_hub;
    bool tool_loaded;
} dt_ui_afc_lane_t;

typedef struct {
    bool online;
    bool capabilities_known;
    bool can_extrude;

    float nozzle_c;
    float nozzle_target_c;

    bool has_load_macro;
    bool has_unload_macro;
    bool has_m600;

    bool afc_detected;
    bool mmu_detected;
    bool toolchanger_detected;

    bool has_bt_change_tool;
    bool has_bt_lane_eject;
    bool has_bt_resume;
    bool has_afc_clear_message;
    bool has_afc_lane_reset;

    /* DT_AFC_UNLOAD_LOADED_LANE */
    bool has_afc_tool_unload;

    char load_macro[48];
    char unload_macro[48];
    char mode[64];

    bool afc_error;
    bool afc_actions_enabled;
    bool printer_printing;
    bool printer_paused;
    char afc_state[32];
    char afc_current_load[24];
    char afc_message[192];

    size_t afc_lane_count;
    dt_ui_afc_lane_t afc_lanes[DT_UI_AFC_MAX_LANES];
} dt_ui_filament_model_t;

typedef void (*dt_ui_filament_request_handler_t)(
    dt_ui_filament_request_t request,
    int lane_number,
    void *ctx
);


/*
 * DT_TEMP_ENTRY
 *
 * Arbitrary heater targets entered on the keypad. Parameterless actions
 * can't carry a value, so these go through their own handler the way
 * lane-scoped filament requests do.
 */
typedef enum {
    DT_UI_HEATER_NOZZLE = 0,
    DT_UI_HEATER_BED,
} dt_ui_heater_t;

typedef void (*dt_ui_temperature_request_handler_t)(
    dt_ui_heater_t heater,
    int celsius,
    void *ctx
);


/*
 * DT_MOVE_STEP
 *
 * Jog and extrude distances are chosen on screen, so the request has to
 * carry the value. Whole millimetres throughout: every offered step and
 * speed is an integer, which also keeps the gcode off snprintf's %f.
 */
typedef enum {
    DT_UI_MOVE_AXIS_X = 0,
    DT_UI_MOVE_AXIS_Y,
    DT_UI_MOVE_AXIS_Z,
    DT_UI_MOVE_AXIS_E,
} dt_ui_move_axis_t;

typedef void (*dt_ui_move_request_handler_t)(
    dt_ui_move_axis_t axis,
    int delta_mm,
    int speed_mms,
    void *ctx
);


/*
 * DT_PRINTER_LIST
 *
 * Saved Moonraker instances, for the picker behind the header hostname. The
 * UI only ever displays this and asks for a change -- main/dt_printers.c owns
 * the storage, because a component cannot depend on main.
 */
#define DT_UI_PRINTER_MAX 6

typedef struct {
    char host[64];
    uint16_t port;
    bool active;
} dt_ui_printer_entry_t;

typedef struct {
    uint8_t count;
    bool full;
    dt_ui_printer_entry_t entries[DT_UI_PRINTER_MAX];
} dt_ui_printer_model_t;

typedef enum {
    /* Apply entries[index] and restart so the runtime rebinds cleanly. */
    DT_UI_PRINTER_REQUEST_SELECT = 0,
    /* Remember host/port/api_key; index is unused. */
    DT_UI_PRINTER_REQUEST_ADD,
    /* Forget entries[index]; host/port/api_key are unused. */
    DT_UI_PRINTER_REQUEST_REMOVE,
} dt_ui_printer_request_t;

typedef void (*dt_ui_printer_request_handler_t)(
    dt_ui_printer_request_t request,
    int index,
    const char *host,
    uint16_t port,
    const char *api_key,
    void *ctx
);


typedef struct {
    dt_ui_connection_t printer_connection;

    char wifi_ssid[40];
    int wifi_rssi;
    char local_ip[24];

    char moonraker_url[160];

    char filament_mode[64];
    size_t afc_lane_count;

    char firmware_version[48];
    char idf_version[48];

    uint32_t internal_free;
    uint32_t internal_largest;
    uint32_t psram_free;
    uint32_t psram_largest;
} dt_ui_system_model_t;


/*
 * DT_WEBCAM_SNAPSHOT
 *
 * A single fetch-on-demand JPEG snapshot from Moonraker's configured
 * webcam, not a live stream (see the ESP32-S3's lack of a hardware
 * video/JPEG codec -- a continuous decode loop isn't a good fit here).
 * jpeg_data/jpeg_size are owned by dt_runtime.c, the same way
 * thumbnail_data/thumbnail_size are for file previews -- this struct
 * only ever points at them.
 */
typedef struct {
    bool online;

    /* Moonraker reports at least one webcam with a usable snapshot URL */
    bool configured;
    char name[32];

    bool loading;
    bool has_image;
    bool error;
    char status_text[96];

    const uint8_t *jpeg_data;
    uint32_t jpeg_size;

    /*
     * DT_WEBCAM_REVISION
     *
     * Bumped by dt_runtime.c on each successful fetch. The UI decodes only
     * when this changes -- comparing jpeg_data pointers would be unsafe,
     * since the buffer is freed and re-allocated per frame and can land at
     * the same address with the same size.
     */
    uint32_t revision;
} dt_ui_webcam_model_t;



/*
 * DT_DYNAMIC_AUX_FANS
 *
 * The primary [fan] remains in the legacy fan_percent field. These entries are
 * additional Klipper fan objects discovered at runtime.
 */
#define DT_UI_AUX_FAN_MAX 8
#define DT_UI_AUX_FAN_PRESET_COUNT 3
#define DT_UI_AUX_FAN_LEVEL_COUNT 101 /* DT_DYNAMIC_AUX_FAN_SLIDERS */

typedef enum {
    DT_UI_FAN_KIND_GENERIC = 0,
    DT_UI_FAN_KIND_CONTROLLER,
    DT_UI_FAN_KIND_HEATER,
    DT_UI_FAN_KIND_TEMPERATURE,
} dt_ui_fan_kind_t;

typedef struct {
    const char *name;
    dt_ui_fan_kind_t kind;
    uint8_t percent;
    bool speed_known;
    bool controllable;
} dt_ui_aux_fan_t;

typedef enum {
    DT_UI_ACTION_PAUSE = 0,
    DT_UI_ACTION_RESUME,
    DT_UI_ACTION_CANCEL,

    DT_UI_ACTION_HOME_ALL,

    /* DT_Z_TILT: [z_tilt] leveling pass */
    DT_UI_ACTION_Z_TILT,


    DT_UI_ACTION_NOZZLE_220,

    DT_UI_ACTION_BED_60,
    DT_UI_ACTION_BED_110,

    DT_UI_ACTION_EXTRUDE_10,
    DT_UI_ACTION_RETRACT_10,

    DT_UI_ACTION_FAN_OFF,
    DT_UI_ACTION_FAN_50,
    DT_UI_ACTION_FAN_100,

    /*
     * Reserved contiguous range:
     * slot 0: Off, 50, 100; slot 1: Off, 50, 100; ...
     */
    DT_UI_ACTION_AUX_FAN_BASE,
    DT_UI_ACTION_AUX_FAN_LAST =
        DT_UI_ACTION_AUX_FAN_BASE +
        (DT_UI_AUX_FAN_MAX * DT_UI_AUX_FAN_LEVEL_COUNT) - 1,

    DT_UI_ACTION_FILE_START_SELECTED,
    DT_UI_ACTION_FILAMENT_LOAD,
    DT_UI_ACTION_FILAMENT_UNLOAD,
    DT_UI_ACTION_SYSTEM_REBOOT,
    DT_UI_ACTION_SYSTEM_FACTORY_RESET,

    /* Home-page quick-access macros; names live in dt_printer_profile.h */
    DT_UI_ACTION_QUICK_MACRO_1,
    DT_UI_ACTION_QUICK_MACRO_2,

    /* DT_WEBCAM_SNAPSHOT */
    DT_UI_ACTION_WEBCAM_REFRESH,

    /* DT_ESTOP: Moonraker /printer/emergency_stop */
    DT_UI_ACTION_EMERGENCY_STOP,
} dt_ui_action_t;

typedef void (*dt_ui_action_handler_t)(
    dt_ui_action_t action,
    void *ctx
);

typedef struct {
    const char *device_name;
    dt_ui_connection_t connection;
    dt_ui_job_state_t job_state;

    /* DT_KLIPPER_ERROR: why Klipper stopped; empty unless connection is ERROR */
    const char *status_message;

    const char *filename;
    uint8_t progress_percent;
    uint32_t elapsed_seconds;
    uint32_t remaining_seconds;

    float nozzle_c;
    float nozzle_target_c;
    float bed_c;
    float bed_target_c;

    /*
     * DT_CHAMBER_TEMP
     *
     * [temperature_sensor chamber]. It has no target, so only the current
     * reading is meaningful. NAN when the sensor isn't present.
     */
    float chamber_c;

    uint8_t fan_percent;

    size_t aux_fan_count;
    dt_ui_aux_fan_t aux_fans[DT_UI_AUX_FAN_MAX];

    float x;
    float y;
    float z;
    bool homed_x;
    bool homed_y;
    bool homed_z;

    bool can_pause;
    bool can_resume;
    bool can_cancel;

    bool can_home;
    bool can_jog;
    bool can_heat;
    bool can_extrude;
    bool can_fan;
} dt_ui_model_t;

esp_err_t dt_ui_create(lv_display_t *display);
esp_err_t dt_ui_update(const dt_ui_model_t *model);
esp_err_t dt_ui_show_page(dt_ui_page_t page);

esp_err_t dt_ui_update_system(
    const dt_ui_system_model_t *model
);

esp_err_t dt_ui_update_filament(
    const dt_ui_filament_model_t *model
);

esp_err_t dt_ui_update_files(
    const dt_ui_files_model_t *model
);

esp_err_t dt_ui_update_webcam(
    const dt_ui_webcam_model_t *model
);

esp_err_t dt_ui_set_filament_request_handler(
    dt_ui_filament_request_handler_t handler,
    void *ctx
);

/*
 * DT_TOAST
 *
 * Transient status for a command that was just sent. Errors linger longer
 * than confirmations. Caller holds the LVGL lock, as with dt_ui_update_*.
 */
typedef enum {
    DT_UI_TOAST_INFO = 0,
    DT_UI_TOAST_SUCCESS,
    DT_UI_TOAST_ERROR,
} dt_ui_toast_kind_t;

esp_err_t dt_ui_toast(
    dt_ui_toast_kind_t kind,
    const char *text
);

/* DT_PRINTER_LIST */
esp_err_t dt_ui_update_printers(
    const dt_ui_printer_model_t *model
);

esp_err_t dt_ui_set_printer_request_handler(
    dt_ui_printer_request_handler_t handler,
    void *ctx
);

/* DT_TEMP_ENTRY */
esp_err_t dt_ui_set_temperature_request_handler(
    dt_ui_temperature_request_handler_t handler,
    void *ctx
);

/* DT_MOVE_STEP */
esp_err_t dt_ui_set_move_request_handler(
    dt_ui_move_request_handler_t handler,
    void *ctx
);

esp_err_t dt_ui_set_file_request_handler(
    dt_ui_file_request_handler_t handler,
    void *ctx
);

esp_err_t dt_ui_set_action_handler(
    dt_ui_action_handler_t handler,
    void *ctx
);

#ifdef __cplusplus
}
#endif
