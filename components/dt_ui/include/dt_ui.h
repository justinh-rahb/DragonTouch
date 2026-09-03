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


typedef enum {
    DT_UI_ACTION_PAUSE = 0,
    DT_UI_ACTION_RESUME,
    DT_UI_ACTION_CANCEL,

    DT_UI_ACTION_HOME_ALL,

    DT_UI_ACTION_JOG_X_NEG,
    DT_UI_ACTION_JOG_X_POS,
    DT_UI_ACTION_JOG_Y_NEG,
    DT_UI_ACTION_JOG_Y_POS,
    DT_UI_ACTION_JOG_Z_NEG,
    DT_UI_ACTION_JOG_Z_POS,

    DT_UI_ACTION_NOZZLE_220,

    DT_UI_ACTION_BED_OFF,
    DT_UI_ACTION_BED_60,
    DT_UI_ACTION_BED_100,

    DT_UI_ACTION_EXTRUDE_10,
    DT_UI_ACTION_RETRACT_10,

    DT_UI_ACTION_FAN_OFF,
    DT_UI_ACTION_FAN_50,
    DT_UI_ACTION_FAN_100,
    DT_UI_ACTION_FILE_START_SELECTED,
    DT_UI_ACTION_FILAMENT_LOAD,
    DT_UI_ACTION_FILAMENT_UNLOAD,
} dt_ui_action_t;

typedef void (*dt_ui_action_handler_t)(
    dt_ui_action_t action,
    void *ctx
);

typedef struct {
    const char *device_name;
    dt_ui_connection_t connection;
    dt_ui_job_state_t job_state;

    const char *filename;
    uint8_t progress_percent;
    uint32_t elapsed_seconds;
    uint32_t remaining_seconds;

    float nozzle_c;
    float nozzle_target_c;
    float bed_c;
    float bed_target_c;
    uint8_t fan_percent;

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

esp_err_t dt_ui_update_filament(
    const dt_ui_filament_model_t *model
);

esp_err_t dt_ui_update_files(
    const dt_ui_files_model_t *model
);

esp_err_t dt_ui_set_filament_request_handler(
    dt_ui_filament_request_handler_t handler,
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
