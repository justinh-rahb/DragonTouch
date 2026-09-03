#!/usr/bin/env python3
# DragonTouch Stage 2:
#   - full Moonraker HTTP telemetry
#   - live Home data
#   - functional Control page
#   - async command queue
#
# Run from ~/projects/DragonTouch

from pathlib import Path
import re
import shutil
import sys
import time

ROOT = Path.cwd().resolve()
UI_H = ROOT / "components/dt_ui/include/dt_ui.h"
UI_C = ROOT / "components/dt_ui/dt_ui.c"
RUNTIME_C = ROOT / "main/dt_runtime.c"
RUNTIME_H = ROOT / "main/dt_runtime.h"
CMAKE = ROOT / "main/CMakeLists.txt"

for p in (UI_H, UI_C, RUNTIME_C, RUNTIME_H, CMAKE):
    if not p.exists():
        sys.exit(f"ERROR: missing {p}")

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = ROOT / "backups" / f"pre-stage2-controls-{stamp}"

for p in (UI_H, UI_C, RUNTIME_C, RUNTIME_H, CMAKE):
    dst = backup / p.relative_to(ROOT)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p, dst)

print(f"Backup: {backup}")

def find_function(text, signature):
    start = text.find(signature)
    if start < 0:
        raise RuntimeError(f"missing function: {signature}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"missing opening brace: {signature}")

    depth = 0
    i = brace
    in_string = in_char = in_line = in_block = escape = False

    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""

        if in_line:
            if c == "\n":
                in_line = False
            i += 1
            continue

        if in_block:
            if c == "*" and n == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                in_char = False
            i += 1
            continue

        if c == "/" and n == "/":
            in_line = True
            i += 2
            continue
        if c == "/" and n == "*":
            in_block = True
            i += 2
            continue
        if c == '"':
            in_string = True
            i += 1
            continue
        if c == "'":
            in_char = True
            i += 1
            continue

        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1

    raise RuntimeError(f"missing closing brace: {signature}")

def replace_function(text, signature, replacement):
    a, b = find_function(text, signature)
    return text[:a] + replacement + text[b:]

# ---------------------------------------------------------------------------
# dt_ui.h
# ---------------------------------------------------------------------------

UI_H.write_text(r'''#pragma once

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
''')

# ---------------------------------------------------------------------------
# dt_ui.c
# ---------------------------------------------------------------------------

ui = UI_C.read_text()

if '#include <math.h>' not in ui:
    ui = ui.replace(
        '#include <stdio.h>\n',
        '#include <stdio.h>\n#include <math.h>\n',
        1
    )

# Make all plain labels explicitly non-interactive. This preserves the nav-icon
# lesson across the rest of the UI and prevents decorative children from
# becoming hit-test targets.
old_make_label = '''static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color_hex)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color(color_hex), 0);
    return label;
}'''

new_make_label = '''static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color_hex)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color(color_hex), 0);

    lv_obj_remove_flag(
        label,
        LV_OBJ_FLAG_CLICKABLE |
        LV_OBJ_FLAG_CLICK_FOCUSABLE |
        LV_OBJ_FLAG_SCROLLABLE
    );

    return label;
}'''

if old_make_label in ui:
    ui = ui.replace(old_make_label, new_make_label, 1)

# Extend private UI state.
needle = '''    lv_obj_t *dialog_confirm_label;
    bool ready;
} dt_ui_state_t;'''

replacement = '''    lv_obj_t *dialog_confirm_label;

    lv_obj_t *home_button;
    lv_obj_t *heat_button;
    lv_obj_t *extrude_button;
    lv_obj_t *retract_button;

    lv_obj_t *jog_buttons[6];
    lv_obj_t *bed_buttons[3];
    lv_obj_t *fan_buttons[3];

    lv_obj_t *axes_text;
    lv_obj_t *nozzle_control_text;
    lv_obj_t *bed_control_text;
    lv_obj_t *fan_control_text;

    dt_ui_job_state_t current_job_state;

    bool ready;
} dt_ui_state_t;'''

if needle not in ui:
    sys.exit("ERROR: dt_ui_state_t anchor not found")
ui = ui.replace(needle, replacement, 1)

needle = '''static const char *TAG = "dt_ui";
static dt_ui_state_t s_ui;'''

replacement = '''static const char *TAG = "dt_ui";
static dt_ui_state_t s_ui;

static dt_ui_action_handler_t s_action_handler;
static void *s_action_ctx;
static dt_ui_action_t s_pending_action;'''

if needle not in ui:
    sys.exit("ERROR: UI global anchor missing")
ui = ui.replace(needle, replacement, 1)

# Replace confirmation/action block.
start = ui.find("typedef struct {\n    const char *title;\n    const char *body;\n    const char *confirm_label;")
end_marker = "static void size_card_action(lv_obj_t *button)"
end = ui.find(end_marker)

if start < 0 or end < 0 or end <= start:
    sys.exit("ERROR: confirmation/action block not found")

action_block = r'''typedef struct {
    const char *title;
    const char *body;
    const char *confirm_label;
    bool destructive;
    dt_ui_action_t action;
} dt_confirmation_t;

static const dt_confirmation_t CONFIRM_CANCEL = {
    "Stop this print?",
    "Stopping cannot be undone. Moonraker will request the printer to cancel the active job.",
    "Stop print",
    true,
    DT_UI_ACTION_CANCEL,
};

static const dt_confirmation_t CONFIRM_HOME = {
    "Home all axes?",
    "The printer will execute G28. Keep the motion envelope clear.",
    "Home all axes",
    false,
    DT_UI_ACTION_HOME_ALL,
};

static const dt_confirmation_t CONFIRM_HEAT = {
    "Set nozzle to 220 °C?",
    "The printer remains authoritative over heater safety and limits.",
    "Set 220 °C",
    false,
    DT_UI_ACTION_NOZZLE_220,
};

static const dt_confirmation_t CONFIRM_EXTRUDE = {
    "Extrude 10 mm?",
    "Klipper will reject the request if the active extruder is below its minimum extrusion temperature.",
    "Extrude 10 mm",
    false,
    DT_UI_ACTION_EXTRUDE_10,
};

static const dt_confirmation_t CONFIRM_RETRACT = {
    "Retract 10 mm?",
    "Klipper will reject the request if the active extruder is below its minimum extrusion temperature.",
    "Retract 10 mm",
    false,
    DT_UI_ACTION_RETRACT_10,
};


static void dispatch_action(dt_ui_action_t action)
{
    if (s_action_handler != NULL) {
        s_action_handler(action, s_action_ctx);
    }
}


static void direct_action_event(lv_event_t *event)
{
    dispatch_action(
        (dt_ui_action_t)(uintptr_t)
        lv_event_get_user_data(event)
    );
}


static void pause_resume_event(lv_event_t *event)
{
    (void)event;

    dispatch_action(
        s_ui.current_job_state == DT_UI_JOB_PAUSED
            ? DT_UI_ACTION_RESUME
            : DT_UI_ACTION_PAUSE
    );
}


static lv_obj_t *make_direct_action(
    lv_obj_t *parent,
    const char *text,
    dt_ui_action_t action
)
{
    lv_obj_t *button =
        make_action(parent, text, false);

    lv_obj_add_event_cb(
        button,
        direct_action_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)action
    );

    return button;
}


static void close_dialog(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(
        s_ui.dialog_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void confirm_dialog_event(lv_event_t *event)
{
    (void)event;

    dispatch_action(s_pending_action);

    lv_obj_add_flag(
        s_ui.dialog_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void show_confirmation(
    const dt_confirmation_t *confirmation
)
{
    s_pending_action =
        confirmation->action;

    lv_label_set_text(
        s_ui.dialog_title,
        confirmation->title
    );

    lv_label_set_text(
        s_ui.dialog_body,
        confirmation->body
    );

    lv_label_set_text(
        s_ui.dialog_confirm_label,
        confirmation->confirm_label
    );

    lv_obj_set_style_bg_color(
        s_ui.dialog_confirm,
        color(
            confirmation->destructive
                ? DT_COLOR_WARNING
                : DT_COLOR_ACCENT
        ),
        0
    );

    lv_obj_set_style_text_color(
        s_ui.dialog_confirm_label,
        color(DT_COLOR_TEXT),
        0
    );

    set_button_enabled(
        s_ui.dialog_confirm,
        s_action_handler != NULL
    );

    lv_obj_remove_flag(
        s_ui.dialog_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(
        s_ui.dialog_scrim
    );
}


static void open_dialog(lv_event_t *event)
{
    show_confirmation(
        lv_event_get_user_data(event)
    );
}


#ifdef DT_UI_HOST_PREVIEW
void dt_ui_preview_show_confirmation(
    const char *name
)
{
    if (strcmp(name, "cancel") == 0) {
        show_confirmation(&CONFIRM_CANCEL);
    } else if (strcmp(name, "home") == 0) {
        show_confirmation(&CONFIRM_HOME);
    } else if (strcmp(name, "heat") == 0) {
        show_confirmation(&CONFIRM_HEAT);
    } else if (strcmp(name, "extrude") == 0) {
        show_confirmation(&CONFIRM_EXTRUDE);
    }
}
#endif


static lv_obj_t *make_guarded_action(
    lv_obj_t *parent,
    const char *text,
    const dt_confirmation_t *confirmation
)
{
    lv_obj_t *button =
        make_action(parent, text, false);

    lv_obj_add_event_cb(
        button,
        open_dialog,
        LV_EVENT_CLICKED,
        (void *)confirmation
    );

    return button;
}


'''

ui = ui[:start] + action_block + ui[end:]

# Replace Control page wholesale.
new_control_page = r'''static void create_control_page(lv_obj_t *page)
{
    static const char *names[] = {
        "Motion",
        "Temperature",
        "Extrusion",
        "Fans"
    };

    lv_obj_t *tabs = create_page_heading(
        page,
        "Control",
        "Commands are sent asynchronously through Moonraker."
    );

    for (
        size_t i = 0;
        i < DT_CONTROL_TAB_COUNT;
        ++i
    ) {
        s_ui.control_tabs[i] =
            create_tab(
                tabs,
                names[i],
                control_tab_event,
                i
            );

        s_ui.control_panels[i] =
            create_tab_panel(page);
    }

    /*
     * MOTION
     */
    lv_obj_t *motion =
        make_control_card(
            s_ui.control_panels[0],
            "AXES",
            "Position unavailable"
        );

    s_ui.axes_text =
        lv_obj_get_child(motion, 1);

    s_ui.home_button =
        make_guarded_action(
            motion,
            "Home all axes",
            &CONFIRM_HOME
        );

    size_card_action(
        s_ui.home_button
    );

    lv_obj_t *jog =
        make_control_card(
            s_ui.control_panels[0],
            "JOG",
            "XY ±10 mm   Z ±1 mm"
        );

    lv_obj_t *jog_row =
        lv_obj_create(jog);

    lv_obj_remove_style_all(jog_row);
    lv_obj_set_size(
        jog_row,
        LV_PCT(100),
        84
    );
    lv_obj_set_layout(
        jog_row,
        LV_LAYOUT_FLEX
    );
    lv_obj_set_flex_flow(
        jog_row,
        LV_FLEX_FLOW_ROW_WRAP
    );
    lv_obj_set_style_pad_row(
        jog_row,
        4,
        0
    );
    lv_obj_set_style_pad_column(
        jog_row,
        4,
        0
    );

    static const char *jog_names[] = {
        "X -",
        "X +",
        "Y -",
        "Y +",
        "Z -",
        "Z +"
    };

    static const dt_ui_action_t jog_actions[] = {
        DT_UI_ACTION_JOG_X_NEG,
        DT_UI_ACTION_JOG_X_POS,
        DT_UI_ACTION_JOG_Y_NEG,
        DT_UI_ACTION_JOG_Y_POS,
        DT_UI_ACTION_JOG_Z_NEG,
        DT_UI_ACTION_JOG_Z_POS
    };

    for (size_t i = 0; i < 6; ++i) {
        s_ui.jog_buttons[i] =
            make_direct_action(
                jog_row,
                jog_names[i],
                jog_actions[i]
            );

        lv_obj_set_size(
            s_ui.jog_buttons[i],
            92,
            38
        );

        lv_obj_set_flex_grow(
            s_ui.jog_buttons[i],
            0
        );
    }

    /*
     * TEMPERATURE
     */
    lv_obj_t *nozzle =
        make_control_card(
            s_ui.control_panels[1],
            "NOZZLE",
            "Unavailable"
        );

    s_ui.nozzle_control_text =
        lv_obj_get_child(nozzle, 1);

    s_ui.heat_button =
        make_guarded_action(
            nozzle,
            "Set 220 °C",
            &CONFIRM_HEAT
        );

    size_card_action(
        s_ui.heat_button
    );

    lv_obj_t *bed =
        make_control_card(
            s_ui.control_panels[1],
            "BED",
            "Unavailable"
        );

    s_ui.bed_control_text =
        lv_obj_get_child(bed, 1);

    lv_obj_t *bed_row =
        lv_obj_create(bed);

    lv_obj_remove_style_all(bed_row);
    lv_obj_set_size(
        bed_row,
        LV_PCT(100),
        42
    );
    lv_obj_set_layout(
        bed_row,
        LV_LAYOUT_FLEX
    );
    lv_obj_set_flex_flow(
        bed_row,
        LV_FLEX_FLOW_ROW
    );
    lv_obj_set_style_pad_column(
        bed_row,
        4,
        0
    );

    static const char *bed_names[] = {
        "Off",
        "60 °C",
        "100 °C"
    };

    static const dt_ui_action_t bed_actions[] = {
        DT_UI_ACTION_BED_OFF,
        DT_UI_ACTION_BED_60,
        DT_UI_ACTION_BED_100
    };

    for (size_t i = 0; i < 3; ++i) {
        s_ui.bed_buttons[i] =
            make_direct_action(
                bed_row,
                bed_names[i],
                bed_actions[i]
            );
    }

    /*
     * EXTRUSION
     */
    lv_obj_t *extrude =
        make_control_card(
            s_ui.control_panels[2],
            "ACTIVE EXTRUDER",
            "10 mm manual move"
        );

    s_ui.extrude_button =
        make_guarded_action(
            extrude,
            "Extrude 10 mm",
            &CONFIRM_EXTRUDE
        );

    size_card_action(
        s_ui.extrude_button
    );

    lv_obj_t *retract =
        make_control_card(
            s_ui.control_panels[2],
            "RETRACT",
            "10 mm manual move"
        );

    s_ui.retract_button =
        make_guarded_action(
            retract,
            "Retract 10 mm",
            &CONFIRM_RETRACT
        );

    size_card_action(
        s_ui.retract_button
    );

    /*
     * FAN
     */
    lv_obj_t *part_fan =
        make_control_card(
            s_ui.control_panels[3],
            "PART FAN",
            "Unavailable"
        );

    s_ui.fan_control_text =
        lv_obj_get_child(part_fan, 1);

    lv_obj_t *fan_row =
        lv_obj_create(part_fan);

    lv_obj_remove_style_all(fan_row);
    lv_obj_set_size(
        fan_row,
        LV_PCT(100),
        42
    );
    lv_obj_set_layout(
        fan_row,
        LV_LAYOUT_FLEX
    );
    lv_obj_set_flex_flow(
        fan_row,
        LV_FLEX_FLOW_ROW
    );
    lv_obj_set_style_pad_column(
        fan_row,
        4,
        0
    );

    static const char *fan_names[] = {
        "Off",
        "50%",
        "100%"
    };

    static const dt_ui_action_t fan_actions[] = {
        DT_UI_ACTION_FAN_OFF,
        DT_UI_ACTION_FAN_50,
        DT_UI_ACTION_FAN_100
    };

    for (size_t i = 0; i < 3; ++i) {
        s_ui.fan_buttons[i] =
            make_direct_action(
                fan_row,
                fan_names[i],
                fan_actions[i]
            );
    }

    make_control_card(
        s_ui.control_panels[3],
        "AUXILIARY FANS",
        "Printer-specific auxiliary fan discovery comes in the device-capability layer."
    );

    select_tab(
        s_ui.control_tabs,
        s_ui.control_panels,
        DT_CONTROL_TAB_COUNT,
        0
    );
}'''

try:
    ui = replace_function(
        ui,
        "static void create_control_page(lv_obj_t *page)",
        new_control_page
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")

# Wire Home pause/cancel buttons.
needle = '''    s_ui.pause_button = make_action(actions, "Pause", true);
    s_ui.pause_label = lv_obj_get_child(s_ui.pause_button, 0);
    s_ui.cancel_button = make_action(actions, "Cancel", false);
    style_destructive_action(s_ui.cancel_button);
    lv_obj_add_event_cb(s_ui.cancel_button, open_dialog, LV_EVENT_CLICKED, (void *)&CONFIRM_CANCEL);'''

replacement = '''    s_ui.pause_button = make_action(actions, "Pause", true);
    s_ui.pause_label = lv_obj_get_child(s_ui.pause_button, 0);

    lv_obj_add_event_cb(
        s_ui.pause_button,
        pause_resume_event,
        LV_EVENT_CLICKED,
        NULL
    );

    s_ui.cancel_button = make_action(actions, "Cancel", false);
    style_destructive_action(s_ui.cancel_button);

    lv_obj_add_event_cb(
        s_ui.cancel_button,
        open_dialog,
        LV_EVENT_CLICKED,
        (void *)&CONFIRM_CANCEL
    );'''

if needle not in ui:
    sys.exit("ERROR: Home action block not found")
ui = ui.replace(needle, replacement, 1)

# Wire dialog confirm.
needle = '''    s_ui.dialog_confirm = make_action(actions, "Request", true);
    s_ui.dialog_confirm_label = lv_obj_get_child(s_ui.dialog_confirm, 0);
    set_button_enabled(s_ui.dialog_confirm, false);'''

replacement = '''    s_ui.dialog_confirm = make_action(actions, "Request", true);
    s_ui.dialog_confirm_label = lv_obj_get_child(s_ui.dialog_confirm, 0);

    lv_obj_add_event_cb(
        s_ui.dialog_confirm,
        confirm_dialog_event,
        LV_EVENT_CLICKED,
        NULL
    );

    set_button_enabled(
        s_ui.dialog_confirm,
        false
    );'''

if needle not in ui:
    sys.exit("ERROR: dialog confirm block missing")
ui = ui.replace(needle, replacement, 1)

# Replace dt_ui_update with extended live update.
new_update = r'''esp_err_t dt_ui_update(const dt_ui_model_t *model)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ui.current_job_state =
        model->job_state;

    lv_label_set_text(
        s_ui.device_name,
        model->device_name != NULL
            ? model->device_name
            : "No printer"
    );

    const bool has_device =
        model->device_name != NULL &&
        model->device_name[0] != '\0';

    lv_label_set_text(
        s_ui.printer_name,
        has_device
            ? model->device_name
            : "No paired printer"
    );

    lv_label_set_text(
        s_ui.printer_hint,
        has_device
            ? "Selected same-LAN printer"
            : "Pair a same-LAN device to begin."
    );

    const char *connection = "Offline";
    uint32_t connection_color =
        DT_COLOR_MUTED;

    if (
        model->connection ==
        DT_UI_CONNECTION_CONNECTING
    ) {
        connection = "Connecting";
        connection_color =
            DT_COLOR_WARNING;

    } else if (
        model->connection ==
        DT_UI_CONNECTION_ONLINE
    ) {
        connection = "Online";
        connection_color =
            DT_COLOR_SUCCESS;
    }

    lv_label_set_text(
        s_ui.connection_text,
        connection
    );

    lv_obj_set_style_bg_color(
        s_ui.connection_dot,
        color(connection_color),
        0
    );

    lv_label_set_text(
        s_ui.job_state,
        job_state_text(
            model->job_state
        )
    );

    lv_obj_set_style_text_color(
        s_ui.job_state,
        color(
            model->job_state ==
                DT_UI_JOB_ERROR
                ? DT_COLOR_WARNING
                : DT_COLOR_TEXT
        ),
        0
    );

    lv_label_set_text(
        s_ui.filename,
        model->filename != NULL &&
        model->filename[0] != '\0'
            ? model->filename
            : "No active file"
    );

    uint8_t progress =
        model->progress_percent > 100
            ? 100
            : model->progress_percent;

    lv_bar_set_value(
        s_ui.progress,
        progress,
        LV_ANIM_OFF
    );

    lv_label_set_text_fmt(
        s_ui.progress_text,
        "%u%%",
        progress
    );

    char elapsed[20];
    char remaining[20];
    char time_line[64];

    format_duration(
        elapsed,
        sizeof(elapsed),
        model->elapsed_seconds
    );

    format_duration(
        remaining,
        sizeof(remaining),
        model->remaining_seconds
    );

    snprintf(
        time_line,
        sizeof(time_line),
        "Elapsed %s  •  Remaining %s",
        elapsed,
        remaining
    );

    lv_label_set_text(
        s_ui.time_text,
        time_line
    );

    char temperature[48];

    if (
        model->connection ==
        DT_UI_CONNECTION_ONLINE
    ) {
        if (
            isfinite(model->nozzle_c) &&
            isfinite(model->nozzle_target_c)
        ) {
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

            if (
                s_ui.nozzle_control_text != NULL
            ) {
                lv_label_set_text(
                    s_ui.nozzle_control_text,
                    temperature
                );
            }

        } else {
            lv_label_set_text(
                s_ui.nozzle_text,
                "Unavailable"
            );
        }

        if (
            isfinite(model->bed_c) &&
            isfinite(model->bed_target_c)
        ) {
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

            if (
                s_ui.bed_control_text != NULL
            ) {
                lv_label_set_text(
                    s_ui.bed_control_text,
                    temperature
                );
            }

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

            if (
                s_ui.fan_control_text != NULL
            ) {
                lv_label_set_text_fmt(
                    s_ui.fan_control_text,
                    "%u%%",
                    model->fan_percent
                );
            }

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
    }

    if (s_ui.axes_text != NULL) {
        lv_label_set_text_fmt(
            s_ui.axes_text,
            "X %.1f   Y %.1f   Z %.1f mm   Home %c%c%c",
            model->x,
            model->y,
            model->z,
            model->homed_x ? 'X' : '-',
            model->homed_y ? 'Y' : '-',
            model->homed_z ? 'Z' : '-'
        );
    }

    const bool paused =
        model->job_state ==
        DT_UI_JOB_PAUSED;

    lv_label_set_text(
        s_ui.pause_label,
        paused
            ? "Resume"
            : "Pause"
    );

    set_button_enabled(
        s_ui.pause_button,
        paused
            ? model->can_resume
            : model->can_pause
    );

    set_button_enabled(
        s_ui.cancel_button,
        model->can_cancel
    );

    if (s_ui.home_button != NULL) {
        set_button_enabled(
            s_ui.home_button,
            model->can_home
        );
    }

    for (size_t i = 0; i < 6; ++i) {
        if (s_ui.jog_buttons[i] != NULL) {
            set_button_enabled(
                s_ui.jog_buttons[i],
                model->can_jog
            );
        }
    }

    if (s_ui.heat_button != NULL) {
        set_button_enabled(
            s_ui.heat_button,
            model->can_heat
        );
    }

    for (size_t i = 0; i < 3; ++i) {
        if (s_ui.bed_buttons[i] != NULL) {
            set_button_enabled(
                s_ui.bed_buttons[i],
                model->can_heat
            );
        }

        if (s_ui.fan_buttons[i] != NULL) {
            set_button_enabled(
                s_ui.fan_buttons[i],
                model->can_fan
            );
        }
    }

    if (s_ui.extrude_button != NULL) {
        set_button_enabled(
            s_ui.extrude_button,
            model->can_extrude
        );
    }

    if (s_ui.retract_button != NULL) {
        set_button_enabled(
            s_ui.retract_button,
            model->can_extrude
        );
    }

    return ESP_OK;
}'''

try:
    ui = replace_function(
        ui,
        "esp_err_t dt_ui_update(const dt_ui_model_t *model)",
        new_update
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")

# Append action handler API if absent.
if "esp_err_t dt_ui_set_action_handler(" not in ui:
    ui += r'''

esp_err_t dt_ui_set_action_handler(
    dt_ui_action_handler_t handler,
    void *ctx
)
{
    s_action_handler = handler;
    s_action_ctx = ctx;
    return ESP_OK;
}
'''

UI_C.write_text(ui)

# ---------------------------------------------------------------------------
# dt_runtime.h
# ---------------------------------------------------------------------------

RUNTIME_H.write_text(r'''#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dt_runtime_start(void);

#ifdef __cplusplus
}
#endif
''')

# ---------------------------------------------------------------------------
# dt_runtime.c
# ---------------------------------------------------------------------------

RUNTIME_C.write_text(r'''#include "dt_runtime.h"

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

#include "nvs.h"


static const char *TAG = "dt_runtime";

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
    char device_name[96];
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
    load_nvs_string(
        "mk_host",
        s_moonraker_host,
        sizeof(s_moonraker_host)
    );

    /*
     * Optional. If no key exists, Moonraker's normal trusted-client
     * authorization is used.
     */
    load_nvs_string(
        "mk_api_key",
        s_api_key,
        sizeof(s_api_key)
    );

    if (s_moonraker_host[0] == '\0') {
        s_base_url[0] = '\0';
        return;
    }

    if (
        strncmp(
            s_moonraker_host,
            "http://",
            7
        ) == 0 ||
        strncmp(
            s_moonraker_host,
            "https://",
            8
        ) == 0
    ) {
        snprintf(
            s_base_url,
            sizeof(s_base_url),
            "%s",
            s_moonraker_host
        );
    } else {
        snprintf(
            s_base_url,
            sizeof(s_base_url),
            "http://%s",
            s_moonraker_host
        );
    }

    size_t n =
        strlen(s_base_url);

    while (
        n > 0 &&
        s_base_url[n - 1] == '/'
    ) {
        s_base_url[--n] = '\0';
    }
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


static void action_task(void *arg)
{
    (void)arg;

    dt_ui_action_t action;

    for (;;) {
        if (
            xQueueReceive(
                s_action_queue,
                &action,
                portMAX_DELAY
            ) != pdTRUE
        ) {
            continue;
        }

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
}


static void status_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "Moonraker status task "
        "started on CPU%d",
        xPortGetCoreID()
    );

    unsigned failure_count = 0;

    for (;;) {
        runtime_snapshot_t snap;

        const bool online =
            query_status(&snap);

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
                    "Moonraker status unavailable "
                    "(attempt %u)",
                    failure_count
                );
            }
        }

        if (
            !s_have_previous ||
            !snapshot_equal(
                &snap,
                &s_previous
            )
        ) {
            push_ui(&snap);

            s_previous = snap;
            s_have_previous = true;

            ESP_LOGI(
                TAG,
                "state conn=%d job=%d "
                "progress=%u "
                "nozzle=%.1f/%.1f "
                "bed=%.1f/%.1f "
                "xyz=%.1f,%.1f,%.1f",
                (int)snap.connection,
                (int)snap.job,
                (unsigned)
                    snap.progress_percent,
                snap.nozzle_c,
                snap.nozzle_target_c,
                snap.bed_c,
                snap.bed_target_c,
                snap.x,
                snap.y,
                snap.z
            );
        }

        vTaskDelay(
            pdMS_TO_TICKS(
                DT_STATUS_PERIOD_MS
            )
        );
    }
}


esp_err_t dt_runtime_start(void)
{
    load_moonraker_config();

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
    } else {
        (void)esp_wifi_set_ps(
            WIFI_PS_NONE
        );
    }

    /*
     * Keep dragon-core's WebSocket Moonraker client active.
     * DragonTouch Stage 2 uses the public HTTP API alongside it
     * for richer HMI telemetry and commands not yet exposed by
     * dc_moonraker's shared status structure.
     */
    err = dc_moonraker_start();

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "dc_moonraker_start: %s",
            esp_err_to_name(err)
        );
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

    BaseType_t action_created =
        xTaskCreatePinnedToCore(
            action_task,
            "dt_commands",
            6144,
            NULL,
            4,
            NULL,
            1
        );

    BaseType_t status_created =
        xTaskCreatePinnedToCore(
            status_task,
            "dt_status",
            7168,
            NULL,
            3,
            NULL,
            1
        );

    if (
        action_created != pdPASS ||
        status_created != pdPASS
    ) {
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
''')

# ---------------------------------------------------------------------------
# main/CMakeLists.txt
# ---------------------------------------------------------------------------

cmake = CMAKE.read_text()

m = re.search(
    r"REQUIRES(?P<body>[^)]*)",
    cmake,
    re.S
)

if not m:
    sys.exit(
        "ERROR: no REQUIRES block in main/CMakeLists.txt"
    )

body = m.group("body")
tokens = set(
    re.findall(
        r"[A-Za-z0-9_]+",
        body
    )
)

for dep in (
    "esp_http_client",
    "json",
):
    if dep not in tokens:
        body += f" {dep}"

cmake = (
    cmake[:m.start("body")] +
    body +
    cmake[m.end("body"):]
)

CMAKE.write_text(cmake)

print()
print("Stage 2 installed.")
print()
print("Added:")
print("  - full Moonraker HTTP object telemetry")
print("  - nozzle / bed / fan / XYZ / homing state")
print("  - filename / progress / elapsed / estimated remaining")
print("  - Pause / Resume / Cancel")
print("  - Home all")
print("  - XY 10 mm / Z 1 mm jogging")
print("  - nozzle 220 C command")
print("  - bed Off / 60 / 100 C")
print("  - extrude / retract 10 mm")
print("  - part fan Off / 50 / 100")
print("  - asynchronous command worker on CPU1")
print()
print("UNCHANGED:")
print("  - dt_board display path")
print("  - RGB buffering")
print("  - GT911 touch path")
print("  - nav icon hit-test fix")
print()
print("Next:")
print("  git diff --check")
print("  idf.py build")
