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
    bool can_pause;
    bool can_resume;
    bool can_cancel;
} dt_ui_model_t;

/**
 * Create the DragonTouch shell on an initialized LVGL display.
 *
 * The caller owns LVGL initialization, display registration, tick delivery, and
 * locking. No hardware command callbacks are installed by this component yet.
 */
esp_err_t dt_ui_create(lv_display_t *display);

/** Update the passive dashboard view. Call while holding the LVGL lock. */
esp_err_t dt_ui_update(const dt_ui_model_t *model);

#ifdef __cplusplus
}
#endif
