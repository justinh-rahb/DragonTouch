#include "dt_ui.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#ifndef DT_UI_HOST_PREVIEW
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define DT_COLOR_BACKGROUND 0x181818
#define DT_COLOR_SURFACE    0x222222
#define DT_COLOR_SURFACE_2  0x303030
#define DT_COLOR_BORDER     0x3A3A3A
#define DT_COLOR_TEXT       0xF5F5F5
#define DT_COLOR_MUTED      0x999999
#define DT_COLOR_ACCENT     0xEF4444
#define DT_COLOR_SUCCESS    0x74D58B
#define DT_COLOR_WARNING    0xF59A56

#define DT_PAGE_COUNT (DT_UI_PAGE_SETTINGS + 1)
#define DT_CONTROL_TAB_COUNT 4
#define DT_FILES_TAB_COUNT 3

typedef enum {
    DT_NAV_ICON_HOME = 0,
    DT_NAV_ICON_CONTROL,
    DT_NAV_ICON_FILES,
    DT_NAV_ICON_FILAMENT,
    DT_NAV_ICON_DEVICES,
    DT_NAV_ICON_SETTINGS,
} dt_nav_icon_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *pages[DT_PAGE_COUNT];
    lv_obj_t *nav_buttons[DT_PAGE_COUNT];
    lv_obj_t *nav_icons[DT_PAGE_COUNT];
    lv_obj_t *device_name;
    lv_obj_t *connection_dot;
    lv_obj_t *connection_text;
    lv_obj_t *job_state;
    lv_obj_t *filename;
    lv_obj_t *progress;
    lv_obj_t *progress_text;
    lv_obj_t *time_text;
    lv_obj_t *nozzle_text;
    lv_obj_t *bed_text;
    lv_obj_t *fan_text;
    lv_obj_t *printer_name;
    lv_obj_t *printer_hint;
    lv_obj_t *pause_button;
    lv_obj_t *pause_label;
    lv_obj_t *cancel_button;
    lv_obj_t *control_panels[DT_CONTROL_TAB_COUNT];
    lv_obj_t *control_tabs[DT_CONTROL_TAB_COUNT];
    lv_obj_t *file_panels[DT_FILES_TAB_COUNT];
    lv_obj_t *file_tabs[DT_FILES_TAB_COUNT];
    lv_obj_t *dialog_scrim;
    lv_obj_t *dialog_title;
    lv_obj_t *dialog_body;
    lv_obj_t *dialog_confirm;
    lv_obj_t *dialog_confirm_label;

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

    /* DT_STAGE4_FILES */
    dt_ui_files_model_t files_model;

    /* DT_STAGE5_FILAMENT */
    dt_ui_filament_model_t filament_model;

    /* DT_STAGE6_SYSTEM_PAGES */
    dt_ui_system_model_t system_model;

    lv_obj_t *devices_printer_text;
    lv_obj_t *devices_network_text;
    lv_obj_t *devices_filament_text;

    lv_obj_t *settings_build_text;
    lv_obj_t *settings_memory_text;
    lv_obj_t *settings_portal_text;
    lv_obj_t *settings_update_text;
    lv_obj_t *settings_reboot_button;
    lv_obj_t *settings_factory_reset_button;

    lv_obj_t *filament_mode_text;
    lv_obj_t *filament_capability_text;
    lv_obj_t *filament_nozzle_text;
    lv_obj_t *filament_load_button;
    lv_obj_t *filament_unload_button;
    lv_obj_t *filament_extrude_button;
    lv_obj_t *filament_retract_button;
    lv_obj_t *filament_heat_button;
    lv_obj_t *filament_generic_macro_row;
    lv_obj_t *afc_summary_text;
    lv_obj_t *afc_message_text;
    lv_obj_t *afc_previous_button;
    lv_obj_t *afc_next_button;
    lv_obj_t *afc_clear_message_button;
    lv_obj_t *afc_resume_button;
    lv_obj_t *afc_lane_rows[DT_UI_AFC_LANE_PAGE_SIZE];
    lv_obj_t *afc_lane_title[DT_UI_AFC_LANE_PAGE_SIZE];
    lv_obj_t *afc_lane_detail[DT_UI_AFC_LANE_PAGE_SIZE];
    lv_obj_t *afc_lane_load_button[DT_UI_AFC_LANE_PAGE_SIZE];
    lv_obj_t *afc_lane_eject_button[DT_UI_AFC_LANE_PAGE_SIZE];
    size_t afc_lane_offset;


    lv_obj_t *file_path_text;
    lv_obj_t *file_status_text;
    lv_obj_t *file_entry_buttons[DT_UI_FILE_ENTRY_MAX];
    lv_obj_t *file_entry_labels[DT_UI_FILE_ENTRY_MAX];
    lv_obj_t *file_up_button;
    lv_obj_t *file_refresh_button;
    lv_obj_t *file_previous_button;
    lv_obj_t *file_next_button;
    lv_obj_t *file_detail_text;
    lv_obj_t *file_start_button;
    char file_confirm_body[320];

    dt_ui_connection_t current_connection;

    dt_ui_job_state_t current_job_state;

    /*
     * DT_UI_LAZY_PAGE_BUILD
     *
     * Page containers always exist, but only Home is populated during boot.
     * Other pages are populated on first visit.
     */
    bool page_built[DT_PAGE_COUNT];

    bool ready;
} dt_ui_state_t;

static const char *TAG = "dt_ui";

/*
 * UI_BUILD_WATCHDOG_FIX
 *
 * During startup app_main owns the LVGL lock while the full
 * object tree is constructed. Stage 2 contains enough objects
 * that uninterrupted construction can starve IDLE0 for longer
 * than the task watchdog period.
 *
 * Yielding one scheduler tick between major page builds lets
 * IDLE0 service the watchdog. The LVGL task cannot alter this
 * tree because app_main still owns the LVGL mutex.
 */
static void ui_build_yield(void)
{
#ifndef DT_UI_HOST_PREVIEW
    vTaskDelay(1);
#endif
}

static dt_ui_state_t s_ui;

static dt_ui_action_handler_t s_action_handler;
static void *s_action_ctx;
static dt_ui_file_request_handler_t s_file_request_handler;
static void *s_file_request_ctx;
static dt_ui_filament_request_handler_t s_filament_request_handler;
static void *s_filament_request_ctx;
static bool s_pending_filament_request_valid;
static dt_ui_filament_request_t s_pending_filament_request;
static int s_pending_filament_lane;
static dt_ui_action_t s_pending_action;

static lv_color_t color(uint32_t hex)
{
    return lv_color_hex(hex);
}

static void draw_icon_line(lv_layer_t *layer, lv_color_t line_color,
                           int32_t x, int32_t y, int32_t x1, int32_t y1,
                           int32_t x2, int32_t y2)
{
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = line_color;
    line.width = 2;
    line.round_start = true;
    line.round_end = true;
    line.p1.x = x + x1;
    line.p1.y = y + y1;
    line.p2.x = x + x2;
    line.p2.y = y + y2;
    lv_draw_line(layer, &line);
}

static void draw_icon_circle(lv_layer_t *layer, lv_color_t line_color,
                             int32_t x, int32_t y, int32_t cx, int32_t cy,
                             uint16_t radius)
{
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = line_color;
    arc.width = 2;
    arc.center.x = x + cx;
    arc.center.y = y + cy;
    arc.radius = radius;
    arc.start_angle = 0;
    arc.end_angle = 360;
    lv_draw_arc(layer, &arc);
}

static void draw_icon_rect(lv_layer_t *layer, lv_color_t line_color,
                           int32_t x, int32_t y, int32_t x1, int32_t y1,
                           int32_t x2, int32_t y2)
{
    draw_icon_line(layer, line_color, x, y, x1, y1, x2, y1);
    draw_icon_line(layer, line_color, x, y, x2, y1, x2, y2);
    draw_icon_line(layer, line_color, x, y, x2, y2, x1, y2);
    draw_icon_line(layer, line_color, x, y, x1, y2, x1, y1);
}

static void nav_icon_draw(lv_event_t *event)
{
    lv_obj_t *icon = lv_event_get_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    dt_nav_icon_t type = (dt_nav_icon_t)(uintptr_t)lv_event_get_user_data(event);
    lv_area_t area;
    lv_obj_get_coords(icon, &area);
    const int32_t x = area.x1;
    const int32_t y = area.y1;
    lv_color_t line_color = lv_obj_get_style_text_color(icon, LV_PART_MAIN);

    switch (type) {
    case DT_NAV_ICON_HOME:
        draw_icon_line(layer, line_color, x, y, 2, 10, 11, 3);
        draw_icon_line(layer, line_color, x, y, 11, 3, 20, 10);
        draw_icon_line(layer, line_color, x, y, 4, 9, 4, 20);
        draw_icon_line(layer, line_color, x, y, 18, 9, 18, 20);
        draw_icon_line(layer, line_color, x, y, 4, 20, 18, 20);
        draw_icon_rect(layer, line_color, x, y, 9, 13, 13, 20);
        break;
    case DT_NAV_ICON_CONTROL:
        draw_icon_line(layer, line_color, x, y, 3, 11, 19, 11);
        draw_icon_line(layer, line_color, x, y, 3, 11, 6, 8);
        draw_icon_line(layer, line_color, x, y, 3, 11, 6, 14);
        draw_icon_line(layer, line_color, x, y, 19, 11, 16, 8);
        draw_icon_line(layer, line_color, x, y, 19, 11, 16, 14);
        draw_icon_line(layer, line_color, x, y, 11, 3, 11, 19);
        draw_icon_line(layer, line_color, x, y, 11, 3, 8, 6);
        draw_icon_line(layer, line_color, x, y, 11, 3, 14, 6);
        draw_icon_line(layer, line_color, x, y, 11, 19, 8, 16);
        draw_icon_line(layer, line_color, x, y, 11, 19, 14, 16);
        break;
    case DT_NAV_ICON_FILES:
        draw_icon_line(layer, line_color, x, y, 5, 2, 14, 2);
        draw_icon_line(layer, line_color, x, y, 14, 2, 19, 7);
        draw_icon_line(layer, line_color, x, y, 19, 7, 19, 20);
        draw_icon_line(layer, line_color, x, y, 19, 20, 5, 20);
        draw_icon_line(layer, line_color, x, y, 5, 20, 5, 2);
        draw_icon_line(layer, line_color, x, y, 14, 2, 14, 7);
        draw_icon_line(layer, line_color, x, y, 14, 7, 19, 7);
        draw_icon_line(layer, line_color, x, y, 8, 12, 16, 12);
        draw_icon_line(layer, line_color, x, y, 8, 16, 16, 16);
        break;
    case DT_NAV_ICON_FILAMENT:
        draw_icon_rect(layer, line_color, x, y, 3, 2, 6, 20);
        draw_icon_rect(layer, line_color, x, y, 16, 2, 19, 20);
        draw_icon_line(layer, line_color, x, y, 6, 7, 16, 7);
        draw_icon_line(layer, line_color, x, y, 6, 11, 16, 11);
        draw_icon_line(layer, line_color, x, y, 6, 15, 16, 15);
        break;
    case DT_NAV_ICON_DEVICES:
        draw_icon_rect(layer, line_color, x, y, 2, 3, 20, 15);
        draw_icon_line(layer, line_color, x, y, 11, 15, 11, 19);
        draw_icon_line(layer, line_color, x, y, 7, 19, 15, 19);
        draw_icon_circle(layer, line_color, x, y, 16, 7, 1);
        break;
    case DT_NAV_ICON_SETTINGS:
        draw_icon_line(layer, line_color, x, y, 2, 6, 4, 6);
        draw_icon_line(layer, line_color, x, y, 10, 6, 20, 6);
        draw_icon_circle(layer, line_color, x, y, 7, 6, 3);
        draw_icon_line(layer, line_color, x, y, 2, 16, 12, 16);
        draw_icon_line(layer, line_color, x, y, 18, 16, 20, 16);
        draw_icon_circle(layer, line_color, x, y, 15, 16, 3);
        break;
    }
}

