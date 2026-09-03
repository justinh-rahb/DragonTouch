#pragma once

#include <stdbool.h>
#include <stdint.h>

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

esp_err_t dt_ui_set_action_handler(
    dt_ui_action_handler_t handler,
    void *ctx
);

#ifdef __cplusplus
}
#endif