static lv_obj_t *make_nav_icon(lv_obj_t *parent, dt_nav_icon_t type)
{
    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);

    /*
     * Decorative nav icons must never consume pointer input.
     *
     * lv_obj_create() creates a CLICKABLE base object by default.
     * Without removing that flag, touches landing directly on the
     * 22x22 icon target the icon instead of the parent nav button,
     * so the parent's LV_EVENT_CLICKED callback never runs.
     */
    lv_obj_remove_flag(
        icon,
        LV_OBJ_FLAG_CLICKABLE |
        LV_OBJ_FLAG_CLICK_FOCUSABLE |
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_size(icon, 22, 22);
    lv_obj_set_style_text_color(icon, color(DT_COLOR_MUTED), 0);
    lv_obj_add_event_cb(
        icon,
        nav_icon_draw,
        LV_EVENT_DRAW_MAIN,
        (void *)(uintptr_t)type
    );

    return icon;
}

static void style_surface(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, color(DT_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, color(DT_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, uint32_t color_hex)
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
}

static lv_obj_t *make_card(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);
    style_surface(card);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_t *heading = make_label(card, title, DT_COLOR_MUTED);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_12, 0);
    return card;
}

static lv_obj_t *make_action(lv_obj_t *parent, const char *text, bool primary)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_height(button, 42);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button,
                              color(primary ? DT_COLOR_ACCENT : DT_COLOR_SURFACE_2), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_opa(button, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_t *label = make_label(button, text, DT_COLOR_TEXT);
    lv_obj_center(label);
    return button;
}

static void style_destructive_action(lv_obj_t *button)
{
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(button, color(DT_COLOR_WARNING), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), color(DT_COLOR_WARNING), 0);
}

static void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static void ensure_page_built(dt_ui_page_t page);
static void recycle_secondary_pages(dt_ui_page_t keep);

static void show_page(dt_ui_page_t selected)
{
    if (
        selected < DT_UI_PAGE_HOME ||
        selected > DT_UI_PAGE_SETTINGS
    ) {
        return;
    }

    /*
     * DT_UI_RECYCLE_SECONDARY_PAGES
     *
     * Home is permanently resident. Keep at most one secondary page's
     * content tree alive. This bounds LVGL's resident object/style
     * allocation instead of allowing every visited page to accumulate.
     */
    recycle_secondary_pages(selected);

    ensure_page_built(selected);

    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        const bool active =
            i == selected;

        if (active) {
            lv_obj_remove_flag(
                s_ui.pages[i],
                LV_OBJ_FLAG_HIDDEN
            );
        } else {
            lv_obj_add_flag(
                s_ui.pages[i],
                LV_OBJ_FLAG_HIDDEN
            );
        }

        lv_obj_set_style_text_color(
            s_ui.nav_icons[i],
            color(
                active
                    ? DT_COLOR_ACCENT
                    : DT_COLOR_MUTED
            ),
            0
        );

        lv_obj_set_style_bg_opa(
            s_ui.nav_buttons[i],
            active
                ? LV_OPA_20
                : LV_OPA_TRANSP,
            0
        );

        lv_obj_set_style_border_width(
            s_ui.nav_buttons[i],
            active ? 3 : 0,
            0
        );

        lv_obj_set_style_border_side(
            s_ui.nav_buttons[i],
            LV_BORDER_SIDE_LEFT,
            0
        );

        lv_obj_set_style_border_color(
            s_ui.nav_buttons[i],
            color(DT_COLOR_ACCENT),
            0
        );
    }
}

static void nav_event(lv_event_t *event)
{
    dt_ui_page_t page =
        (dt_ui_page_t)(uintptr_t)
        lv_event_get_user_data(event);

    show_page(page);
}

typedef struct {
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


static const dt_confirmation_t CONFIRM_FILAMENT_LOAD = {
    "Run filament load?",
    "DragonTouch will invoke the printer-provided LOAD_FILAMENT macro. The macro remains authoritative over heating and motion.",
    "Load filament",
    false,
    DT_UI_ACTION_FILAMENT_LOAD,
};

static const dt_confirmation_t CONFIRM_FILAMENT_UNLOAD = {
    "Run filament unload?",
    "DragonTouch will invoke the printer-provided UNLOAD_FILAMENT macro. The macro remains authoritative over heating and motion.",
    "Unload filament",
    false,
    DT_UI_ACTION_FILAMENT_UNLOAD,
};


static const dt_confirmation_t CONFIRM_SYSTEM_REBOOT = {
    "Reboot DragonTouch?",
    "The display controller will restart. The printer and its current print are not restarted.",
    "Reboot",
    false,
    DT_UI_ACTION_SYSTEM_REBOOT,
};


static const dt_confirmation_t CONFIRM_SYSTEM_FACTORY_RESET = {
    "Factory reset DragonTouch?",
    "This clears DragonTouch Wi-Fi credentials and Moonraker configuration, "
    "then reboots into the setup access point. It does NOT reset Klipper or "
    "erase printer configuration.",
    "Factory reset",
    true,
    DT_UI_ACTION_SYSTEM_FACTORY_RESET,
};


static void dispatch_action(dt_ui_action_t action)
{
    if (s_action_handler != NULL) {
        s_action_handler(action, s_action_ctx);
    }
}



static void dispatch_filament_request(
    dt_ui_filament_request_t request,
    int lane_number
)
{
    if (s_filament_request_handler != NULL) {
        s_filament_request_handler(
            request,
            lane_number,
            s_filament_request_ctx
        );
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

    if (s_pending_filament_request_valid) {
        dispatch_filament_request(
            s_pending_filament_request,
            s_pending_filament_lane
        );

        s_pending_filament_request_valid = false;
    } else {
        dispatch_action(s_pending_action);
    }

    lv_obj_add_flag(
        s_ui.dialog_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void show_confirmation(
    const dt_confirmation_t *confirmation
)
{
    s_pending_filament_request_valid = false;

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


static void size_card_action(lv_obj_t *button)
{
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_flex_grow(button, 0);
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(page, 12, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_style_pad_column(page, 10, 0);
    return page;
}

static void create_metric(lv_obj_t *parent, const char *name, lv_obj_t **value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 26);
    make_label(row, name, DT_COLOR_MUTED);
    *value = make_label(row, "--", DT_COLOR_TEXT);
    lv_obj_align(*value, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void create_home_page(lv_obj_t *page)
{
    lv_obj_set_layout(page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW);

    lv_obj_t *main_column = lv_obj_create(page);
    lv_obj_remove_style_all(main_column);
    lv_obj_set_height(main_column, LV_PCT(100));
    lv_obj_set_flex_grow(main_column, 3);
    lv_obj_set_layout(main_column, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(main_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(main_column, 10, 0);

    lv_obj_t *job = make_card(main_column, "CURRENT JOB");
    lv_obj_set_width(job, LV_PCT(100));
    lv_obj_set_flex_grow(job, 1);
    s_ui.job_state = make_label(job, "Printer unavailable", DT_COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.job_state, &lv_font_montserrat_20, 0);
    s_ui.filename = make_label(job, "No active file", DT_COLOR_MUTED);
    lv_label_set_long_mode(s_ui.filename, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_ui.filename, LV_PCT(100));
    s_ui.progress = lv_bar_create(job);
    lv_obj_set_size(s_ui.progress, LV_PCT(100), 10);
    lv_bar_set_range(s_ui.progress, 0, 100);
    lv_obj_set_style_bg_color(s_ui.progress, color(DT_COLOR_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.progress, color(DT_COLOR_ACCENT), LV_PART_INDICATOR);
    s_ui.progress_text = make_label(job, "0%", DT_COLOR_TEXT);
    s_ui.time_text = make_label(job, "Elapsed --  •  Remaining --", DT_COLOR_MUTED);

    lv_obj_t *actions = lv_obj_create(job);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), 42);
    lv_obj_set_layout(actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 8, 0);
    s_ui.pause_button = make_action(actions, "Pause", true);
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
    );
    set_button_enabled(s_ui.pause_button, false);
    set_button_enabled(s_ui.cancel_button, false);

    lv_obj_t *quick = make_card(main_column, "QUICK ACCESS");
    lv_obj_set_size(quick, LV_PCT(100), 92);
    lv_obj_t *quick_row = lv_obj_create(quick);
    lv_obj_remove_style_all(quick_row);
    lv_obj_set_size(quick_row, LV_PCT(100), 42);
    lv_obj_set_layout(quick_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(quick_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(quick_row, 8, 0);
    const char *quick_names[] = {"Home axes", "Load", "Unload", "Lights"};
    for (size_t i = 0; i < 4; ++i) {
        lv_obj_t *button = make_action(quick_row, quick_names[i], false);
        set_button_enabled(button, false);
    }

    lv_obj_t *side = lv_obj_create(page);
    lv_obj_remove_style_all(side);
    lv_obj_set_height(side, LV_PCT(100));
    lv_obj_set_flex_grow(side, 2);
    lv_obj_set_layout(side, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 10, 0);

    lv_obj_t *temperatures = make_card(side, "TEMPERATURES");
    lv_obj_set_width(temperatures, LV_PCT(100));
    lv_obj_set_flex_grow(temperatures, 1);
    create_metric(temperatures, "Nozzle", &s_ui.nozzle_text);
    create_metric(temperatures, "Bed", &s_ui.bed_text);
    create_metric(temperatures, "Part fan", &s_ui.fan_text);

    lv_obj_t *printer = make_card(side, "PRINTER");
    lv_obj_set_size(printer, LV_PCT(100), 118);
    s_ui.printer_name = make_label(printer, "No paired printer", DT_COLOR_TEXT);
    s_ui.printer_hint = make_label(printer, "Pair a same-LAN device to begin.", DT_COLOR_MUTED);
}

static void create_stub_page(
    lv_obj_t *page,
    const char *title,
    const char *description,
    const char *const *cards,
    size_t card_count
)
{
    /*
     * DT_UI_STUB_GRID_FIX
     *
     * Avoid wrapping flex rows with fixed-width children that also
     * flex-grow. Settings is the first stub with four cards and is
     * therefore the first page that crosses onto a second flex line.
     *
     * A two-column grid has deterministic geometry and avoids that
     * flex-wrap path entirely.
     */
    static int32_t col_dsc[] = {
        LV_GRID_FR(1),
        LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST
    };

    static int32_t row_dsc[] = {
        116,
        116,
        116,
        116,
        LV_GRID_TEMPLATE_LAST
    };

    lv_obj_set_layout(
        page,
        LV_LAYOUT_FLEX
    );

    lv_obj_set_flex_flow(
        page,
        LV_FLEX_FLOW_COLUMN
    );

    lv_obj_t *heading =
        make_label(
            page,
            title,
            DT_COLOR_TEXT
        );

    lv_obj_set_style_text_font(
        heading,
        &lv_font_montserrat_20,
        0
    );

    lv_obj_t *body =
        make_label(
            page,
            description,
            DT_COLOR_MUTED
        );

    lv_label_set_long_mode(
        body,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        body,
        LV_PCT(100)
    );

    lv_obj_t *grid =
        lv_obj_create(page);

    lv_obj_remove_style_all(grid);

    lv_obj_set_width(
        grid,
        LV_PCT(100)
    );

    lv_obj_set_flex_grow(
        grid,
        1
    );

    lv_obj_set_layout(
        grid,
        LV_LAYOUT_GRID
    );

    lv_obj_set_grid_dsc_array(
        grid,
        col_dsc,
        row_dsc
    );

    lv_obj_set_style_pad_row(
        grid,
        10,
        0
    );

    lv_obj_set_style_pad_column(
        grid,
        10,
        0
    );

    for (
        size_t i = 0;
        i < card_count;
        ++i
    ) {
        const int32_t col =
            (int32_t)(i % 2);

        const int32_t row =
            (int32_t)(i / 2);

        if (row >= 4) {
            ESP_LOGW(
                TAG,
                "stub page '%s' has too many cards: %u",
                title,
                (unsigned)card_count
            );
            break;
        }

        lv_obj_t *card =
            make_card(
                grid,
                cards[i]
            );

        lv_obj_set_grid_cell(
            card,
            LV_GRID_ALIGN_STRETCH,
            col,
            1,
            LV_GRID_ALIGN_STRETCH,
            row,
            1
        );

        lv_obj_t *availability =
            make_label(
                card,
                "Available after printer pairing",
                DT_COLOR_MUTED
            );

        lv_label_set_long_mode(
            availability,
            LV_LABEL_LONG_MODE_WRAP
        );

        lv_obj_set_width(
            availability,
            LV_PCT(100)
        );

        lv_obj_t *button =
            make_action(
                card,
                "Unavailable",
                false
            );

        lv_obj_set_width(
            button,
            LV_PCT(100)
        );

        lv_obj_set_flex_grow(
            button,
            0
        );

        set_button_enabled(
            button,
            false
        );
    }
}

static void select_tab(lv_obj_t *const *tabs, lv_obj_t *const *panels, size_t count,
                       size_t selected)
{
    for (size_t i = 0; i < count; ++i) {
        lv_obj_set_style_bg_color(tabs[i],
                                  color(i == selected ? DT_COLOR_ACCENT : DT_COLOR_SURFACE_2), 0);
        if (i == selected) {
            lv_obj_remove_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void control_tab_event(lv_event_t *event)
{
    select_tab(s_ui.control_tabs, s_ui.control_panels, DT_CONTROL_TAB_COUNT,
               (size_t)(uintptr_t)lv_event_get_user_data(event));
}


static void render_files_model(void);

static void dispatch_file_request(
    dt_ui_file_request_t request,
    const char *path
)
{
    if (s_file_request_handler != NULL) {
        s_file_request_handler(request, path, s_file_request_ctx);
    }
}

static void file_refresh_event(lv_event_t *event)
{
    (void)event;
    dispatch_file_request(DT_UI_FILE_REQUEST_REFRESH, NULL);
}

static void file_up_event(lv_event_t *event)
{
    (void)event;
    dispatch_file_request(DT_UI_FILE_REQUEST_UP, NULL);
}

static void file_previous_event(lv_event_t *event)
{
    (void)event;
    dispatch_file_request(DT_UI_FILE_REQUEST_PREVIOUS, NULL);
}

static void file_next_event(lv_event_t *event)
{
    (void)event;
    dispatch_file_request(DT_UI_FILE_REQUEST_NEXT, NULL);
}

static void file_entry_event(lv_event_t *event)
{
    const size_t index =
        (size_t)(uintptr_t)lv_event_get_user_data(event);

    if (
        index >= s_ui.files_model.entry_count ||
        index >= DT_UI_FILE_ENTRY_MAX
    ) {
        return;
    }

    const dt_ui_file_entry_t *entry =
        &s_ui.files_model.entries[index];

    if (entry->is_directory) {
        dispatch_file_request(
            DT_UI_FILE_REQUEST_OPEN_DIRECTORY,
            entry->path
        );
        return;
    }

    s_ui.files_model.selected = true;

    snprintf(
        s_ui.files_model.selected_name,
        sizeof(s_ui.files_model.selected_name),
        "%s",
        entry->name
    );

    snprintf(
        s_ui.files_model.selected_path,
        sizeof(s_ui.files_model.selected_path),
        "%s",
        entry->path
    );

    s_ui.files_model.selected_size_bytes = entry->size_bytes;
    s_ui.files_model.estimated_seconds = 0;
    s_ui.files_model.filament_weight_g = 0.0f;
    s_ui.files_model.filament_length_mm = 0.0f;
    s_ui.files_model.layer_height_mm = 0.0f;
    s_ui.files_model.slicer[0] = '\0';
    s_ui.files_model.filament_type[0] = '\0';

    snprintf(
        s_ui.files_model.detail_error,
        sizeof(s_ui.files_model.detail_error),
        "Loading metadata..."
    );

    render_files_model();

    select_tab(
        s_ui.file_tabs,
        s_ui.file_panels,
        DT_FILES_TAB_COUNT,
        1
    );

    dispatch_file_request(
        DT_UI_FILE_REQUEST_SELECT_FILE,
        entry->path
    );
}

static void file_start_event(lv_event_t *event)
{
    (void)event;

    if (!s_ui.files_model.selected) {
        return;
    }

    snprintf(
        s_ui.file_confirm_body,
        sizeof(s_ui.file_confirm_body),
        "Moonraker will start:\n%s\n\n"
        "The printer must be idle and ready.",
        s_ui.files_model.selected_name
    );

    const dt_confirmation_t confirmation = {
        "Start this print?",
        s_ui.file_confirm_body,
        "Start print",
        false,
        DT_UI_ACTION_FILE_START_SELECTED,
    };

    show_confirmation(&confirmation);
}

static void format_file_size(
    char *buffer,
    size_t length,
    uint32_t bytes
)
{
    if (bytes >= 1024U * 1024U) {
        snprintf(
            buffer,
            length,
            "%.1f MB",
            (double)bytes / (1024.0 * 1024.0)
        );
    } else if (bytes >= 1024U) {
        snprintf(
            buffer,
            length,
            "%.0f KB",
            (double)bytes / 1024.0
        );
    } else {
        snprintf(
            buffer,
            length,
            "%u B",
            (unsigned)bytes
        );
    }
}

static void render_files_model(void)
{
    if (s_ui.file_path_text == NULL) {
        return;
    }

    const dt_ui_files_model_t *model =
        &s_ui.files_model;

    const char *display_path = model->directory;

    if (strncmp(display_path, "gcodes/", 7) == 0) {
        display_path += 6;
    } else if (strcmp(display_path, "gcodes") == 0) {
        display_path = "/";
    }

    lv_label_set_text_fmt(
        s_ui.file_path_text,
        "%s",
        display_path[0] != '\0' ? display_path : "/"
    );

    if (model->loading) {
        lv_label_set_text(s_ui.file_status_text, "Loading...");
    } else if (model->error[0] != '\0') {
        lv_label_set_text(s_ui.file_status_text, model->error);
    } else if (!model->online) {
        lv_label_set_text(s_ui.file_status_text, "Printer offline");
    } else {
        lv_label_set_text_fmt(
            s_ui.file_status_text,
            "%u item%s  |  showing %u-%u",
            (unsigned)model->total_entries,
            model->total_entries == 1 ? "" : "s",
            model->total_entries == 0
                ? 0U
                : (unsigned)model->offset + 1U,
            (unsigned)(model->offset + model->entry_count)
        );
    }

    for (size_t i = 0; i < DT_UI_FILE_ENTRY_MAX; ++i) {
        lv_obj_t *button = s_ui.file_entry_buttons[i];

        if (button == NULL) {
            continue;
        }

        if (i >= model->entry_count) {
            lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_remove_flag(button, LV_OBJ_FLAG_HIDDEN);

        const dt_ui_file_entry_t *entry = &model->entries[i];
        char size_text[32] = {0};

        format_file_size(
            size_text,
            sizeof(size_text),
            entry->size_bytes
        );

        if (entry->is_directory) {
            lv_label_set_text_fmt(
                s_ui.file_entry_labels[i],
                "[DIR] %s",
                entry->name
            );
        } else {
            lv_label_set_text_fmt(
                s_ui.file_entry_labels[i],
                "%s   %s",
                entry->name,
                size_text
            );
        }

        set_button_enabled(
            button,
            model->online && !model->loading
        );
    }

    set_button_enabled(
        s_ui.file_refresh_button,
        !model->loading
    );

    set_button_enabled(
        s_ui.file_up_button,
        !model->loading &&
            strcmp(model->directory, "gcodes") != 0
    );

    set_button_enabled(
        s_ui.file_previous_button,
        !model->loading && model->has_previous
    );

    set_button_enabled(
        s_ui.file_next_button,
        !model->loading && model->has_next
    );

    if (s_ui.file_detail_text != NULL) {
        if (!model->selected) {
            lv_label_set_text(
                s_ui.file_detail_text,
                "Select a G-code file to inspect its Moonraker metadata."
            );
        } else if (model->detail_error[0] != '\0') {
            lv_label_set_text_fmt(
                s_ui.file_detail_text,
                "%s\n\n%s",
                model->selected_name,
                model->detail_error
            );
        } else {
            char size_text[32] = {0};
            char time_text[48] = "--";
            char filament_text[64] = "--";
            char layer_text[48] = "--";

            format_file_size(
                size_text,
                sizeof(size_text),
                model->selected_size_bytes
            );

            if (model->estimated_seconds > 0) {
                snprintf(
                    time_text,
                    sizeof(time_text),
                    "%uh %02um",
                    (unsigned)(model->estimated_seconds / 3600U),
                    (unsigned)(
                        (model->estimated_seconds % 3600U) / 60U
                    )
                );
            }

            if (model->filament_weight_g > 0.0f) {
                snprintf(
                    filament_text,
                    sizeof(filament_text),
                    "%.1f g%s%s",
                    (double)model->filament_weight_g,
                    model->filament_type[0] != '\0' ? "  |  " : "",
                    model->filament_type
                );
            } else if (model->filament_length_mm > 0.0f) {
                snprintf(
                    filament_text,
                    sizeof(filament_text),
                    "%.1f m%s%s",
                    (double)(model->filament_length_mm / 1000.0f),
                    model->filament_type[0] != '\0' ? "  |  " : "",
                    model->filament_type
                );
            }

            if (model->layer_height_mm > 0.0f) {
                snprintf(
                    layer_text,
                    sizeof(layer_text),
                    "%.2f mm",
                    (double)model->layer_height_mm
                );
            }

            lv_label_set_text_fmt(
                s_ui.file_detail_text,
                "%s\n\n"
                "Size: %s\n"
                "Estimate: %s\n"
                "Filament: %s\n"
                "Layer: %s\n"
                "Slicer: %s",
                model->selected_name,
                size_text,
                time_text,
                filament_text,
                layer_text,
                model->slicer[0] != '\0' ? model->slicer : "--"
            );
        }
    }

    const bool print_active =
        s_ui.current_job_state == DT_UI_JOB_PRINTING ||
        s_ui.current_job_state == DT_UI_JOB_PAUSED;

    set_button_enabled(
        s_ui.file_start_button,
        model->online &&
            model->selected &&
            !model->loading &&
            !print_active
    );
}


static void file_tab_event(lv_event_t *event)
{
    select_tab(s_ui.file_tabs, s_ui.file_panels, DT_FILES_TAB_COUNT,
               (size_t)(uintptr_t)lv_event_get_user_data(event));
}

static lv_obj_t *create_page_heading(lv_obj_t *page, const char *title, const char *description)
{
    lv_obj_set_layout(page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 7, 0);
    lv_obj_t *heading = make_label(page, title, DT_COLOR_TEXT);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
    make_label(page, description, DT_COLOR_MUTED);
    lv_obj_t *tabs = lv_obj_create(page);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, LV_PCT(100), 38);
    lv_obj_set_layout(tabs, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 6, 0);
    return tabs;
}

static lv_obj_t *create_tab(lv_obj_t *row, const char *name, lv_event_cb_t callback, size_t index)
{
    lv_obj_t *tab = make_action(row, name, index == 0);
    lv_obj_set_height(tab, 36);
    lv_obj_add_event_cb(tab, callback, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    return tab;
}

static lv_obj_t *create_tab_panel(lv_obj_t *page)
{
    lv_obj_t *panel = lv_obj_create(page);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(panel, 10, 0);
    return panel;
}

static lv_obj_t *make_control_card(lv_obj_t *parent, const char *title, const char *body)
{
    lv_obj_t *card = make_card(parent, title);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_t *description = make_label(card, body, DT_COLOR_MUTED);
    lv_label_set_long_mode(description, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(description, LV_PCT(100));
    return card;
}

static void create_control_page(lv_obj_t *page)
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
}


/*
 * DT_STAGE5_AFC_LANES
 */
static void render_filament_model(void)
{
    if (s_ui.filament_mode_text == NULL) {
        return;
    }

    const dt_ui_filament_model_t *model =
        &s_ui.filament_model;

    if (!model->online) {
        lv_label_set_text(
            s_ui.filament_mode_text,
            "Printer offline"
        );
    } else if (!model->capabilities_known) {
        lv_label_set_text(
            s_ui.filament_mode_text,
            "Detecting printer capabilities..."
        );
    } else {
        lv_label_set_text_fmt(
            s_ui.filament_mode_text,
            "Mode: %s",
            model->mode[0] != '\0'
                ? model->mode
                : "Standard extruder"
        );
    }

    char nozzle_text[96] = {0};

    if (
        model->online &&
        isfinite(model->nozzle_c) &&
        isfinite(model->nozzle_target_c)
    ) {
        snprintf(
            nozzle_text,
            sizeof(nozzle_text),
            "%.1f / %.0f °C%s",
            (double)model->nozzle_c,
            (double)model->nozzle_target_c,
            model->can_extrude
                ? "  |  extrusion ready"
                : ""
        );
    } else {
        snprintf(
            nozzle_text,
            sizeof(nozzle_text),
            "Temperature unavailable"
        );
    }

    lv_label_set_text(
        s_ui.filament_nozzle_text,
        nozzle_text
    );

    const bool afc_mode =
        model->afc_detected &&
        model->afc_lane_count > 0;

    if (s_ui.filament_generic_macro_row != NULL) {
        if (afc_mode) {
            lv_obj_add_flag(
                s_ui.filament_generic_macro_row,
                LV_OBJ_FLAG_HIDDEN
            );
        } else {
            lv_obj_remove_flag(
                s_ui.filament_generic_macro_row,
                LV_OBJ_FLAG_HIDDEN
            );
        }
    }

    if (s_ui.afc_summary_text != NULL) {
        if (!afc_mode) {
            lv_label_set_text(
                s_ui.afc_summary_text,
                "No AFC lanes detected."
            );
        } else {
            lv_label_set_text_fmt(
                s_ui.afc_summary_text,
                "AFC: %s  |  Loaded: %s  |  %u lane%s",
                model->afc_state[0] != '\0'
                    ? model->afc_state
                    : "--",
                model->afc_current_load[0] != '\0'
                    ? model->afc_current_load
                    : "none",
                (unsigned)model->afc_lane_count,
                model->afc_lane_count == 1 ? "" : "s"
            );
        }
    }

    if (s_ui.afc_message_text != NULL) {
        if (model->afc_message[0] != '\0') {
            lv_label_set_text(
                s_ui.afc_message_text,
                model->afc_message
            );
        } else {
            lv_label_set_text(
                s_ui.afc_message_text,
                model->afc_error
                    ? "AFC reports an error."
                    : "AFC ready."
            );
        }
    }

    if (
        s_ui.afc_lane_offset >= model->afc_lane_count &&
        model->afc_lane_count > 0
    ) {
        s_ui.afc_lane_offset =
            ((model->afc_lane_count - 1U) /
                DT_UI_AFC_LANE_PAGE_SIZE) *
            DT_UI_AFC_LANE_PAGE_SIZE;
    }

    for (
        size_t slot = 0;
        slot < DT_UI_AFC_LANE_PAGE_SIZE;
        ++slot
    ) {
        lv_obj_t *row =
            s_ui.afc_lane_rows[slot];

        if (row == NULL) {
            continue;
        }

        const size_t index =
            s_ui.afc_lane_offset + slot;

        if (
            !afc_mode ||
            index >= model->afc_lane_count
        ) {
            lv_obj_add_flag(
                row,
                LV_OBJ_FLAG_HIDDEN
            );
            continue;
        }

        lv_obj_remove_flag(
            row,
            LV_OBJ_FLAG_HIDDEN
        );

        const dt_ui_afc_lane_t *lane =
            &model->afc_lanes[index];

        lv_label_set_text_fmt(
            s_ui.afc_lane_title[slot],
            "%s  %s",
            lane->name,
            lane->map[0] != '\0'
                ? lane->map
                : ""
        );

        char detail[192] = {0};

        snprintf(
            detail,
            sizeof(detail),
            "%s%s%s  |  %s  |  %.1f g\n"
            "%s  |  prep:%s load:%s hub:%s tool:%s",
            lane->material[0] != '\0'
                ? lane->material
                : "Unknown",
            lane->color[0] != '\0'
                ? " "
                : "",
            lane->color[0] != '\0'
                ? lane->color
                : "",
            lane->filament_status[0] != '\0'
                ? lane->filament_status
                : lane->status,
            (double)lane->weight_g,
            lane->status[0] != '\0'
                ? lane->status
                : "--",
            lane->prep ? "Y" : "N",
            lane->load ? "Y" : "N",
            lane->loaded_to_hub ? "Y" : "N",
            lane->tool_loaded ? "Y" : "N"
        );

        lv_label_set_text(
            s_ui.afc_lane_detail[slot],
            detail
        );

        const bool lane_has_filament_state =
            lane->prep ||
            lane->load ||
            lane->loaded_to_hub ||
            lane->tool_loaded;

        const bool recovery_mode =
            model->afc_error;

        lv_obj_t *primary_label =
            lv_obj_get_child(
                s_ui.afc_lane_load_button[slot],
                0
            );

        if (primary_label != NULL) {
            lv_label_set_text(
                primary_label,
                recovery_mode
                    ? "Reset"
                    : "Load"
            );
        }

        const bool can_change =
            !recovery_mode &&
            model->afc_actions_enabled &&
            model->has_bt_change_tool &&
            lane->prep;

        /*
         * Recovery reset is allowed while paused, but never while the
         * printer is actively printing. AFC itself remains authoritative
         * over whether the selected lane can actually be reset.
         */
        const bool can_reset =
            recovery_mode &&
            model->online &&
            !model->printer_printing &&
            model->has_afc_lane_reset &&
            lane_has_filament_state;

        const bool can_eject =
            !recovery_mode &&
            model->afc_actions_enabled &&
            model->has_bt_lane_eject &&
            lane_has_filament_state;

        set_button_enabled(
            s_ui.afc_lane_load_button[slot],
            recovery_mode
                ? can_reset
                : can_change
        );

        set_button_enabled(
            s_ui.afc_lane_eject_button[slot],
            can_eject
        );
    }

    set_button_enabled(
        s_ui.afc_previous_button,
        afc_mode &&
            s_ui.afc_lane_offset > 0
    );

    set_button_enabled(
        s_ui.afc_next_button,
        afc_mode &&
            s_ui.afc_lane_offset +
                DT_UI_AFC_LANE_PAGE_SIZE <
            model->afc_lane_count
    );

    set_button_enabled(
        s_ui.afc_clear_message_button,
        afc_mode &&
            model->has_afc_clear_message &&
            model->afc_message[0] != '\0'
    );

    set_button_enabled(
        s_ui.afc_resume_button,
        afc_mode &&
            model->has_bt_resume &&
            model->printer_paused &&
            !model->afc_error
    );

    set_button_enabled(
        s_ui.filament_load_button,
        !afc_mode &&
            model->online &&
            model->capabilities_known &&
            model->has_load_macro
    );

    set_button_enabled(
        s_ui.filament_unload_button,
        !afc_mode &&
            model->online &&
            model->capabilities_known &&
            model->has_unload_macro
    );

    set_button_enabled(
        s_ui.filament_extrude_button,
        model->online &&
            model->can_extrude
    );

    set_button_enabled(
        s_ui.filament_retract_button,
        model->online &&
            model->can_extrude
    );

    set_button_enabled(
        s_ui.filament_heat_button,
        model->online
    );
}


/*
 * DT_STAGE5_COMPACT_FILAMENT_PAGE
 *
 * Keep the Filament page deliberately shallow. The previous Stage 5
 * implementation nested three full-height flex cards inside a row panel.
 * That layout is unnecessary on the 800x480 target and creates a much
 * larger LVGL object/layout tree during lazy page construction.
 *
 * This page uses one resident card with two action rows.
 */

static void show_afc_confirmation(
    dt_ui_filament_request_t request,
    int lane_number,
    const char *title,
    const char *body,
    const char *confirm_label
)
{
    s_pending_filament_request_valid = true;
    s_pending_filament_request = request;
    s_pending_filament_lane = lane_number;

    lv_label_set_text(
        s_ui.dialog_title,
        title
    );

    lv_label_set_text(
        s_ui.dialog_body,
        body
    );

    lv_label_set_text(
        s_ui.dialog_confirm_label,
        confirm_label
    );

    set_button_enabled(
        s_ui.dialog_confirm,
        s_filament_request_handler != NULL
    );

    lv_obj_remove_flag(
        s_ui.dialog_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(
        s_ui.dialog_scrim
    );
}


/*
 * DT_STAGE5_AFC_RECOVERY
 */
static void afc_clear_message_event(lv_event_t *event)
{
    (void)event;

    dispatch_filament_request(
        DT_UI_FILAMENT_REQUEST_CLEAR_MESSAGE,
        0
    );
}


static void afc_resume_event(lv_event_t *event)
{
    (void)event;

    show_afc_confirmation(
        DT_UI_FILAMENT_REQUEST_RESUME,
        0,
        "Resume after AFC recovery?",
        "Run BT_RESUME? AFC will restore its saved recovery "
        "position and resume the paused print.",
        "Resume print"
    );
}


static int afc_lane_for_slot(size_t slot)
{
    const size_t index =
        s_ui.afc_lane_offset + slot;

    if (
        index >= s_ui.filament_model.afc_lane_count
    ) {
        return 0;
    }

    return
        s_ui.filament_model
            .afc_lanes[index]
            .lane_number;
}


static void afc_lane_load_event(lv_event_t *event)
{
    const size_t slot =
        (size_t)(uintptr_t)
        lv_event_get_user_data(event);

    const size_t index =
        s_ui.afc_lane_offset + slot;

    if (
        index >= s_ui.filament_model.afc_lane_count
    ) {
        return;
    }

    const dt_ui_afc_lane_t *lane =
        &s_ui.filament_model.afc_lanes[index];

    const int lane_number =
        lane->lane_number;

    if (lane_number <= 0) {
        return;
    }

    char body[224] = {0};

    if (s_ui.filament_model.afc_error) {
        snprintf(
            body,
            sizeof(body),
            "Run AFC_LANE_RESET LANE=%s?\n"
            "AFC will reset this lane back toward the hub "
            "using its configured/default reset distance.",
            lane->name
        );

        show_afc_confirmation(
            DT_UI_FILAMENT_REQUEST_RESET_LANE,
            lane_number,
            "Reset AFC lane?",
            body,
            "Reset lane"
        );

        return;
    }

    snprintf(
        body,
        sizeof(body),
        "Run BT_CHANGE_TOOL LANE=%d?\n"
        "AFC will unload the current lane if necessary "
        "and load the selected lane.",
        lane_number
    );

    show_afc_confirmation(
        DT_UI_FILAMENT_REQUEST_CHANGE_TOOL,
        lane_number,
        "Load AFC lane?",
        body,
        "Change tool"
    );
}


static void afc_lane_eject_event(lv_event_t *event)
{
    const size_t slot =
        (size_t)(uintptr_t)
        lv_event_get_user_data(event);

    const int lane =
        afc_lane_for_slot(slot);

    if (lane <= 0) {
        return;
    }

    char body[192] = {0};

    snprintf(
        body,
        sizeof(body),
        "Run BT_LANE_EJECT LANE=%d?\n"
        "AFC will fully eject this lane so the spool "
        "can be removed.",
        lane
    );

    show_afc_confirmation(
        DT_UI_FILAMENT_REQUEST_EJECT_LANE,
        lane,
        "Eject AFC lane?",
        body,
        "Eject lane"
    );
}


static void afc_previous_event(lv_event_t *event)
{
    (void)event;

    if (
        s_ui.afc_lane_offset >=
            DT_UI_AFC_LANE_PAGE_SIZE
    ) {
        s_ui.afc_lane_offset -=
            DT_UI_AFC_LANE_PAGE_SIZE;
    } else {
        s_ui.afc_lane_offset = 0;
    }

    render_filament_model();
}


static void afc_next_event(lv_event_t *event)
{
    (void)event;

    if (
        s_ui.afc_lane_offset +
            DT_UI_AFC_LANE_PAGE_SIZE <
        s_ui.filament_model.afc_lane_count
    ) {
        s_ui.afc_lane_offset +=
            DT_UI_AFC_LANE_PAGE_SIZE;
    }

    render_filament_model();
}



static const char *system_connection_text(
    dt_ui_connection_t connection
)
{
    switch (connection) {
    case DT_UI_CONNECTION_ONLINE:
        return "Online";

    case DT_UI_CONNECTION_CONNECTING:
        return "Connecting";

    default:
        return "Offline";
    }
}


static void render_system_model(void)
{
    const dt_ui_system_model_t *model =
        &s_ui.system_model;

    if (s_ui.devices_printer_text != NULL) {
        lv_label_set_text_fmt(
            s_ui.devices_printer_text,
            "%s\n%s",
            system_connection_text(
                model->printer_connection
            ),
            model->moonraker_url[0] != '\0'
                ? model->moonraker_url
                : "Moonraker not configured"
        );
    }

    if (s_ui.devices_network_text != NULL) {
        lv_label_set_text_fmt(
            s_ui.devices_network_text,
            "%s  |  RSSI %d dBm\nIP %s",
            model->wifi_ssid[0] != '\0'
                ? model->wifi_ssid
                : "Wi-Fi unavailable",
            model->wifi_rssi,
            model->local_ip[0] != '\0'
                ? model->local_ip
                : "--"
        );
    }

    if (s_ui.devices_filament_text != NULL) {
        lv_label_set_text_fmt(
            s_ui.devices_filament_text,
            "%s\nAFC lanes: %u",
            model->filament_mode[0] != '\0'
                ? model->filament_mode
                : "Standard extruder",
            (unsigned)model->afc_lane_count
        );
    }

    if (s_ui.settings_build_text != NULL) {
        lv_label_set_text_fmt(
            s_ui.settings_build_text,
            "DragonTouch %s\nESP-IDF %s",
            model->firmware_version[0] != '\0'
                ? model->firmware_version
                : "--",
            model->idf_version[0] != '\0'
                ? model->idf_version
                : "--"
        );
    }

    if (s_ui.settings_memory_text != NULL) {
        lv_label_set_text_fmt(
            s_ui.settings_memory_text,
            "Internal: %u free | %u largest\n"
            "PSRAM: %u free | %u largest",
            (unsigned)model->internal_free,
            (unsigned)model->internal_largest,
            (unsigned)model->psram_free,
            (unsigned)model->psram_largest
        );
    }

    if (s_ui.settings_portal_text != NULL) {
        char portal[192] = {0};

        if (model->local_ip[0] != '\0') {
            snprintf(
                portal,
                sizeof(portal),
                "Printer setup:\nhttp://%s/dragontouch",
                model->local_ip
            );
        } else {
            snprintf(
                portal,
                sizeof(portal),
                "Printer setup:\nhttp://192.168.4.1/dragontouch"
            );
        }

        lv_label_set_text(
            s_ui.settings_portal_text,
            portal
        );
    }


    if (s_ui.settings_update_text != NULL) {
        char update[224] = {0};

        if (model->local_ip[0] != '\0') {
            snprintf(
                update,
                sizeof(update),
                "Browser-assisted OTA:\n"
                "http://%s/setup\n"
                "Upload the DragonTouch application .bin there.",
                model->local_ip
            );
        } else {
            snprintf(
                update,
                sizeof(update),
                "Browser-assisted OTA:\n"
                "http://192.168.4.1/setup\n"
                "Upload the DragonTouch application .bin there."
            );
        }

        lv_label_set_text(
            s_ui.settings_update_text,
            update
        );
    }

    /*
     * DT_STAGE6_NULL_SAFE_REBOOT_BUTTON
     *
     * Devices and Settings are lazy/recycled pages. render_system_model()
     * is shared by both, so the Settings-only reboot button may legitimately
     * be NULL while the Devices page is being built.
     */
    if (s_ui.settings_reboot_button != NULL) {
        set_button_enabled(
            s_ui.settings_reboot_button,
            true
        );
    }


    if (s_ui.settings_factory_reset_button != NULL) {
        set_button_enabled(
            s_ui.settings_factory_reset_button,
            true
        );
    }
}


/*
 * DT_STAGE6_PAGE_SCROLL_SYSTEM_CARDS
 *
 * Devices and Settings use page-level scrolling. Their individual cards
 * should size themselves to their contents and must never become nested
 * scroll containers.
 */
static void fit_system_card_to_content(lv_obj_t *card)
{
    if (card == NULL) {
        return;
    }

    lv_obj_set_width(
        card,
        LV_PCT(100)
    );

    lv_obj_set_height(
        card,
        LV_SIZE_CONTENT
    );

    lv_obj_set_flex_grow(
        card,
        0
    );

    lv_obj_remove_flag(
        card,
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_scrollbar_mode(
        card,
        LV_SCROLLBAR_MODE_OFF
    );
}


static void enable_system_page_scrolling(lv_obj_t *page)
{
    if (page == NULL) {
        return;
    }

    lv_obj_add_flag(
        page,
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_scroll_dir(
        page,
        LV_DIR_VER
    );

    lv_obj_set_scrollbar_mode(
        page,
        LV_SCROLLBAR_MODE_AUTO
    );
}


static void create_devices_page(lv_obj_t *page)
{
    enable_system_page_scrolling(page);

    /* DT_STAGE6_FULL_WIDTH_SYSTEM_CARDS */
    create_page_heading(
        page,
        "Devices",
        "Connected printer, network, and filament-system status."
    );

    lv_obj_t *printer =
        make_control_card(
            page,
            "PRINTER",
            "Waiting for runtime status..."
        );

    fit_system_card_to_content(
        printer
    );

    s_ui.devices_printer_text =
        lv_obj_get_child(
            printer,
            1
        );

    lv_label_set_long_mode(
        s_ui.devices_printer_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.devices_printer_text,
        LV_PCT(100)
    );

    lv_obj_t *network =
        make_control_card(
            page,
            "NETWORK",
            "Waiting for Wi-Fi status..."
        );

    fit_system_card_to_content(
        network
    );

    s_ui.devices_network_text =
        lv_obj_get_child(
            network,
            1
        );

    lv_label_set_long_mode(
        s_ui.devices_network_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.devices_network_text,
        LV_PCT(100)
    );

    lv_obj_t *filament =
        make_control_card(
            page,
            "FILAMENT SYSTEM",
            "Waiting for capability discovery..."
        );

    fit_system_card_to_content(
        filament
    );

    s_ui.devices_filament_text =
        lv_obj_get_child(
            filament,
            1
        );

    lv_label_set_long_mode(
        s_ui.devices_filament_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.devices_filament_text,
        LV_PCT(100)
    );

    render_system_model();
}


static void create_settings_page(lv_obj_t *page)
{
    enable_system_page_scrolling(page);

    /* DT_STAGE6B_MAINTENANCE */

    create_page_heading(
        page,
        "Settings",
        "DragonTouch diagnostics and local configuration access."
    );

    lv_obj_t *build =
        make_control_card(
            page,
            "SOFTWARE",
            "Version unavailable"
        );

    fit_system_card_to_content(
        build
    );

    s_ui.settings_build_text =
        lv_obj_get_child(
            build,
            1
        );

    lv_obj_t *memory =
        make_control_card(
            page,
            "MEMORY",
            "Heap diagnostics unavailable"
        );

    fit_system_card_to_content(
        memory
    );

    s_ui.settings_memory_text =
        lv_obj_get_child(
            memory,
            1
        );

    lv_obj_t *portal =
        make_control_card(
            page,
            "WEB CONFIGURATION",
            "Printer setup URL unavailable"
        );

    fit_system_card_to_content(
        portal
    );

    s_ui.settings_portal_text =
        lv_obj_get_child(
            portal,
            1
        );

    lv_label_set_long_mode(
        s_ui.settings_portal_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.settings_portal_text,
        LV_PCT(100)
    );

    s_ui.settings_reboot_button =
        make_guarded_action(
            portal,
            "Reboot DragonTouch",
            &CONFIRM_SYSTEM_REBOOT
        );

    size_card_action(
        s_ui.settings_reboot_button
    );

    lv_obj_t *update =
        make_control_card(
            page,
            "FIRMWARE UPDATE",
            "Browser-assisted OTA URL unavailable"
        );

    fit_system_card_to_content(
        update
    );

    s_ui.settings_update_text =
        lv_obj_get_child(
            update,
            1
        );

    lv_label_set_long_mode(
        s_ui.settings_update_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.settings_update_text,
        LV_PCT(100)
    );

    lv_obj_t *danger =
        make_control_card(
            page,
            "MAINTENANCE",
            "Factory reset affects DragonTouch connectivity only."
        );

    fit_system_card_to_content(
        danger
    );

    s_ui.settings_factory_reset_button =
        make_guarded_action(
            danger,
            "Factory reset",
            &CONFIRM_SYSTEM_FACTORY_RESET
        );

    size_card_action(
        s_ui.settings_factory_reset_button
    );

    render_system_model();
}


static void create_filament_page(lv_obj_t *page)
{
    ESP_LOGI(TAG, "FILAMENT_BUILD begin");

    lv_obj_t *heading_row =
        create_page_heading(
            page,
            "Filament",
            "AFC lanes, printer macros, and manual extruder controls."
        );

    lv_obj_t *status =
        make_label(
            heading_row,
            "Capability-aware",
            DT_COLOR_MUTED
        );

    lv_obj_set_flex_grow(status, 1);

    lv_obj_set_style_text_align(
        status,
        LV_TEXT_ALIGN_RIGHT,
        0
    );

    lv_obj_t *card =
        make_card(
            page,
            "FILAMENT SYSTEM"
        );

    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);

    s_ui.filament_mode_text =
        make_label(
            card,
            "Detecting printer capabilities...",
            DT_COLOR_TEXT
        );

    s_ui.afc_summary_text =
        make_label(
            card,
            "No AFC lanes detected.",
            DT_COLOR_MUTED
        );

    lv_obj_set_width(
        s_ui.afc_summary_text,
        LV_PCT(100)
    );

    lv_obj_t *afc_nav =
        lv_obj_create(card);

    lv_obj_remove_style_all(afc_nav);
    lv_obj_set_size(afc_nav, LV_PCT(100), 34);
    lv_obj_set_layout(afc_nav, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(afc_nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(afc_nav, 8, 0);

    s_ui.afc_previous_button =
        make_action(afc_nav, "<", false);

    s_ui.afc_next_button =
        make_action(afc_nav, ">", false);

    lv_obj_add_event_cb(
        s_ui.afc_previous_button,
        afc_previous_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        s_ui.afc_next_button,
        afc_next_event,
        LV_EVENT_CLICKED,
        NULL
    );

    for (
        size_t slot = 0;
        slot < DT_UI_AFC_LANE_PAGE_SIZE;
        ++slot
    ) {
        lv_obj_t *lane_row =
            lv_obj_create(card);

        style_surface(lane_row);
        lv_obj_set_width(lane_row, LV_PCT(100));
        lv_obj_set_height(lane_row, 82);
        lv_obj_set_layout(lane_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(
            lane_row,
            LV_FLEX_FLOW_ROW
        );
        lv_obj_set_style_pad_column(
            lane_row,
            8,
            0
        );

        s_ui.afc_lane_rows[slot] =
            lane_row;

        lv_obj_t *text_col =
            lv_obj_create(lane_row);

        lv_obj_remove_style_all(text_col);
        lv_obj_set_height(text_col, LV_PCT(100));
        lv_obj_set_flex_grow(text_col, 1);
        lv_obj_set_layout(text_col, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(
            text_col,
            LV_FLEX_FLOW_COLUMN
        );
        lv_obj_set_style_pad_row(
            text_col,
            3,
            0
        );

        s_ui.afc_lane_title[slot] =
            make_label(
                text_col,
                "lane",
                DT_COLOR_TEXT
            );

        s_ui.afc_lane_detail[slot] =
            make_label(
                text_col,
                "--",
                DT_COLOR_MUTED
            );

        lv_label_set_long_mode(
            s_ui.afc_lane_detail[slot],
            LV_LABEL_LONG_MODE_WRAP
        );

        lv_obj_set_width(
            s_ui.afc_lane_detail[slot],
            LV_PCT(100)
        );

        lv_obj_t *action_col =
            lv_obj_create(lane_row);

        lv_obj_remove_style_all(action_col);
        lv_obj_set_size(action_col, 150, LV_PCT(100));
        lv_obj_set_layout(action_col, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(
            action_col,
            LV_FLEX_FLOW_COLUMN
        );
        lv_obj_set_style_pad_row(
            action_col,
            5,
            0
        );

        s_ui.afc_lane_load_button[slot] =
            make_action(
                action_col,
                "Load",
                true
            );

        s_ui.afc_lane_eject_button[slot] =
            make_action(
                action_col,
                "Eject",
                false
            );

        lv_obj_set_width(
            s_ui.afc_lane_load_button[slot],
            LV_PCT(100)
        );

        lv_obj_set_width(
            s_ui.afc_lane_eject_button[slot],
            LV_PCT(100)
        );

        lv_obj_set_height(
            s_ui.afc_lane_load_button[slot],
            32
        );

        lv_obj_set_height(
            s_ui.afc_lane_eject_button[slot],
            32
        );

        lv_obj_add_event_cb(
            s_ui.afc_lane_load_button[slot],
            afc_lane_load_event,
            LV_EVENT_CLICKED,
            (void *)(uintptr_t)slot
        );

        lv_obj_add_event_cb(
            s_ui.afc_lane_eject_button[slot],
            afc_lane_eject_event,
            LV_EVENT_CLICKED,
            (void *)(uintptr_t)slot
        );
    }

    s_ui.afc_message_text =
        make_label(
            card,
            "AFC state unavailable.",
            DT_COLOR_MUTED
        );

    lv_label_set_long_mode(
        s_ui.afc_message_text,
        LV_LABEL_LONG_MODE_DOTS
    );

    lv_obj_set_width(
        s_ui.afc_message_text,
        LV_PCT(100)
    );

    lv_obj_t *recovery_row =
        lv_obj_create(card);

    lv_obj_remove_style_all(
        recovery_row
    );

    lv_obj_set_size(
        recovery_row,
        LV_PCT(100),
        34
    );

    lv_obj_set_layout(
        recovery_row,
        LV_LAYOUT_FLEX
    );

    lv_obj_set_flex_flow(
        recovery_row,
        LV_FLEX_FLOW_ROW
    );

    lv_obj_set_style_pad_column(
        recovery_row,
        8,
        0
    );

    s_ui.afc_clear_message_button =
        make_action(
            recovery_row,
            "Clear AFC message",
            false
        );

    s_ui.afc_resume_button =
        make_action(
            recovery_row,
            "Resume",
            true
        );

    lv_obj_set_height(
        s_ui.afc_clear_message_button,
        32
    );

    lv_obj_set_height(
        s_ui.afc_resume_button,
        32
    );

    lv_obj_add_event_cb(
        s_ui.afc_clear_message_button,
        afc_clear_message_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        s_ui.afc_resume_button,
        afc_resume_event,
        LV_EVENT_CLICKED,
        NULL
    );

    s_ui.filament_nozzle_text =
        make_label(
            card,
            "Temperature unavailable",
            DT_COLOR_TEXT
        );

    s_ui.filament_generic_macro_row =
        lv_obj_create(card);

    lv_obj_remove_style_all(
        s_ui.filament_generic_macro_row
    );

    lv_obj_set_size(
        s_ui.filament_generic_macro_row,
        LV_PCT(100),
        38
    );

    lv_obj_set_layout(
        s_ui.filament_generic_macro_row,
        LV_LAYOUT_FLEX
    );

    lv_obj_set_flex_flow(
        s_ui.filament_generic_macro_row,
        LV_FLEX_FLOW_ROW
    );

    lv_obj_set_style_pad_column(
        s_ui.filament_generic_macro_row,
        8,
        0
    );

    s_ui.filament_load_button =
        make_guarded_action(
            s_ui.filament_generic_macro_row,
            "Load",
            &CONFIRM_FILAMENT_LOAD
        );

    s_ui.filament_unload_button =
        make_guarded_action(
            s_ui.filament_generic_macro_row,
            "Unload",
            &CONFIRM_FILAMENT_UNLOAD
        );

    lv_obj_t *manual_row =
        lv_obj_create(card);

    lv_obj_remove_style_all(manual_row);
    lv_obj_set_size(manual_row, LV_PCT(100), 38);
    lv_obj_set_layout(manual_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(manual_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(manual_row, 8, 0);

    s_ui.filament_extrude_button =
        make_guarded_action(
            manual_row,
            "Extrude 10",
            &CONFIRM_EXTRUDE
        );

    s_ui.filament_retract_button =
        make_guarded_action(
            manual_row,
            "Retract 10",
            &CONFIRM_RETRACT
        );

    s_ui.filament_heat_button =
        make_guarded_action(
            manual_row,
            "220 C",
            &CONFIRM_HEAT
        );

    render_filament_model();

    ESP_LOGI(TAG, "FILAMENT_BUILD complete");
}


static void create_files_page(lv_obj_t *page)
{
    static const char *names[] = {
        "Browse",
        "Details",
        "USB"
    };

    lv_obj_t *tabs =
        create_page_heading(
            page,
            "Print files",
            "Browse Moonraker storage, inspect metadata, "
            "and confirm before starting a print."
        );

    for (size_t i = 0; i < DT_FILES_TAB_COUNT; ++i) {
        s_ui.file_tabs[i] =
            create_tab(
                tabs,
                names[i],
                file_tab_event,
                i
            );

        s_ui.file_panels[i] =
            create_tab_panel(page);
    }

    lv_obj_t *browser =
        make_control_card(
            s_ui.file_panels[0],
            "PRINTER STORAGE",
            "/"
        );

    s_ui.file_path_text = lv_obj_get_child(browser, 1);

    lv_obj_t *nav = lv_obj_create(browser);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, LV_PCT(100), 38);
    lv_obj_set_layout(nav, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 6, 0);

    s_ui.file_up_button =
        make_action(nav, "Up", false);
    s_ui.file_refresh_button =
        make_action(nav, "Refresh", false);
    s_ui.file_previous_button =
        make_action(nav, "<", false);
    s_ui.file_next_button =
        make_action(nav, ">", false);

    lv_obj_add_event_cb(
        s_ui.file_up_button,
        file_up_event,
        LV_EVENT_CLICKED,
        NULL
    );
    lv_obj_add_event_cb(
        s_ui.file_refresh_button,
        file_refresh_event,
        LV_EVENT_CLICKED,
        NULL
    );
    lv_obj_add_event_cb(
        s_ui.file_previous_button,
        file_previous_event,
        LV_EVENT_CLICKED,
        NULL
    );
    lv_obj_add_event_cb(
        s_ui.file_next_button,
        file_next_event,
        LV_EVENT_CLICKED,
        NULL
    );

    s_ui.file_status_text =
        make_label(
            browser,
            "Waiting for Moonraker...",
            DT_COLOR_MUTED
        );

    lv_obj_set_width(
        s_ui.file_status_text,
        LV_PCT(100)
    );

    for (size_t i = 0; i < DT_UI_FILE_ENTRY_MAX; ++i) {
        lv_obj_t *button =
            make_action(browser, "--", false);

        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, 34);
        lv_obj_set_flex_grow(button, 0);

        s_ui.file_entry_buttons[i] = button;
        s_ui.file_entry_labels[i] =
            lv_obj_get_child(button, 0);

        lv_label_set_long_mode(
            s_ui.file_entry_labels[i],
            LV_LABEL_LONG_MODE_DOTS
        );

        lv_obj_set_width(
            s_ui.file_entry_labels[i],
            LV_PCT(92)
        );

        lv_obj_add_event_cb(
            button,
            file_entry_event,
            LV_EVENT_CLICKED,
            (void *)(uintptr_t)i
        );
    }

    lv_obj_t *details =
        make_control_card(
            s_ui.file_panels[1],
            "FILE DETAILS",
            "Select a G-code file to inspect its Moonraker metadata."
        );

    s_ui.file_detail_text = lv_obj_get_child(details, 1);

    lv_label_set_long_mode(
        s_ui.file_detail_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.file_detail_text,
        LV_PCT(100)
    );

    s_ui.file_start_button =
        make_action(details, "Start print", true);

    size_card_action(s_ui.file_start_button);

    lv_obj_add_event_cb(
        s_ui.file_start_button,
        file_start_event,
        LV_EVENT_CLICKED,
        NULL
    );

    set_button_enabled(
        s_ui.file_start_button,
        false
    );

    lv_obj_t *usb =
        make_control_card(
            s_ui.file_panels[2],
            "USB STORAGE",
            "Removable storage is not configured for this DragonTouch target."
        );

    lv_obj_t *usb_button =
        make_action(usb, "Unavailable", false);

    size_card_action(usb_button);
    set_button_enabled(usb_button, false);

    select_tab(
        s_ui.file_tabs,
        s_ui.file_panels,
        DT_FILES_TAB_COUNT,
        0
    );

    render_files_model();
}



static void log_ui_heap(
    const char *phase,
    dt_ui_page_t page
)
{
    const size_t internal_free =
        heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    const size_t internal_largest =
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL |
            MALLOC_CAP_8BIT
        );

    const size_t psram_free =
        heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT
        );

    ESP_LOGI(
        TAG,
        "UI_HEAP %s page=%d internal=%u largest=%u psram=%u",
        phase,
        (int)page,
        (unsigned)internal_free,
        (unsigned)internal_largest,
        (unsigned)psram_free
    );
}


static void clear_recycled_page_refs(
    dt_ui_page_t page
)
{
    if (page != DT_UI_PAGE_CONTROL) {
        return;
    }

    s_ui.home_button = NULL;
    s_ui.heat_button = NULL;
    s_ui.extrude_button = NULL;
    s_ui.retract_button = NULL;

    s_ui.axes_text = NULL;
    s_ui.nozzle_control_text = NULL;
    s_ui.bed_control_text = NULL;
    s_ui.fan_control_text = NULL;

    for (size_t i = 0; i < 6; ++i) {
        s_ui.jog_buttons[i] = NULL;
    }

    for (size_t i = 0; i < 3; ++i) {
        s_ui.bed_buttons[i] = NULL;
        s_ui.fan_buttons[i] = NULL;
    }

    for (
        size_t i = 0;
        i < DT_CONTROL_TAB_COUNT;
        ++i
    ) {
        s_ui.control_tabs[i] = NULL;
        s_ui.control_panels[i] = NULL;
    }
}


static void recycle_secondary_pages(
    dt_ui_page_t keep
)
{
    for (
        int i = DT_UI_PAGE_CONTROL;
        i <= DT_UI_PAGE_SETTINGS;
        ++i
    ) {
        const dt_ui_page_t page =
            (dt_ui_page_t)i;

        if (
            page == keep ||
            !s_ui.page_built[page]
        ) {
            continue;
        }

        log_ui_heap(
            "before-clean",
            page
        );

        lv_obj_clean(
            s_ui.pages[page]
        );

        clear_recycled_page_refs(
            page
        );

        if (page == DT_UI_PAGE_FILES) {
            s_ui.file_path_text = NULL;
            s_ui.file_status_text = NULL;
            s_ui.file_up_button = NULL;
            s_ui.file_refresh_button = NULL;
            s_ui.file_previous_button = NULL;
            s_ui.file_next_button = NULL;
            s_ui.file_detail_text = NULL;
            s_ui.file_start_button = NULL;

            for (size_t file_i = 0; file_i < DT_UI_FILE_ENTRY_MAX; ++file_i) {
                s_ui.file_entry_buttons[file_i] = NULL;
                s_ui.file_entry_labels[file_i] = NULL;
            }

            for (size_t tab_i = 0; tab_i < DT_FILES_TAB_COUNT; ++tab_i) {
                s_ui.file_tabs[tab_i] = NULL;
                s_ui.file_panels[tab_i] = NULL;
            }
        }

        if (page == DT_UI_PAGE_FILAMENT) {
            s_ui.filament_mode_text = NULL;
            s_ui.filament_capability_text = NULL;
            s_ui.filament_nozzle_text = NULL;
            s_ui.filament_load_button = NULL;
            s_ui.filament_unload_button = NULL;
            s_ui.filament_extrude_button = NULL;
            s_ui.filament_retract_button = NULL;
            s_ui.filament_heat_button = NULL;
            s_ui.filament_generic_macro_row = NULL;
            s_ui.afc_summary_text = NULL;
            s_ui.afc_message_text = NULL;
            s_ui.afc_previous_button = NULL;
            s_ui.afc_next_button = NULL;
            s_ui.afc_clear_message_button = NULL;
            s_ui.afc_resume_button = NULL;

            for (
                size_t slot = 0;
                slot < DT_UI_AFC_LANE_PAGE_SIZE;
                ++slot
            ) {
                s_ui.afc_lane_rows[slot] = NULL;
                s_ui.afc_lane_title[slot] = NULL;
                s_ui.afc_lane_detail[slot] = NULL;
                s_ui.afc_lane_load_button[slot] = NULL;
                s_ui.afc_lane_eject_button[slot] = NULL;
            }
        }

        if (page == DT_UI_PAGE_DEVICES) {
            s_ui.devices_printer_text = NULL;
            s_ui.devices_network_text = NULL;
            s_ui.devices_filament_text = NULL;
        }

        if (page == DT_UI_PAGE_SETTINGS) {
            s_ui.settings_build_text = NULL;
            s_ui.settings_memory_text = NULL;
            s_ui.settings_portal_text = NULL;
            s_ui.settings_update_text = NULL;
            s_ui.settings_reboot_button = NULL;
            s_ui.settings_factory_reset_button = NULL;
        }

        s_ui.page_built[page] =
            false;

        log_ui_heap(
            "after-clean",
            page
        );
    }
}


static void ensure_page_built(dt_ui_page_t page)
{
    if (
        page < DT_UI_PAGE_HOME ||
        page > DT_UI_PAGE_SETTINGS
    ) {
        return;
    }

    if (s_ui.page_built[page]) {
        return;
    }

    const int64_t started_us = esp_timer_get_time();

    log_ui_heap(
        "before-build",
        page
    );

    ESP_LOGI(
        TAG,
        "lazy build page=%d start",
        (int)page
    );

    switch (page) {
    case DT_UI_PAGE_HOME:
        create_home_page(
            s_ui.pages[DT_UI_PAGE_HOME]
        );
        break;

    case DT_UI_PAGE_CONTROL:
        create_control_page(
            s_ui.pages[DT_UI_PAGE_CONTROL]
        );
        break;

    case DT_UI_PAGE_FILES:
        create_files_page(
            s_ui.pages[DT_UI_PAGE_FILES]
        );
        break;

    case DT_UI_PAGE_FILAMENT:
        create_filament_page(
            s_ui.pages[DT_UI_PAGE_FILAMENT]
        );
        break;

    case DT_UI_PAGE_DEVICES:
        create_devices_page(
            s_ui.pages[DT_UI_PAGE_DEVICES]
        );
        break;

    case DT_UI_PAGE_SETTINGS:
        create_settings_page(
            s_ui.pages[DT_UI_PAGE_SETTINGS]
        );
        break;

    default:
        return;
    }

    s_ui.page_built[page] = true;

    ESP_LOGI(
        TAG,
        "lazy build page=%d complete in %lld ms",
        (int)page,
        (long long)(
            (
                esp_timer_get_time() -
                started_us
            ) / 1000
        )
    );

    log_ui_heap(
        "after-build",
        page
    );
}

static void create_pages(lv_obj_t *content)
{
    ESP_LOGI(
        TAG,
        "lazy page mode: creating containers"
    );

    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        s_ui.pages[i] = make_page(content);

        lv_obj_add_flag(
            s_ui.pages[i],
            LV_OBJ_FLAG_HIDDEN
        );
    }

    /*
     * Home is the only full page tree constructed synchronously at boot.
     */
    ensure_page_built(
        DT_UI_PAGE_HOME
    );

    ESP_LOGI(
        TAG,
        "lazy page mode: boot page complete"
    );
}

static void create_dialog_overlay(void)
{
    s_ui.dialog_scrim = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.dialog_scrim);
    lv_obj_add_flag(s_ui.dialog_scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_ui.dialog_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.dialog_scrim, color(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ui.dialog_scrim, LV_OPA_70, 0);

    lv_obj_t *dialog = lv_obj_create(s_ui.dialog_scrim);
    style_surface(dialog);
    lv_obj_set_size(dialog, 420, 222);
    lv_obj_center(dialog);
    lv_obj_set_layout(dialog, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dialog, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(dialog, 18, 0);
    lv_obj_set_style_pad_row(dialog, 12, 0);
    s_ui.dialog_title = make_label(dialog, "Confirm request", DT_COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.dialog_title, &lv_font_montserrat_20, 0);
    s_ui.dialog_body = make_label(dialog, "Review this printer-owned request.", DT_COLOR_MUTED);
    lv_label_set_long_mode(s_ui.dialog_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_ui.dialog_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.dialog_body, 1);
    lv_obj_t *guard = make_label(dialog, "PRINTER COMMAND | CONFIRM BEFORE SEND", DT_COLOR_WARNING);
    lv_obj_set_style_text_font(guard, &lv_font_montserrat_12, 0);

    lv_obj_t *actions = lv_obj_create(dialog);
    lv_obj_remove_style_all(actions);
    lv_obj_set_size(actions, LV_PCT(100), 42);
    lv_obj_set_layout(actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 8, 0);
    lv_obj_t *back = make_action(actions, "Go back", false);
    lv_obj_add_event_cb(back, close_dialog, LV_EVENT_CLICKED, NULL);
    s_ui.dialog_confirm = make_action(actions, "Request", true);
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
    );
    lv_obj_add_flag(s_ui.dialog_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void create_shell(lv_display_t *display)
{
    static const dt_nav_icon_t nav_icons[DT_PAGE_COUNT] = {
        DT_NAV_ICON_HOME,
        DT_NAV_ICON_CONTROL,
        DT_NAV_ICON_FILES,
        DT_NAV_ICON_FILAMENT,
        DT_NAV_ICON_DEVICES,
        DT_NAV_ICON_SETTINGS,
    };

    s_ui.screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_ui.screen);
    lv_obj_set_style_bg_color(s_ui.screen, color(DT_COLOR_BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_ui.screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_ui.screen, color(DT_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_ui.screen, &lv_font_montserrat_14, 0);
    lv_obj_set_layout(s_ui.screen, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ui.screen, LV_FLEX_FLOW_ROW);

    lv_obj_t *rail = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, 60, LV_PCT(100));
    lv_obj_set_style_bg_color(rail, color(DT_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(rail, color(DT_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_pad_all(rail, 6, 0);
    lv_obj_set_style_pad_row(rail, 4, 0);
    lv_obj_set_layout(rail, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        if (i == DT_UI_PAGE_SETTINGS) {
            lv_obj_t *spacer = lv_obj_create(rail);
            lv_obj_remove_style_all(spacer);
            lv_obj_set_width(spacer, LV_PCT(100));
            lv_obj_set_flex_grow(spacer, 1);
        }
        lv_obj_t *button = lv_button_create(rail);
        lv_obj_remove_style_all(button);
        s_ui.nav_buttons[i] = button;
        lv_obj_set_size(button, LV_PCT(100), 52);
        lv_obj_set_style_bg_color(button, color(DT_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_radius(button, 4, 0);
        lv_obj_set_layout(button, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(button, 1, 0);
        lv_obj_add_event_cb(button, nav_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_ui.nav_icons[i] = make_nav_icon(button, nav_icons[i]);
    }

    lv_obj_t *body = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_height(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_layout(body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *header = lv_obj_create(body);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, LV_PCT(100), 56);
    lv_obj_set_style_pad_hor(header, 14, 0);
    lv_obj_set_style_border_color(header, color(DT_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_t *brand = make_label(header, "DragonTouch", DT_COLOR_TEXT);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_20, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 0, 0);

    s_ui.connection_dot = lv_obj_create(header);
    lv_obj_remove_style_all(s_ui.connection_dot);
    lv_obj_set_size(s_ui.connection_dot, 9, 9);
    lv_obj_set_style_radius(s_ui.connection_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.connection_dot, color(DT_COLOR_MUTED), 0);
    lv_obj_set_style_bg_opa(s_ui.connection_dot, LV_OPA_COVER, 0);
    lv_obj_align(s_ui.connection_dot, LV_ALIGN_RIGHT_MID, -230, 0);
    s_ui.connection_text = make_label(header, "Offline", DT_COLOR_MUTED);
    lv_obj_set_width(s_ui.connection_text, 82);
    lv_obj_set_style_text_align(s_ui.connection_text, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_ui.connection_text, LV_ALIGN_RIGHT_MID, -140, 0);
    s_ui.device_name = make_label(header, "No printer", DT_COLOR_TEXT);
    lv_obj_set_width(s_ui.device_name, 132);
    lv_label_set_long_mode(s_ui.device_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_ui.device_name, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_ui.device_name, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *content = lv_obj_create(body);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    create_pages(content);
    create_dialog_overlay();
    show_page(DT_UI_PAGE_HOME);

    lv_display_set_default(display);
    lv_screen_load(s_ui.screen);
}

esp_err_t dt_ui_create(lv_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_ui, 0, sizeof(s_ui));
    lv_display_set_default(display);
    create_shell(display);
    s_ui.ready = true;
    ESP_LOGI(TAG, "DragonTouch LVGL shell created");
    return ESP_OK;
}

esp_err_t dt_ui_show_page(dt_ui_page_t page)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (page < DT_UI_PAGE_HOME || page > DT_UI_PAGE_SETTINGS) {
        return ESP_ERR_INVALID_ARG;
    }
    show_page(page);
    return ESP_OK;
}

static const char *job_state_text(dt_ui_job_state_t state)
{
    switch (state) {
    case DT_UI_JOB_PRINTING: return "Printing";
    case DT_UI_JOB_PAUSED: return "Paused";
    case DT_UI_JOB_COMPLETE: return "Complete";
    case DT_UI_JOB_ERROR: return "Printer fault";
    case DT_UI_JOB_IDLE:
    default: return "Ready";
    }
}

static void format_duration(char *buffer, size_t length, uint32_t seconds)
{
    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds % 3600) / 60;
    snprintf(buffer, length, "%luh %02lum", (unsigned long)hours, (unsigned long)minutes);
}

static void format_temperature(char *buffer, size_t length, float current, float target)
{
    int current_tenths = (int)(current * 10.0f + (current >= 0.0f ? 0.5f : -0.5f));
    int target_whole = (int)(target + (target >= 0.0f ? 0.5f : -0.5f));
    int fraction = current_tenths < 0 ? -(current_tenths % 10) : current_tenths % 10;
    snprintf(buffer, length, "%d.%d / %d °C", current_tenths / 10, fraction, target_whole);
}

esp_err_t dt_ui_update(const dt_ui_model_t *model)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ui.current_job_state =
        model->job_state;

    s_ui.current_connection = model->connection;
    if (s_ui.file_start_button != NULL) {
        render_files_model();
    }

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
}





esp_err_t dt_ui_update_system(
    const dt_ui_system_model_t *model
)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ui.system_model = *model;

    if (
        s_ui.page_built[DT_UI_PAGE_DEVICES] ||
        s_ui.page_built[DT_UI_PAGE_SETTINGS]
    ) {
        render_system_model();
    }

    return ESP_OK;
}


esp_err_t dt_ui_update_filament(
    const dt_ui_filament_model_t *model
)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ui.filament_model = *model;

    if (s_ui.page_built[DT_UI_PAGE_FILAMENT]) {
        render_filament_model();
    }

    return ESP_OK;
}


esp_err_t dt_ui_update_files(
    const dt_ui_files_model_t *model
)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ui.files_model = *model;

    if (s_ui.page_built[DT_UI_PAGE_FILES]) {
        render_files_model();
    }

    return ESP_OK;
}


esp_err_t dt_ui_set_filament_request_handler(
    dt_ui_filament_request_handler_t handler,
    void *ctx
)
{
    s_filament_request_handler = handler;
    s_filament_request_ctx = ctx;
    return ESP_OK;
}


esp_err_t dt_ui_set_file_request_handler(
    dt_ui_file_request_handler_t handler,
    void *ctx
)
{
    s_file_request_handler = handler;
    s_file_request_ctx = ctx;
    return ESP_OK;
}


esp_err_t dt_ui_set_action_handler(
    dt_ui_action_handler_t handler,
    void *ctx
)
{
    s_action_handler = handler;
    s_action_ctx = ctx;
    return ESP_OK;
}
