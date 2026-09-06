#include "dt_ui.h"

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "dt_printer_profile.h"

/*
 * DT_FILE_THUMBNAIL_CACHE_DROP
 *
 * lv_image_cache_drop() -- needed below so we don't serve a stale decoded
 * image out of LVGL's image cache when re-using the same lv_image_dsc_t
 * for a new file's PNG bytes (see the call site for why).
 */
#include "misc/cache/instance/lv_image_cache.h"

/*
 * DT_FILE_THUMBNAIL_DECODE_DIAG
 *
 * lv_image_decoder_dsc_t is only forward-declared (opaque) in the public
 * LVGL headers (misc/lv_types.h) -- its real struct body lives here.
 * webcam_decode_to_rgb() needs the complete type: it puts one on the
 * stack and reads dsc.decoded while walking the JPEG's MCU blocks.
 */
#include "draw/lv_image_decoder_private.h"

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

/*
 * DT_MOVE_STEP
 *
 * Jog/extrude distances and extrude speeds, chosen on screen. Whole
 * millimetres so the gcode never needs float formatting.
 */
#define DT_MOVE_STEP_COUNT 4
#define DT_EXTRUDE_SPEED_COUNT 2

/*
 * DT_WEBCAM_PRESCALE
 *
 * One pre-scaled copy of the current frame per view, sized to that view's
 * pane so LVGL can blit it 1:1.
 *
 * The alternative -- LV_IMAGE_ALIGN_CONTAIN over the native 640x480 RGB888
 * frame -- makes LVGL software-rescale the whole image on EVERY redraw that
 * touches the pane, while holding the LVGL lock. A status label changing was
 * enough to starve the periodic model pushes ("LVGL lock timeout; skipping
 * ... update"). Scaling once per frame moves that cost off the draw path.
 *
 * The two panes are different sizes, so they cannot share one buffer.
 */
typedef struct {
    uint8_t *data;
    size_t capacity;
    lv_image_dsc_t dsc;
    uint32_t width;
    uint32_t height;
    uint32_t revision;
} dt_webcam_view_t;
#define DT_FILES_TAB_COUNT 3

typedef enum {
    DT_NAV_ICON_HOME = 0,
    DT_NAV_ICON_CONTROL,
    DT_NAV_ICON_FILES,
    DT_NAV_ICON_FILAMENT,
    DT_NAV_ICON_DEVICES,

    /* DT_WEBCAM_SNAPSHOT */
    DT_NAV_ICON_WEBCAM,

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
    lv_obj_t *elapsed_text;
    lv_obj_t *remaining_text;
    lv_obj_t *estop_button;
    lv_obj_t *nozzle_text;
    lv_obj_t *bed_text;
    lv_obj_t *chamber_text;

    /* DT_WEBCAM_HOME_PANE: second view of the same decoded frame */
    lv_obj_t *home_webcam_image;
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

    /* DT_TOAST */
    lv_obj_t *toast_panel;
    lv_obj_t *toast_label;
    lv_timer_t *toast_timer;

    /* DT_PRINTER_LIST */
    lv_obj_t *printer_scrim;
    lv_obj_t *printer_list;
    lv_obj_t *printer_add_button;
    lv_obj_t *printer_rows[DT_UI_PRINTER_MAX];
    lv_obj_t *printer_entry_buttons[DT_UI_PRINTER_MAX];
    lv_obj_t *printer_delete_buttons[DT_UI_PRINTER_MAX];
    lv_obj_t *host_scrim;
    lv_obj_t *host_textarea;
    lv_obj_t *port_textarea;
    lv_obj_t *api_key_textarea;
    lv_obj_t *host_keyboard;
    dt_ui_printer_model_t printer_model;

    /* DT_MOVE_STEP */
    lv_obj_t *z_tilt_button;
    lv_obj_t *move_step_boxes[DT_MOVE_STEP_COUNT];
    lv_obj_t *extrude_step_boxes[DT_MOVE_STEP_COUNT];
    lv_obj_t *extrude_speed_boxes[DT_EXTRUDE_SPEED_COUNT];

    /* DT_TEMP_ENTRY */
    lv_obj_t *nozzle_set_button;
    lv_obj_t *nozzle_cooldown_button;
    lv_obj_t *bed_set_button;
    lv_obj_t *bed_cooldown_button;
    lv_obj_t *keypad_scrim;
    lv_obj_t *keypad_title;
    lv_obj_t *keypad_value;
    lv_obj_t *extrude_button;
    lv_obj_t *retract_button;

    /* Home-page quick-access macros; see dt_printer_profile.h */
    lv_obj_t *quick_home_button;
    lv_obj_t *quick_clean_nozzle_button;
    lv_obj_t *quick_macro2_button;

    lv_obj_t *jog_buttons[6];
    lv_obj_t *bed_buttons[3];
    lv_obj_t *fan_buttons[3];

    lv_obj_t *axes_text;
    lv_obj_t *nozzle_control_text;
    lv_obj_t *bed_control_text;
    lv_obj_t *fan_control_text;

    /* DT_DYNAMIC_AUX_FANS */
    lv_obj_t *aux_fan_cards[DT_UI_AUX_FAN_MAX];
    lv_obj_t *aux_fan_title[DT_UI_AUX_FAN_MAX];
    lv_obj_t *aux_fan_status[DT_UI_AUX_FAN_MAX];
    lv_obj_t *aux_fan_rows[DT_UI_AUX_FAN_MAX];
    lv_obj_t *aux_fan_buttons
        [DT_UI_AUX_FAN_MAX]
        [DT_UI_AUX_FAN_PRESET_COUNT];

    /*
     * DT_DYNAMIC_AUX_FANS_DIRECT_ROWS
     *
     * Up to four manual fan_generic rows are rendered inside one shallow card.
     */
    lv_obj_t *aux_manual_rows[4];
    lv_obj_t *aux_manual_labels[4];
    /* DT_DYNAMIC_AUX_FAN_SLIDERS */
    lv_obj_t *aux_manual_sliders[4];
    char aux_manual_names[4][64];
    bool aux_manual_dragging[4];
    size_t aux_manual_slots[4];
    size_t aux_manual_count;

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

    /* DT_FILE_THUMBNAIL_PREVIEW */
    lv_obj_t *file_thumbnail_row;
    lv_obj_t *file_thumbnail_image;
    lv_image_dsc_t file_thumbnail_dsc;

    char file_confirm_body[320];

    /* DT_WEBCAM_SNAPSHOT */
    lv_obj_t *webcam_status_text;
    lv_obj_t *webcam_image_wrap;
    lv_obj_t *webcam_image;
    lv_image_dsc_t webcam_dsc;     /* encoded JPEG: decoder input */
    lv_image_dsc_t webcam_rgb_dsc; /* decoded frame: what the widget draws */
    uint8_t *webcam_rgb_data;
    uint32_t webcam_decoded_revision;
    size_t webcam_rgb_capacity;

    /* DT_WEBCAM_PRESCALE */
    dt_webcam_view_t webcam_view_page;
    dt_webcam_view_t webcam_view_home;
    dt_ui_webcam_model_t webcam_model;

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
static dt_ui_state_t s_ui;

static dt_ui_action_handler_t s_action_handler;
static void *s_action_ctx;
static dt_ui_file_request_handler_t s_file_request_handler;
static void *s_file_request_ctx;
static dt_ui_filament_request_handler_t s_filament_request_handler;

/* DT_TEMP_ENTRY */
static dt_ui_temperature_request_handler_t s_temperature_request_handler;
static void *s_temperature_request_ctx;

/* DT_MOVE_STEP */
static dt_ui_move_request_handler_t s_move_request_handler;
static void *s_move_request_ctx;

/* DT_PRINTER_LIST */
static dt_ui_printer_request_handler_t s_printer_request_handler;
static void *s_printer_request_ctx;
static int s_pending_printer_index = -1;
static dt_ui_printer_request_t s_pending_printer_request;
static char s_printer_confirm_body[192];

static const int DT_MOVE_STEPS[DT_MOVE_STEP_COUNT] = {1, 10, 25, 50};
static const int DT_EXTRUDE_SPEEDS[DT_EXTRUDE_SPEED_COUNT] = {2, 10};

static int s_move_step_mm = 10;
static int s_extrude_step_mm = 10;
static int s_extrude_speed_mms = 2;
static void *s_filament_request_ctx;
static bool s_pending_filament_request_valid;

/* DT_MOVE_STEP */
static bool s_pending_move_request_valid;
static dt_ui_move_axis_t s_pending_move_axis;
static int s_pending_move_delta;
static int s_pending_move_speed;
static char s_extrude_confirm_body[192];
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
    case DT_NAV_ICON_WEBCAM:
        draw_icon_rect(layer, line_color, x, y, 2, 7, 20, 18);
        draw_icon_rect(layer, line_color, x, y, 8, 3, 14, 7);
        draw_icon_circle(layer, line_color, x, y, 11, 13, 4);
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
static void dispatch_action(dt_ui_action_t action);

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

    /*
     * DT_WEBCAM_AUTOLOAD
     *
     * Opening the webcam page pulls a frame straight away, so it is never
     * shown empty. This only queues the request -- the runtime task does
     * the fetch, exactly as it does for a tap on the image.
     */
    if (
        selected == DT_UI_PAGE_WEBCAM ||
        selected == DT_UI_PAGE_HOME
    ) {
        dispatch_action(DT_UI_ACTION_WEBCAM_REFRESH);
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

/* DT_Z_TILT */
static const dt_confirmation_t CONFIRM_Z_TILT = {
    "Run Z tilt adjust?",
    "The printer will execute Z_TILT_ADJUST and probe the bed. "
    "Keep the motion envelope clear.",
    "Run Z tilt",
    false,
    DT_UI_ACTION_Z_TILT,
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


/*
 * DT_TEMP_ENTRY
 *
 * Nozzle and bed maxima mirror the printer's configured max_temp. Klipper
 * would reject anything higher anyway; clamping here means the user sees a
 * capped number rather than an error after the fact.
 */
static dt_ui_heater_t s_keypad_heater;
static char s_keypad_digits[4];

/*
 * DT_MOVE_STEP
 *
 * lv_checkbox has no built-in radio behaviour, so exclusivity is enforced
 * by hand: on any tap the whole group is re-stated from the stored value.
 * That also re-checks the box if the user taps the one already selected,
 * so a group is never left with nothing chosen.
 */
enum {
    DT_STEP_GROUP_MOVE = 0,
    DT_STEP_GROUP_EXTRUDE_STEP,
    DT_STEP_GROUP_EXTRUDE_SPEED,
};

static void dispatch_move_request(
    dt_ui_move_axis_t axis,
    int delta_mm,
    int speed_mms
)
{
    if (s_move_request_handler != NULL) {
        s_move_request_handler(
            axis,
            delta_mm,
            speed_mms,
            s_move_request_ctx
        );
    }
}


static void step_group_apply(int group)
{
    lv_obj_t *const *boxes;
    const int *values;
    size_t count;
    int selected;

    switch (group) {
    case DT_STEP_GROUP_EXTRUDE_STEP:
        boxes = s_ui.extrude_step_boxes;
        values = DT_MOVE_STEPS;
        count = DT_MOVE_STEP_COUNT;
        selected = s_extrude_step_mm;
        break;

    case DT_STEP_GROUP_EXTRUDE_SPEED:
        boxes = s_ui.extrude_speed_boxes;
        values = DT_EXTRUDE_SPEEDS;
        count = DT_EXTRUDE_SPEED_COUNT;
        selected = s_extrude_speed_mms;
        break;

    default:
        boxes = s_ui.move_step_boxes;
        values = DT_MOVE_STEPS;
        count = DT_MOVE_STEP_COUNT;
        selected = s_move_step_mm;
        break;
    }

    for (size_t i = 0; i < count; ++i) {
        if (boxes[i] == NULL) {
            continue;
        }

        if (values[i] == selected) {
            lv_obj_add_state(boxes[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(boxes[i], LV_STATE_CHECKED);
        }
    }
}


static void step_select_event(lv_event_t *event)
{
    const uintptr_t packed =
        (uintptr_t)lv_event_get_user_data(event);

    const int group = (int)(packed >> 8);
    const size_t index = (size_t)(packed & 0xFFU);

    switch (group) {
    case DT_STEP_GROUP_EXTRUDE_STEP:
        s_extrude_step_mm = DT_MOVE_STEPS[index];
        break;

    case DT_STEP_GROUP_EXTRUDE_SPEED:
        s_extrude_speed_mms = DT_EXTRUDE_SPEEDS[index];
        break;

    default:
        s_move_step_mm = DT_MOVE_STEPS[index];
        break;
    }

    step_group_apply(group);
}


static void create_step_group(
    lv_obj_t *parent,
    int group,
    const int *values,
    size_t count,
    lv_obj_t **boxes,
    const char *suffix
)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 30);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < count; ++i) {
        char label[16];

        snprintf(
            label,
            sizeof(label),
            "%d %s",
            values[i],
            suffix
        );

        boxes[i] = lv_checkbox_create(row);
        lv_checkbox_set_text(boxes[i], label);

        lv_obj_set_style_text_color(
            boxes[i],
            color(DT_COLOR_TEXT),
            0
        );

        /*
         * CLICKED, not VALUE_CHANGED: step_group_apply() sets the state
         * programmatically and VALUE_CHANGED would feed back into here.
         */
        lv_obj_add_event_cb(
            boxes[i],
            step_select_event,
            LV_EVENT_CLICKED,
            (void *)(uintptr_t)(((unsigned)group << 8) | (unsigned)i)
        );
    }

    step_group_apply(group);
}


static void jog_event(lv_event_t *event)
{
    const uintptr_t packed =
        (uintptr_t)lv_event_get_user_data(event);

    dispatch_move_request(
        (dt_ui_move_axis_t)(packed >> 1),
        (packed & 1U) ? s_move_step_mm : -s_move_step_mm,
        0
    );
}


/*
 * DT_TOAST
 *
 * One-shot hide. The timer is created paused and simply reset on each new
 * message, so a burst of commands keeps the panel up rather than flickering.
 */
static void toast_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_ui.toast_panel != NULL) {
        lv_obj_add_flag(
            s_ui.toast_panel,
            LV_OBJ_FLAG_HIDDEN
        );
    }

    if (s_ui.toast_timer != NULL) {
        lv_timer_pause(s_ui.toast_timer);
    }
}


/*
 * DT_PRINTER_LIST
 *
 * Switching instances restarts the device rather than rebinding in place.
 * load_moonraker_config() runs once at runtime init and the capability
 * discovery behind it is not re-entrant, so a restart is the only way to land
 * in a known-good state today. It also matches dc_source_set(), which already
 * defers a source change to the next boot.
 */
static void dispatch_printer_request(
    dt_ui_printer_request_t request,
    int index,
    const char *host,
    uint16_t port,
    const char *api_key
)
{
    if (s_printer_request_handler != NULL) {
        s_printer_request_handler(
            request,
            index,
            host,
            port,
            api_key,
            s_printer_request_ctx
        );
    }
}


static void render_printer_model(void)
{
    const dt_ui_printer_model_t *model =
        &s_ui.printer_model;

    for (size_t i = 0; i < DT_UI_PRINTER_MAX; ++i) {
        lv_obj_t *button =
            s_ui.printer_entry_buttons[i];

        lv_obj_t *row = s_ui.printer_rows[i];

        if (button == NULL || row == NULL) {
            continue;
        }

        if (i >= model->count) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *label =
            lv_obj_get_child(button, 0);

        if (label != NULL) {
            /* The live entry is marked rather than disabled -- re-selecting
             * it is a legitimate way to force a clean restart. */
            lv_label_set_text_fmt(
                label,
                "%s%s",
                model->entries[i].host,
                model->entries[i].active ? "   (active)" : ""
            );
        }

        lv_obj_set_style_bg_color(
            button,
            color(
                model->entries[i].active
                    ? DT_COLOR_ACCENT
                    : DT_COLOR_SURFACE_2
            ),
            0
        );
    }

    if (s_ui.printer_add_button != NULL) {
        set_button_enabled(
            s_ui.printer_add_button,
            !model->full
        );
    }
}


static void printer_close_event(lv_event_t *event)
{
    (void)event;

    lv_obj_add_flag(
        s_ui.printer_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void printer_open_event(lv_event_t *event)
{
    (void)event;

    if (s_ui.printer_scrim == NULL) {
        return;
    }

    render_printer_model();

    lv_obj_remove_flag(
        s_ui.printer_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(s_ui.printer_scrim);
}


static void host_close_event(lv_event_t *event)
{
    (void)event;

    lv_obj_add_flag(
        s_ui.host_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void host_open_event(lv_event_t *event)
{
    (void)event;

    if (s_ui.host_scrim == NULL) {
        return;
    }

    lv_textarea_set_text(s_ui.host_textarea, "");
    lv_textarea_set_text(s_ui.port_textarea, "");
    lv_textarea_set_text(s_ui.api_key_textarea, "");

    lv_obj_add_flag(
        s_ui.printer_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_remove_flag(
        s_ui.host_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(s_ui.host_scrim);
}


static void host_confirm_event(lv_event_t *event)
{
    (void)event;

    const char *host =
        lv_textarea_get_text(s_ui.host_textarea);

    const char *port_text =
        lv_textarea_get_text(s_ui.port_textarea);

    const char *api_key =
        lv_textarea_get_text(s_ui.api_key_textarea);

    if (host != NULL && host[0] != '\0') {
        /* Blank port means "the default", which dt_printers_add() fills in. */
        int port =
            (port_text != NULL && port_text[0] != '\0')
                ? atoi(port_text)
                : 0;

        if (port < 0 || port > 65535) {
            port = 0;
        }

        dispatch_printer_request(
            DT_UI_PRINTER_REQUEST_ADD,
            -1,
            host,
            (uint16_t)port,
            api_key != NULL ? api_key : ""
        );
    }

    lv_obj_add_flag(
        s_ui.host_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


/*
 * DT_PRINTER_LIST
 *
 * The keyboard follows whichever field was last tapped, and switches to
 * digits for the port so a hostname's letters cannot land in it.
 *
 * Bound to CLICKED, not FOCUSED: lv_textarea only reacts to focus events,
 * it does not focus itself on touch, and there is no input group in this
 * UI to generate them.
 */
static void host_field_focus_event(lv_event_t *event)
{
    lv_obj_t *target =
        lv_event_get_target_obj(event);

    if (s_ui.host_keyboard == NULL || target == NULL) {
        return;
    }

    lv_keyboard_set_mode(
        s_ui.host_keyboard,
        target == s_ui.port_textarea
            ? LV_KEYBOARD_MODE_NUMBER
            : LV_KEYBOARD_MODE_TEXT_LOWER
    );

    lv_keyboard_set_textarea(s_ui.host_keyboard, target);
}


static void dispatch_temperature_request(
    dt_ui_heater_t heater,
    int celsius
)
{
    if (s_temperature_request_handler != NULL) {
        s_temperature_request_handler(
            heater,
            celsius,
            s_temperature_request_ctx
        );
    }
}


static int keypad_limit(dt_ui_heater_t heater)
{
    return heater == DT_UI_HEATER_BED
        ? DT_PROFILE_BED_MAX_C
        : DT_PROFILE_NOZZLE_MAX_C;
}


static void keypad_refresh_value(void)
{
    if (s_ui.keypad_value == NULL) {
        return;
    }

    lv_label_set_text_fmt(
        s_ui.keypad_value,
        "%s °C",
        s_keypad_digits[0] != '\0'
            ? s_keypad_digits
            : "0"
    );
}


static void keypad_close_event(lv_event_t *event)
{
    (void)event;

    lv_obj_add_flag(
        s_ui.keypad_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void keypad_digit_event(lv_event_t *event)
{
    const char digit =
        (char)(uintptr_t)lv_event_get_user_data(event);

    const size_t length =
        strlen(s_keypad_digits);

    if (length + 1 >= sizeof(s_keypad_digits)) {
        return;
    }

    /* No leading zeros -- "0" alone comes from Cooldown or an empty entry. */
    if (length == 0 && digit == '0') {
        return;
    }

    s_keypad_digits[length] = digit;
    s_keypad_digits[length + 1] = '\0';

    keypad_refresh_value();
}


static void keypad_clear_event(lv_event_t *event)
{
    (void)event;

    s_keypad_digits[0] = '\0';
    keypad_refresh_value();
}


static void keypad_delete_event(lv_event_t *event)
{
    (void)event;

    const size_t length =
        strlen(s_keypad_digits);

    if (length > 0) {
        s_keypad_digits[length - 1] = '\0';
    }

    keypad_refresh_value();
}


static void keypad_confirm_event(lv_event_t *event)
{
    (void)event;

    int value = atoi(s_keypad_digits);
    const int limit = keypad_limit(s_keypad_heater);

    if (value < 0) {
        value = 0;
    }

    if (value > limit) {
        value = limit;
    }

    dispatch_temperature_request(
        s_keypad_heater,
        value
    );

    lv_obj_add_flag(
        s_ui.keypad_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void keypad_open_event(lv_event_t *event)
{
    s_keypad_heater =
        (dt_ui_heater_t)(uintptr_t)
        lv_event_get_user_data(event);

    s_keypad_digits[0] = '\0';

    if (s_ui.keypad_title != NULL) {
        lv_label_set_text_fmt(
            s_ui.keypad_title,
            "%s target  (max %d °C)",
            s_keypad_heater == DT_UI_HEATER_BED
                ? "Bed"
                : "Nozzle",
            keypad_limit(s_keypad_heater)
        );
    }

    keypad_refresh_value();

    lv_obj_remove_flag(
        s_ui.keypad_scrim,
        LV_OBJ_FLAG_HIDDEN
    );
}


static void cooldown_event(lv_event_t *event)
{
    dispatch_temperature_request(
        (dt_ui_heater_t)(uintptr_t)
        lv_event_get_user_data(event),
        0
    );
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

    if (s_pending_printer_index >= 0) {
        /* DT_PRINTER_LIST */
        dispatch_printer_request(
            s_pending_printer_request,
            s_pending_printer_index,
            NULL,
            0,
            NULL
        );

        s_pending_printer_index = -1;
    } else if (s_pending_move_request_valid) {
        /* DT_MOVE_STEP */
        dispatch_move_request(
            s_pending_move_axis,
            s_pending_move_delta,
            s_pending_move_speed
        );

        s_pending_move_request_valid = false;
    } else if (s_pending_filament_request_valid) {
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
    s_pending_move_request_valid = false;
    s_pending_printer_index = -1;

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


/*
 * DT_MOVE_STEP
 *
 * Same dialog, but the pending request carries a value rather than a bare
 * action, so the body can name the distance and speed actually selected.
 */
static void show_move_confirmation(
    dt_ui_move_axis_t axis,
    int delta_mm,
    int speed_mms,
    const char *title,
    const char *body,
    const char *confirm_label
)
{
    s_pending_move_request_valid = true;
    s_pending_filament_request_valid = false;
    s_pending_printer_index = -1;
    s_pending_move_axis = axis;
    s_pending_move_delta = delta_mm;
    s_pending_move_speed = speed_mms;

    lv_label_set_text(s_ui.dialog_title, title);
    lv_label_set_text(s_ui.dialog_body, body);
    lv_label_set_text(s_ui.dialog_confirm_label, confirm_label);

    lv_obj_set_style_text_color(
        s_ui.dialog_confirm_label,
        color(DT_COLOR_TEXT),
        0
    );

    set_button_enabled(
        s_ui.dialog_confirm,
        s_move_request_handler != NULL
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
 * DT_PRINTER_LIST
 *
 * Routed through the same dialog as every other printer-owned command, but
 * the pending slot holds a list index rather than an action.
 */
static void printer_select_event(lv_event_t *event)
{
    const int index =
        (int)(intptr_t)lv_event_get_user_data(event);

    if (
        index < 0 ||
        (size_t)index >= s_ui.printer_model.count
    ) {
        return;
    }

    lv_obj_add_flag(
        s_ui.printer_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    snprintf(
        s_printer_confirm_body,
        sizeof(s_printer_confirm_body),
        "Switch to %s? Any print in progress is unaffected -- this only "
        "changes which printer DragonTouch is watching.",
        s_ui.printer_model.entries[index].host
    );

    s_pending_filament_request_valid = false;
    s_pending_move_request_valid = false;
    s_pending_printer_index = index;
    s_pending_printer_request = DT_UI_PRINTER_REQUEST_SELECT;

    lv_label_set_text(s_ui.dialog_title, "Switch printer?");
    lv_label_set_text(s_ui.dialog_body, s_printer_confirm_body);
    lv_label_set_text(s_ui.dialog_confirm_label, "Switch");

    lv_obj_set_style_text_color(
        s_ui.dialog_confirm_label,
        color(DT_COLOR_TEXT),
        0
    );

    set_button_enabled(
        s_ui.dialog_confirm,
        s_printer_request_handler != NULL
    );

    lv_obj_remove_flag(
        s_ui.dialog_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(s_ui.dialog_scrim);
}


static void printer_delete_event(lv_event_t *event)
{
    const int index =
        (int)(intptr_t)lv_event_get_user_data(event);

    if (
        index < 0 ||
        (size_t)index >= s_ui.printer_model.count
    ) {
        return;
    }

    lv_obj_add_flag(
        s_ui.printer_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    snprintf(
        s_printer_confirm_body,
        sizeof(s_printer_confirm_body),
        "Forget %s? This only removes it from this list -- if it is the "
        "printer currently in use, the connection is unaffected.",
        s_ui.printer_model.entries[index].host
    );

    s_pending_filament_request_valid = false;
    s_pending_move_request_valid = false;
    s_pending_printer_index = index;
    s_pending_printer_request = DT_UI_PRINTER_REQUEST_REMOVE;

    lv_label_set_text(s_ui.dialog_title, "Forget printer?");
    lv_label_set_text(s_ui.dialog_body, s_printer_confirm_body);
    lv_label_set_text(s_ui.dialog_confirm_label, "Forget");

    lv_obj_set_style_text_color(
        s_ui.dialog_confirm_label,
        color(DT_COLOR_TEXT),
        0
    );

    set_button_enabled(
        s_ui.dialog_confirm,
        s_printer_request_handler != NULL
    );

    lv_obj_remove_flag(
        s_ui.dialog_scrim,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(s_ui.dialog_scrim);
}


static void extrude_event(lv_event_t *event)
{
    const bool forward =
        (uintptr_t)lv_event_get_user_data(event) != 0U;

    snprintf(
        s_extrude_confirm_body,
        sizeof(s_extrude_confirm_body),
        "The printer will %s %d mm of filament at %d mm/s. "
        "The hotend must be at temperature.",
        forward ? "extrude" : "retract",
        s_extrude_step_mm,
        s_extrude_speed_mms
    );

    show_move_confirmation(
        DT_UI_MOVE_AXIS_E,
        forward ? s_extrude_step_mm : -s_extrude_step_mm,
        s_extrude_speed_mms,
        forward ? "Extrude filament?" : "Retract filament?",
        s_extrude_confirm_body,
        forward ? "Extrude" : "Retract"
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

/*
 * DT_TEMP_GRID
 *
 * One column of the temperatures tile: sensor name above its reading, both
 * centered. Three of these side by side give the two-row, three-column
 * layout.
 */
static void create_temp_cell(
    lv_obj_t *parent,
    const char *name,
    lv_obj_t **value
)
{
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_height(cell, LV_PCT(100));
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_layout(cell, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);

    lv_obj_set_flex_align(
        cell,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );

    lv_obj_set_style_pad_row(cell, 4, 0);

    lv_obj_t *label =
        make_label(cell, name, DT_COLOR_MUTED);

    lv_obj_set_style_text_align(
        label,
        LV_TEXT_ALIGN_CENTER,
        0
    );

    *value = make_label(cell, "--", DT_COLOR_TEXT);

    lv_obj_set_style_text_align(
        *value,
        LV_TEXT_ALIGN_CENTER,
        0
    );
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
    lv_obj_remove_flag(job, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * DT_JOB_TWO_COLUMN
     *
     * Job details on the left, elapsed/remaining stacked on the right.
     * Splitting the old single time line off into its own column is what
     * lets the card fit its slot without scrolling. The action row stays
     * below, spanning both columns.
     */
    lv_obj_t *job_row = lv_obj_create(job);
    lv_obj_remove_style_all(job_row);
    lv_obj_set_width(job_row, LV_PCT(100));
    lv_obj_set_flex_grow(job_row, 1);
    lv_obj_set_layout(job_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(job_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(job_row, 12, 0);
    lv_obj_remove_flag(job_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *job_details = lv_obj_create(job_row);
    lv_obj_remove_style_all(job_details);
    lv_obj_set_height(job_details, LV_PCT(100));
    lv_obj_set_flex_grow(job_details, 1);
    lv_obj_set_layout(job_details, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(job_details, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(job_details, 6, 0);
    lv_obj_remove_flag(job_details, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.job_state = make_label(job_details, "Printer unavailable", DT_COLOR_TEXT);
    lv_obj_set_style_text_font(s_ui.job_state, &lv_font_montserrat_20, 0);
    s_ui.filename = make_label(job_details, "No active file", DT_COLOR_MUTED);
    lv_label_set_long_mode(s_ui.filename, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_ui.filename, LV_PCT(100));
    s_ui.progress = lv_bar_create(job_details);
    lv_obj_set_size(s_ui.progress, LV_PCT(100), 10);
    lv_bar_set_range(s_ui.progress, 0, 100);
    lv_obj_set_style_bg_color(s_ui.progress, color(DT_COLOR_SURFACE_2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.progress, color(DT_COLOR_ACCENT), LV_PART_INDICATOR);
    s_ui.progress_text = make_label(job_details, "0%", DT_COLOR_TEXT);

    lv_obj_t *job_times = lv_obj_create(job_row);
    lv_obj_remove_style_all(job_times);
    lv_obj_set_width(job_times, 150);
    lv_obj_set_height(job_times, LV_PCT(100));
    lv_obj_set_layout(job_times, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(job_times, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(job_times, 6, 0);
    lv_obj_remove_flag(job_times, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.elapsed_text =
        make_label(job_times, "Elapsed --", DT_COLOR_MUTED);

    s_ui.remaining_text =
        make_label(job_times, "Remaining --", DT_COLOR_MUTED);

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

    /*
     * DT_TEMP_GRID
     *
     * Two rows, three columns: names on top, readings beneath. Part fan
     * moved out -- it isn't a temperature, and the fan page still shows
     * it. Chamber ([temperature_sensor chamber]) takes its place.
     */
    lv_obj_t *temperatures =
        make_card(main_column, "TEMPERATURES");

    lv_obj_set_width(temperatures, LV_PCT(100));
    lv_obj_set_height(temperatures, 96);

    lv_obj_t *temp_row = lv_obj_create(temperatures);
    lv_obj_remove_style_all(temp_row);
    lv_obj_set_width(temp_row, LV_PCT(100));
    lv_obj_set_flex_grow(temp_row, 1);
    lv_obj_set_layout(temp_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(temp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(temp_row, 8, 0);

    create_temp_cell(temp_row, "Nozzle", &s_ui.nozzle_text);
    create_temp_cell(temp_row, "Bed", &s_ui.bed_text);
    create_temp_cell(temp_row, "Chamber", &s_ui.chamber_text);

    lv_obj_t *quick = make_card(main_column, "QUICK ACCESS");
    lv_obj_set_size(quick, LV_PCT(100), 92);
    lv_obj_t *quick_row = lv_obj_create(quick);
    lv_obj_remove_style_all(quick_row);
    lv_obj_set_size(quick_row, LV_PCT(100), 42);
    lv_obj_set_layout(quick_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(quick_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(quick_row, 8, 0);
    /*
     * Quick-access row.
     *
     * "Home axes" reuses the same CONFIRM_HOME dialog as the Move panel's
     * Home button rather than dispatching G28 unconfirmed from a second
     * entry point. The two macro buttons are direct triggers with no
     * confirmation -- their names come from dt_printer_profile.h, so a
     * printer that does not define them simply reports Klipper's error.
     * Enabled state tracks model->can_home, mirroring the Move panel.
     * Lights has no backing action yet and stays an inert placeholder.
     */
    s_ui.quick_home_button =
        make_guarded_action(quick_row, "Home axes", &CONFIRM_HOME);
    set_button_enabled(s_ui.quick_home_button, false);

    lv_obj_t *quick_lights_button = make_action(quick_row, "Lights", false);
    set_button_enabled(quick_lights_button, false);

    s_ui.quick_clean_nozzle_button =
        make_direct_action(
            quick_row,
            DT_PROFILE_QUICK1_LABEL,
            DT_UI_ACTION_QUICK_MACRO_1
        );
    set_button_enabled(s_ui.quick_clean_nozzle_button, false);

    s_ui.quick_macro2_button =
        make_direct_action(
            quick_row,
            DT_PROFILE_QUICK2_LABEL,
            DT_UI_ACTION_QUICK_MACRO_2
        );

    set_button_enabled(s_ui.quick_macro2_button, false);

    lv_obj_t *side = lv_obj_create(page);
    lv_obj_remove_style_all(side);
    lv_obj_set_height(side, LV_PCT(100));
    lv_obj_set_flex_grow(side, 2);
    lv_obj_set_layout(side, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 10, 0);

    /*
     * DT_WEBCAM_HOME_PANE
     *
     * The PRINTER card is gone -- the header already shows the address.
     * The side column is now the webcam, sharing the single decoded frame
     * with the dedicated webcam page and scaled to fit this pane.
     */
    lv_obj_t *webcam_card =
        make_card(side, "WEBCAM");

    lv_obj_set_width(webcam_card, LV_PCT(100));
    lv_obj_set_flex_grow(webcam_card, 1);

    lv_obj_t *home_webcam_wrap =
        lv_obj_create(webcam_card);

    lv_obj_remove_style_all(home_webcam_wrap);
    lv_obj_set_width(home_webcam_wrap, LV_PCT(100));
    lv_obj_set_flex_grow(home_webcam_wrap, 1);

    lv_obj_remove_flag(
        home_webcam_wrap,
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_add_flag(
        home_webcam_wrap,
        LV_OBJ_FLAG_CLICKABLE
    );

    lv_obj_add_event_cb(
        home_webcam_wrap,
        direct_action_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)DT_UI_ACTION_WEBCAM_REFRESH
    );

    s_ui.home_webcam_image =
        lv_image_create(home_webcam_wrap);

    lv_image_set_antialias(
        s_ui.home_webcam_image,
        true
    );

    lv_obj_set_size(
        s_ui.home_webcam_image,
        LV_PCT(100),
        LV_PCT(100)
    );

    lv_obj_center(s_ui.home_webcam_image);

    lv_obj_add_flag(
        s_ui.home_webcam_image,
        LV_OBJ_FLAG_HIDDEN
    );
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

    s_ui.files_model.thumbnail_data = NULL;
    s_ui.files_model.thumbnail_size = 0;
    s_ui.files_model.thumbnail_width = 0;
    s_ui.files_model.thumbnail_height = 0;

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

    /*
     * DT_FILE_THUMBNAIL_PREVIEW
     *
     * LVGL LodePNG accepts LV_IMAGE_SRC_VARIABLE and decodes the encoded PNG
     * directly from lv_image_dsc_t::data.
     */
    if (
        s_ui.file_thumbnail_row != NULL &&
        s_ui.file_thumbnail_image != NULL
    ) {
        if (
            !model->selected ||
            model->thumbnail_data == NULL ||
            model->thumbnail_size < 24U ||
            model->thumbnail_width == 0U ||
            model->thumbnail_height == 0U
        ) {
            lv_obj_add_flag(
                s_ui.file_thumbnail_row,
                LV_OBJ_FLAG_HIDDEN
            );

            memset(
                &s_ui.file_thumbnail_dsc,
                0,
                sizeof(s_ui.file_thumbnail_dsc)
            );
        } else {
            memset(
                &s_ui.file_thumbnail_dsc,
                0,
                sizeof(s_ui.file_thumbnail_dsc)
            );

            s_ui.file_thumbnail_dsc.header.magic =
                LV_IMAGE_HEADER_MAGIC;

            /*
             * Encoded PNG bytes loaded at runtime are intentionally RAW.
             * LodePNG recognizes PNG magic in the data buffer.
             */
            s_ui.file_thumbnail_dsc.header.cf =
                LV_COLOR_FORMAT_RAW;

            s_ui.file_thumbnail_dsc.header.flags =
                0;

            s_ui.file_thumbnail_dsc.header.w =
                0;

            s_ui.file_thumbnail_dsc.header.h =
                0;

            s_ui.file_thumbnail_dsc.header.stride =
                0;

            s_ui.file_thumbnail_dsc.data_size =
                model->thumbnail_size;

            s_ui.file_thumbnail_dsc.data =
                model->thumbnail_data;

            lv_image_header_t decoded_header = {0};

            lv_result_t info_result =
                lv_image_decoder_get_info(
                    &s_ui.file_thumbnail_dsc,
                    &decoded_header
                );

            if (info_result == LV_RESULT_OK) {
                lv_image_cache_drop(&s_ui.file_thumbnail_dsc);

                lv_image_set_src(
                    s_ui.file_thumbnail_image,
                    &s_ui.file_thumbnail_dsc
                );

                /*
                 * Scale whatever Orca embedded into our fixed 150x150 preview.
                 * For the current 48x48 source this is 800 / 256 = 3.125x.
                 */
                uint32_t scale =
                    decoded_header.w > 0
                        ? (
                            150U * 256U +
                            decoded_header.w / 2U
                        ) /
                            decoded_header.w
                        : 256U;

                if (scale == 0U) {
                    scale = 1U;
                }

                lv_image_set_scale(
                    s_ui.file_thumbnail_image,
                    scale
                );

                lv_obj_set_size(
                    s_ui.file_thumbnail_image,
                    150,
                    150
                );

                lv_obj_remove_flag(
                    s_ui.file_thumbnail_row,
                    LV_OBJ_FLAG_HIDDEN
                );

                lv_obj_invalidate(
                    s_ui.file_thumbnail_image
                );
            } else {
                ESP_LOGE(
                    TAG,
                    "FILE_THUMBNAIL PNG decoder rejected in-memory source"
                );

                lv_obj_add_flag(
                    s_ui.file_thumbnail_row,
                    LV_OBJ_FLAG_HIDDEN
                );
            }
        }
    }

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


static const char *aux_fan_kind_text(
    dt_ui_fan_kind_t kind
)
{
    switch (kind) {
    case DT_UI_FAN_KIND_GENERIC:
        return "Manual fan";
    case DT_UI_FAN_KIND_CONTROLLER:
        return "Automatic controller fan";
    case DT_UI_FAN_KIND_HEATER:
        return "Automatic heater fan";
    case DT_UI_FAN_KIND_TEMPERATURE:
        return "Automatic temperature fan";
    default:
        return "Fan";
    }
}


static void aux_fan_slider_event(
    lv_event_t *event
)
{
    const size_t row =
        (size_t)(uintptr_t)
        lv_event_get_user_data(event);

    if (
        row >= s_ui.aux_manual_count ||
        row >= 4 ||
        s_ui.aux_manual_sliders[row] == NULL
    ) {
        return;
    }

    const lv_event_code_t code =
        lv_event_get_code(event);

    const int32_t raw =
        lv_slider_get_value(
            s_ui.aux_manual_sliders[row]
        );

    const uint8_t percent =
        (uint8_t)(
            raw < 0 ? 0 :
            raw > 100 ? 100 :
            raw
        );

    if (code == LV_EVENT_PRESSED) {
        s_ui.aux_manual_dragging[row] = true;
    }

    if (
        code == LV_EVENT_PRESSED ||
        code == LV_EVENT_VALUE_CHANGED ||
        code == LV_EVENT_RELEASED
    ) {
        char label[96] = {0};

        snprintf(
            label,
            sizeof(label),
            "%s  %u%%",
            s_ui.aux_manual_names[row][0] != '\0'
                ? s_ui.aux_manual_names[row]
                : "Fan",
            (unsigned)percent
        );

        lv_label_set_text(
            s_ui.aux_manual_labels[row],
            label
        );
    }

    if (code != LV_EVENT_RELEASED) {
        return;
    }

    s_ui.aux_manual_dragging[row] = false;

    const size_t slot =
        s_ui.aux_manual_slots[row];

    if (slot >= DT_UI_AUX_FAN_MAX) {
        return;
    }

    dispatch_action(
        (dt_ui_action_t)(
            DT_UI_ACTION_AUX_FAN_BASE +
            slot *
                DT_UI_AUX_FAN_LEVEL_COUNT +
            percent
        )
    );
}





static void render_aux_fans(
    const dt_ui_model_t *model
)
{
    /*
     * DT_DYNAMIC_AUX_FANS_DIRECT_ROWS
     *
     * Manual control is exposed only when:
     *   - kind == fan_generic,
     *   - the object is marked controllable,
     *   - live speed telemetry is available,
     *   - the printer is online.
     */
    if (
        model == NULL ||
        s_ui.aux_fan_cards[0] == NULL ||
        s_ui.aux_fan_status[0] == NULL
    ) {
        return;
    }

    const bool online =
        model->connection ==
        DT_UI_CONNECTION_ONLINE;

    char summary[640] = {0};
    size_t used = 0;

    for (
        size_t i = 0;
        i < model->aux_fan_count &&
            i < DT_UI_AUX_FAN_MAX;
        ++i
    ) {
        const dt_ui_aux_fan_t *fan =
            &model->aux_fans[i];

        char line[128] = {0};

        if (!online) {
            snprintf(
                line,
                sizeof(line),
                "%s - offline",
                fan->name != NULL
                    ? fan->name
                    : "Fan"
            );
        } else if (fan->speed_known) {
            snprintf(
                line,
                sizeof(line),
                "%s - %u%% - %s",
                fan->name != NULL
                    ? fan->name
                    : "Fan",
                (unsigned)fan->percent,
                aux_fan_kind_text(fan->kind)
            );
        } else {
            snprintf(
                line,
                sizeof(line),
                "%s - speed unavailable - %s",
                fan->name != NULL
                    ? fan->name
                    : "Fan",
                aux_fan_kind_text(fan->kind)
            );
        }

        int written =
            snprintf(
                summary + used,
                sizeof(summary) - used,
                "%s%s",
                used == 0 ? "" : "\n",
                line
            );

        if (
            written < 0 ||
            (size_t)written >=
                sizeof(summary) - used
        ) {
            break;
        }

        used += (size_t)written;
    }

    if (model->aux_fan_count == 0) {
        snprintf(
            summary,
            sizeof(summary),
            "%s",
            online
                ? "No auxiliary Klipper fan objects discovered."
                : "Printer offline"
        );
    }

    lv_label_set_text(
        s_ui.aux_fan_status[0],
        summary
    );

    size_t manual_count = 0;

    if (online) {
        for (
            size_t slot = 0;
            slot < model->aux_fan_count &&
                slot < DT_UI_AUX_FAN_MAX &&
                manual_count < 4;
            ++slot
        ) {
            const dt_ui_aux_fan_t *fan =
                &model->aux_fans[slot];

            const bool live_manual =
                fan->kind ==
                    DT_UI_FAN_KIND_GENERIC &&
                fan->controllable &&
                fan->speed_known;

            if (!live_manual) {
                continue;
            }

            s_ui.aux_manual_slots[manual_count] =
                slot;

            snprintf(
                s_ui.aux_manual_names[manual_count],
                sizeof(s_ui.aux_manual_names[manual_count]),
                "%s",
                fan->name != NULL
                    ? fan->name
                    : "Fan"
            );

            if (
                s_ui.aux_manual_sliders[manual_count] != NULL &&
                !s_ui.aux_manual_dragging[manual_count]
            ) {
                lv_slider_set_value(
                    s_ui.aux_manual_sliders[manual_count],
                    fan->percent,
                    LV_ANIM_OFF
                );
            }

            char label[96] = {0};

            snprintf(
                label,
                sizeof(label),
                "%s  %u%%",
                fan->name != NULL
                    ? fan->name
                    : "Fan",
                (unsigned)fan->percent
            );

            lv_label_set_text(
                s_ui.aux_manual_labels[manual_count],
                label
            );

            lv_obj_remove_flag(
                s_ui.aux_manual_rows[manual_count],
                LV_OBJ_FLAG_HIDDEN
            );

            if (
                s_ui.aux_manual_sliders[manual_count] != NULL
            ) {
                lv_obj_remove_state(
                    s_ui.aux_manual_sliders[manual_count],
                    LV_STATE_DISABLED
                );
            }

            ++manual_count;
        }
    }

    s_ui.aux_manual_count =
        manual_count;

    for (
        size_t row = manual_count;
        row < 4;
        ++row
    ) {
        if (s_ui.aux_manual_rows[row] != NULL) {
            lv_obj_add_flag(
                s_ui.aux_manual_rows[row],
                LV_OBJ_FLAG_HIDDEN
            );
        }

        s_ui.aux_manual_slots[row] =
            DT_UI_AUX_FAN_MAX;

        s_ui.aux_manual_names[row][0] = '\0';
        s_ui.aux_manual_dragging[row] = false;

        if (s_ui.aux_manual_sliders[row] != NULL) {
            lv_obj_add_state(
                s_ui.aux_manual_sliders[row],
                LV_STATE_DISABLED
            );
        }
    }
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

    lv_obj_t *home_row = lv_obj_create(motion);
    lv_obj_remove_style_all(home_row);
    lv_obj_set_size(home_row, LV_PCT(100), 42);
    lv_obj_set_layout(home_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(home_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(home_row, 4, 0);

    s_ui.home_button =
        make_guarded_action(
            home_row,
            "Home all axes",
            &CONFIRM_HOME
        );

    /* DT_Z_TILT */
    s_ui.z_tilt_button =
        make_guarded_action(
            home_row,
            DT_PROFILE_LEVEL_LABEL,
            &CONFIRM_Z_TILT
        );

    lv_obj_t *jog =
        make_control_card(
            s_ui.control_panels[0],
            "JOG",
            "Step size applies to X, Y and Z."
        );

    /* DT_MOVE_STEP */
    create_step_group(
        jog,
        DT_STEP_GROUP_MOVE,
        DT_MOVE_STEPS,
        DT_MOVE_STEP_COUNT,
        s_ui.move_step_boxes,
        "mm"
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

    /*
     * DT_MOVE_STEP
     *
     * The fixed-distance JOG actions can't carry the selected step, so
     * these go through the move-request handler instead. user_data packs
     * the axis and the sign: (axis << 1) | positive.
     */
    static const uintptr_t jog_targets[] = {
        (DT_UI_MOVE_AXIS_X << 1) | 0U,
        (DT_UI_MOVE_AXIS_X << 1) | 1U,
        (DT_UI_MOVE_AXIS_Y << 1) | 0U,
        (DT_UI_MOVE_AXIS_Y << 1) | 1U,
        (DT_UI_MOVE_AXIS_Z << 1) | 0U,
        (DT_UI_MOVE_AXIS_Z << 1) | 1U
    };

    for (size_t i = 0; i < 6; ++i) {
        s_ui.jog_buttons[i] =
            make_action(
                jog_row,
                jog_names[i],
                false
            );

        lv_obj_add_event_cb(
            s_ui.jog_buttons[i],
            jog_event,
            LV_EVENT_CLICKED,
            (void *)jog_targets[i]
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

    lv_obj_t *nozzle_row =
        lv_obj_create(nozzle);

    lv_obj_remove_style_all(nozzle_row);
    lv_obj_set_size(nozzle_row, LV_PCT(100), 42);
    lv_obj_set_layout(nozzle_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(nozzle_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nozzle_row, 4, 0);

    s_ui.heat_button =
        make_guarded_action(
            nozzle_row,
            "220 °C",
            &CONFIRM_HEAT
        );

    /* DT_TEMP_ENTRY */
    s_ui.nozzle_set_button =
        make_action(nozzle_row, "Set...", false);

    lv_obj_add_event_cb(
        s_ui.nozzle_set_button,
        keypad_open_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)DT_UI_HEATER_NOZZLE
    );

    s_ui.nozzle_cooldown_button =
        make_action(nozzle_row, "Cooldown", false);

    lv_obj_add_event_cb(
        s_ui.nozzle_cooldown_button,
        cooldown_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)DT_UI_HEATER_NOZZLE
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
        "60 °C",
        "110 °C"
    };

    static const dt_ui_action_t bed_actions[] = {
        DT_UI_ACTION_BED_60,
        DT_UI_ACTION_BED_110
    };

    for (size_t i = 0; i < 2; ++i) {
        s_ui.bed_buttons[i] =
            make_direct_action(
                bed_row,
                bed_names[i],
                bed_actions[i]
            );
    }

    /*
     * DT_TEMP_ENTRY
     *
     * The old "Off" preset is now Cooldown, routed through the same
     * numeric path as the keypad (target 0) so both heaters turn off by
     * exactly one mechanism.
     */
    s_ui.bed_set_button =
        make_action(bed_row, "Set...", false);

    lv_obj_add_event_cb(
        s_ui.bed_set_button,
        keypad_open_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)DT_UI_HEATER_BED
    );

    s_ui.bed_cooldown_button =
        make_action(bed_row, "Cooldown", false);

    lv_obj_add_event_cb(
        s_ui.bed_cooldown_button,
        cooldown_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)DT_UI_HEATER_BED
    );

    /*
     * EXTRUSION
     */
    lv_obj_t *extrude =
        make_control_card(
            s_ui.control_panels[2],
            "ACTIVE EXTRUDER",
            "Distance and speed apply to both directions."
        );

    /* DT_MOVE_STEP */
    create_step_group(
        extrude,
        DT_STEP_GROUP_EXTRUDE_STEP,
        DT_MOVE_STEPS,
        DT_MOVE_STEP_COUNT,
        s_ui.extrude_step_boxes,
        "mm"
    );

    create_step_group(
        extrude,
        DT_STEP_GROUP_EXTRUDE_SPEED,
        DT_EXTRUDE_SPEEDS,
        DT_EXTRUDE_SPEED_COUNT,
        s_ui.extrude_speed_boxes,
        "mm/s"
    );

    lv_obj_t *extrude_row = lv_obj_create(extrude);
    lv_obj_remove_style_all(extrude_row);
    lv_obj_set_size(extrude_row, LV_PCT(100), 42);
    lv_obj_set_layout(extrude_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(extrude_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(extrude_row, 4, 0);

    /*
     * Still confirmed before sending, as the fixed 10 mm buttons were --
     * the dialog body is filled in at tap time with the selected values.
     */
    s_ui.extrude_button =
        make_action(extrude_row, "Extrude", false);

    lv_obj_add_event_cb(
        s_ui.extrude_button,
        extrude_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)1U
    );

    s_ui.retract_button =
        make_action(extrude_row, "Retract", false);

    lv_obj_add_event_cb(
        s_ui.retract_button,
        extrude_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)0U
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

    /*
     * DT_DYNAMIC_AUX_FANS_COMPACT
     *
     * One shallow content card avoids the crash-prone nested fan UI tree.
     */
    lv_obj_t *aux_fans =
        make_control_card(
            s_ui.control_panels[3],
            "AUXILIARY FANS",
            "Waiting for fan telemetry..."
        );

    s_ui.aux_fan_cards[0] =
        aux_fans;

    s_ui.aux_fan_title[0] =
        lv_obj_get_child(
            aux_fans,
            0
        );

    s_ui.aux_fan_status[0] =
        lv_obj_get_child(
            aux_fans,
            1
        );

    /*
     * DT_AUX_FAN_SLIDER_SPACING_FIX
     * Keep telemetry and manual rows visually separated.
     */
    lv_obj_set_style_pad_row(
        aux_fans,
        10,
        0
    );

    lv_label_set_long_mode(
        s_ui.aux_fan_status[0],
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.aux_fan_status[0],
        LV_PCT(100)
    );


    /*
     * DT_DYNAMIC_AUX_FANS_DIRECT_ROWS
     *
     * One auxiliary card, up to four shallow manual fan rows.
     */
    for (
        size_t row_index = 0;
        row_index < 4;
        ++row_index
    ) {
        lv_obj_t *row =
            lv_obj_create(aux_fans);

        s_ui.aux_manual_rows[row_index] =
            row;

        lv_obj_remove_style_all(row);

        /* DT_AUX_FAN_SLIDER_SPACING_FIX */
        lv_obj_set_size(
            row,
            LV_PCT(100),
            60
        );

        lv_obj_set_flex_align(
            row,
            LV_FLEX_ALIGN_START,
            LV_FLEX_ALIGN_CENTER,
            LV_FLEX_ALIGN_CENTER
        );

        lv_obj_set_layout(
            row,
            LV_LAYOUT_FLEX
        );

        lv_obj_set_flex_flow(
            row,
            LV_FLEX_FLOW_ROW
        );

        lv_obj_set_style_pad_column(
            row,
            4,
            0
        );

        lv_obj_t *label =
            make_label(
                row,
                "--",
                DT_COLOR_TEXT
            );

        s_ui.aux_manual_labels[row_index] =
            label;

        lv_obj_set_width(
            label,
            150
        );

        lv_label_set_long_mode(
            label,
            LV_LABEL_LONG_MODE_DOTS
        );

        /*
         * DT_DYNAMIC_AUX_FAN_SLIDERS
         *
         * Live label updates while dragging; command is sent on release.
         */
        lv_obj_t *slider =
            lv_slider_create(row);

        s_ui.aux_manual_sliders[row_index] =
            slider;

        lv_slider_set_range(
            slider,
            0,
            100
        );

        lv_slider_set_value(
            slider,
            0,
            LV_ANIM_OFF
        );

        lv_obj_set_height(
            slider,
            34
        );

        /*
         * Give the knob clearance from the row clip boundary and keep rows
         * visually separated even with a large touch target.
         */
        lv_obj_set_style_margin_top(
            slider,
            6,
            0
        );

        lv_obj_set_style_margin_bottom(
            slider,
            6,
            0
        );

        lv_obj_set_flex_grow(
            slider,
            1
        );

        lv_obj_add_event_cb(
            slider,
            aux_fan_slider_event,
            LV_EVENT_ALL,
            (void *)(uintptr_t)row_index
        );

        s_ui.aux_manual_slots[row_index] =
            DT_UI_AUX_FAN_MAX;

        lv_obj_add_flag(
            row,
            LV_OBJ_FLAG_HIDDEN
        );
    }

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
                    /*
                     * DT_AFC_UNLOAD_LOADED_LANE: a lane already loaded into
                     * the tool has nothing left for "Load" to do, so the
                     * same button switches to a real Unload action instead.
                     */
                    : (lane->tool_loaded ? "Unload" : "Load")
            );
        }

        /*
         * DT_AFC_LOAD_STATE_AWARE / DT_AFC_UNLOAD_LOADED_LANE
         * A lane that's already the active tool load has nothing left for
         * "Load" (BT_CHANGE_TOOL) to do -- that path is only offered to
         * lanes that aren't currently loaded. The already-loaded lane gets
         * "Unload" (TOOL_UNLOAD) instead, gated on its own capability flag.
         */
        const bool can_change =
            !recovery_mode &&
            model->afc_actions_enabled &&
            model->has_bt_change_tool &&
            lane->prep &&
            !lane->tool_loaded;

        const bool can_unload =
            !recovery_mode &&
            model->afc_actions_enabled &&
            model->has_afc_tool_unload &&
            lane->tool_loaded;

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
                : (lane->tool_loaded ? can_unload : can_change)
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
    s_pending_move_request_valid = false;
    s_pending_printer_index = -1;
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

    /*
     * DT_AFC_UNLOAD_LOADED_LANE
     * This lane is already the active tool load -- there's nothing for
     * "Load" to do, so the same button instead offers to unload it via
     * TOOL_UNLOAD (which always targets whatever's actually in the
     * toolhead; runtime double-checks this lane is still the one AFC
     * reports as tool_loaded before sending it).
     */
    if (lane->tool_loaded) {
        snprintf(
            body,
            sizeof(body),
            "Run TOOL_UNLOAD?\n"
            "AFC will unload %s from the toolhead back into its lane.",
            lane->name
        );

        show_afc_confirmation(
            DT_UI_FILAMENT_REQUEST_UNLOAD_LANE,
            lane_number,
            "Unload AFC lane?",
            body,
            "Unload"
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

    /* DT_KLIPPER_ERROR */
    case DT_UI_CONNECTION_ERROR:
        return "Klipper error";

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


/*
 * DT_WEBCAM_FULL_DECODE
 *
 * Decode the encoded snapshot into one complete RGB888 frame.
 *
 * TJpgDec is a partial decoder: lv_image_decoder_open() produces no pixel
 * buffer, and LVGL pulls the picture out one MCU block at a time during
 * every draw pass -- re-decoding the JPEG on every redraw, and giving no
 * single buffer to work from.
 *
 * Running the block loop once here assembles the whole frame, which is what
 * webcam_show_frame() then box-averages down to each pane's size. It is also
 * why scaling could never be left to LVGL: lv_draw's partial path transforms
 * each decoded block about its own area rather than about the whole image,
 * so a scale factor over a tiled source renders garbage.
 */
static bool webcam_decode_to_rgb(
    uint32_t *width_out,
    uint32_t *height_out
)
{
    *width_out = 0;
    *height_out = 0;

    lv_image_decoder_dsc_t dsc = {0};

    if (
        lv_image_decoder_open(
            &dsc,
            &s_ui.webcam_dsc,
            NULL
        ) != LV_RESULT_OK
    ) {
        return false;
    }

    const uint32_t width = dsc.header.w;
    const uint32_t height = dsc.header.h;

    if (width == 0 || height == 0) {
        lv_image_decoder_close(&dsc);
        return false;
    }

    const size_t stride = (size_t)width * 3U;
    const size_t needed = stride * (size_t)height;

    if (s_ui.webcam_rgb_capacity < needed) {
        free(s_ui.webcam_rgb_data);

        s_ui.webcam_rgb_data =
            heap_caps_malloc(
                needed,
                MALLOC_CAP_SPIRAM |
                MALLOC_CAP_8BIT
            );

        s_ui.webcam_rgb_capacity =
            s_ui.webcam_rgb_data != NULL
                ? needed
                : 0;
    }

    if (s_ui.webcam_rgb_data == NULL) {
        ESP_LOGW(
            TAG,
            "webcam: no PSRAM for a %ux%u frame",
            (unsigned)width,
            (unsigned)height
        );

        lv_image_decoder_close(&dsc);
        return false;
    }

    const lv_area_t full_area = {
        .x1 = 0,
        .y1 = 0,
        .x2 = (int32_t)width - 1,
        .y2 = (int32_t)height - 1
    };

    /*
     * LV_COORD_MIN in y1 is the decoder's "this is the first block"
     * signal -- it seeds its MCU walk from that sentinel.
     */
    lv_area_t block = {
        .x1 = LV_COORD_MIN,
        .y1 = LV_COORD_MIN,
        .x2 = LV_COORD_MIN,
        .y2 = LV_COORD_MIN
    };

    uint32_t rows_filled = 0;

    while (
        lv_image_decoder_get_area(
            &dsc,
            &full_area,
            &block
        ) == LV_RESULT_OK
    ) {
        const lv_draw_buf_t *decoded = dsc.decoded;

        if (decoded == NULL || decoded->data == NULL) {
            break;
        }

        const int32_t block_w = lv_area_get_width(&block);
        const int32_t block_h = lv_area_get_height(&block);

        if (
            block.x1 < 0 ||
            block.y1 < 0 ||
            block_w <= 0 ||
            block_h <= 0 ||
            (uint32_t)(block.x1 + block_w) > width ||
            (uint32_t)(block.y1 + block_h) > height
        ) {
            continue;
        }

        for (int32_t row = 0; row < block_h; ++row) {
            memcpy(
                s_ui.webcam_rgb_data +
                    ((size_t)(block.y1 + row) * stride) +
                    ((size_t)block.x1 * 3U),
                decoded->data +
                    ((size_t)row * (size_t)decoded->header.stride),
                (size_t)block_w * 3U
            );
        }

        rows_filled =
            (uint32_t)(block.y1 + block_h);
    }

    lv_image_decoder_close(&dsc);

    if (rows_filled == 0) {
        return false;
    }

    if (rows_filled < height) {
        ESP_LOGW(
            TAG,
            "webcam: partial decode, %u of %u rows",
            (unsigned)rows_filled,
            (unsigned)height
        );
    }

    *width_out = width;
    *height_out = height;

    return true;
}


/*
 * DT_WEBCAM_HOME_PANE
 *
 * Point one image widget at the decoded frame. Both the webcam page and
 * the home pane draw the same buffer; each fills its own container and
 * lets CONTAIN letterbox the frame into it, so the aspect ratio is kept,
 * the whole frame is always visible, and neither ever scrolls.
 */
static void webcam_hide_frame(void)
{
    if (s_ui.webcam_image != NULL) {
        lv_obj_add_flag(
            s_ui.webcam_image,
            LV_OBJ_FLAG_HIDDEN
        );
    }

    if (s_ui.home_webcam_image != NULL) {
        lv_obj_add_flag(
            s_ui.home_webcam_image,
            LV_OBJ_FLAG_HIDDEN
        );
    }
}


/*
 * DT_WEBCAM_PRESCALE
 *
 * Box-average downscale. Always a reduction here (640x480 into panes of at
 * most ~520 wide), and averaging the source rectangle behind each destination
 * pixel is both better looking than nearest-neighbour and cheap -- it touches
 * each source pixel once.
 *
 * Channel order is irrelevant: each of the three bytes is averaged
 * independently and written back in the position it came from.
 */
static void webcam_scale_rgb888(
    const uint8_t *src,
    uint32_t src_w,
    uint32_t src_h,
    uint8_t *dst,
    uint32_t dst_w,
    uint32_t dst_h
)
{
    for (uint32_t y = 0; y < dst_h; ++y) {
        uint32_t sy0 = (uint32_t)(((uint64_t)y * src_h) / dst_h);
        uint32_t sy1 = (uint32_t)(((uint64_t)(y + 1) * src_h) / dst_h);

        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }

        if (sy1 > src_h) {
            sy1 = src_h;
        }

        for (uint32_t x = 0; x < dst_w; ++x) {
            uint32_t sx0 = (uint32_t)(((uint64_t)x * src_w) / dst_w);
            uint32_t sx1 = (uint32_t)(((uint64_t)(x + 1) * src_w) / dst_w);

            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }

            if (sx1 > src_w) {
                sx1 = src_w;
            }

            uint32_t a = 0;
            uint32_t b = 0;
            uint32_t d = 0;
            uint32_t n = 0;

            for (uint32_t sy = sy0; sy < sy1; ++sy) {
                const uint8_t *row =
                    src +
                    ((size_t)sy * src_w + sx0) * 3U;

                for (uint32_t sx = sx0; sx < sx1; ++sx) {
                    a += row[0];
                    b += row[1];
                    d += row[2];
                    row += 3;
                    ++n;
                }
            }

            uint8_t *out =
                dst + ((size_t)y * dst_w + x) * 3U;

            out[0] = (uint8_t)(a / n);
            out[1] = (uint8_t)(b / n);
            out[2] = (uint8_t)(d / n);
        }
    }
}


static void webcam_show_frame(
    lv_obj_t *image,
    dt_webcam_view_t *view
)
{
    if (image == NULL || s_ui.webcam_rgb_data == NULL) {
        return;
    }

    const uint32_t src_w = s_ui.webcam_rgb_dsc.header.w;
    const uint32_t src_h = s_ui.webcam_rgb_dsc.header.h;

    if (src_w == 0 || src_h == 0) {
        return;
    }

    lv_obj_t *pane = lv_obj_get_parent(image);

    if (pane == NULL) {
        return;
    }

    /* Sizes are only meaningful once layout has run. */
    lv_obj_update_layout(pane);

    const int32_t box_w = lv_obj_get_content_width(pane);
    const int32_t box_h = lv_obj_get_content_height(pane);

    if (box_w <= 0 || box_h <= 0) {
        return;
    }

    /* Fit inside the pane, aspect preserved, integer maths throughout. */
    uint32_t dst_w = (uint32_t)box_w;
    uint32_t dst_h = (dst_w * src_h) / src_w;

    if (dst_h > (uint32_t)box_h) {
        dst_h = (uint32_t)box_h;
        dst_w = (dst_h * src_w) / src_h;
    }

    if (dst_w == 0 || dst_h == 0) {
        return;
    }

    const bool stale =
        view->data == NULL ||
        view->width != dst_w ||
        view->height != dst_h ||
        view->revision != s_ui.webcam_decoded_revision;

    if (stale) {
        const size_t needed =
            (size_t)dst_w * (size_t)dst_h * 3U;

        if (view->capacity < needed) {
            free(view->data);

            view->data =
                heap_caps_malloc(
                    needed,
                    MALLOC_CAP_SPIRAM |
                    MALLOC_CAP_8BIT
                );

            view->capacity =
                view->data != NULL ? needed : 0;
        }

        if (view->data == NULL) {
            return;
        }

        webcam_scale_rgb888(
            s_ui.webcam_rgb_data,
            src_w,
            src_h,
            view->data,
            dst_w,
            dst_h
        );

        view->width = dst_w;
        view->height = dst_h;
        view->revision = s_ui.webcam_decoded_revision;

        memset(&view->dsc, 0, sizeof(view->dsc));
        view->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        view->dsc.header.cf = LV_COLOR_FORMAT_RGB888;
        view->dsc.header.w = dst_w;
        view->dsc.header.h = dst_h;
        view->dsc.header.stride = dst_w * 3U;
        view->dsc.data = view->data;
        view->dsc.data_size = (uint32_t)needed;

        /* Fixed address reused per frame -- same cache hazard as elsewhere. */
        lv_image_cache_drop(&view->dsc);
    }

    lv_image_set_src(image, &view->dsc);

    /*
     * Exactly the bitmap's size and no inner alignment, so LVGL blits
     * without a transform. Centring is the pane's job now.
     */
    lv_obj_set_size(
        image,
        (int32_t)dst_w,
        (int32_t)dst_h
    );

    lv_image_set_inner_align(
        image,
        LV_IMAGE_ALIGN_DEFAULT
    );

    lv_obj_center(image);

    lv_obj_remove_flag(
        image,
        LV_OBJ_FLAG_HIDDEN
    );
}


/*
 * DT_WEBCAM_SNAPSHOT
 *
 * Renders whatever s_ui.webcam_model currently holds: a status line above
 * the viewport, plus the decoded frame scaled to fit inside it.
 */
static void render_webcam_model(void)
{
    const dt_ui_webcam_model_t *model =
        &s_ui.webcam_model;

    if (s_ui.webcam_status_text != NULL) {
        /*
         * Never assert "no webcam configured" as a default -- that is a
         * claim about Moonraker we have not checked yet on a freshly
         * opened page. dt_runtime.c puts the real finding in status_text
         * once discovery has actually run, and that wins here.
         */
        const char *text = "TAP TO REFRESH";

        if (model->status_text[0] != '\0') {
            text = model->status_text;
        } else if (model->loading) {
            text = "Loading snapshot...";
        }

        lv_label_set_text(
            s_ui.webcam_status_text,
            text
        );
    }

    /*
      * DT_WEBCAM_HOME_PANE
      *
      * Either view is reason enough to decode. Gating this on the webcam
      * page's widgets meant that whenever only the home pane existed --
      * the state at boot, and any time the webcam page had been recycled
      * -- this returned before decoding and the home pane stayed empty.
      */
    if (
        s_ui.webcam_image == NULL &&
        s_ui.home_webcam_image == NULL
    ) {
        return;
    }

    if (
        !model->has_image ||
        model->jpeg_data == NULL ||
        model->jpeg_size == 0
    ) {
        webcam_hide_frame();

        memset(
            &s_ui.webcam_dsc,
            0,
            sizeof(s_ui.webcam_dsc)
        );

        return;
    }

    /*
     * DT_WEBCAM_REVISION
     *
     * Every push re-entered this path, so a refresh decoded twice: once for
     * the loading=true push (re-decoding the PREVIOUS frame for nothing) and
     * again for the new one. Each decode holds the LVGL lock long enough to
     * starve the other model pushes. Decode only when the bytes actually
     * changed; the show_frame calls below are cheap and still run every
     * time, so a rebuilt page still gets its source set.
     */
    if (
        s_ui.webcam_rgb_data != NULL &&
        model->revision != 0 &&
        model->revision == s_ui.webcam_decoded_revision
    ) {
        webcam_show_frame(
            s_ui.webcam_image,
            &s_ui.webcam_view_page
        );

        webcam_show_frame(
            s_ui.home_webcam_image,
            &s_ui.webcam_view_home
        );

        return;
    }

    memset(
        &s_ui.webcam_dsc,
        0,
        sizeof(s_ui.webcam_dsc)
    );

    s_ui.webcam_dsc.header.magic =
        LV_IMAGE_HEADER_MAGIC;

    /*
     * The encoded bytes are intentionally RAW -- TJpgDec sniffs the JPEG
     * signature out of the buffer. dt_runtime.c splices in a JFIF APP0
     * segment beforehand so that sniff actually succeeds.
     */
    s_ui.webcam_dsc.header.cf =
        LV_COLOR_FORMAT_RAW;

    s_ui.webcam_dsc.data_size =
        model->jpeg_size;

    s_ui.webcam_dsc.data =
        model->jpeg_data;

    /*
     * The image cache is keyed on the source pointer, and &s_ui.webcam_dsc
     * is a fixed address reused for every frame -- so drop the previous
     * snapshot's entry before decoding, or lv_image_decoder_open() will
     * short-circuit on it and hand back a stale, zeroed header.
     */
    lv_image_cache_drop(&s_ui.webcam_dsc);

    uint32_t width = 0;
    uint32_t height = 0;

    const bool decoded =
        webcam_decode_to_rgb(
            &width,
            &height
        );

    if (!decoded) {
        webcam_hide_frame();

        if (s_ui.webcam_status_text != NULL) {
            lv_label_set_text(
                s_ui.webcam_status_text,
                "Snapshot decode failed. Tap to retry."
            );
        }

        return;
    }

    memset(
        &s_ui.webcam_rgb_dsc,
        0,
        sizeof(s_ui.webcam_rgb_dsc)
    );

    s_ui.webcam_rgb_dsc.header.magic =
        LV_IMAGE_HEADER_MAGIC;

    s_ui.webcam_rgb_dsc.header.cf =
        LV_COLOR_FORMAT_RGB888;

    s_ui.webcam_rgb_dsc.header.w = width;
    s_ui.webcam_rgb_dsc.header.h = height;

    s_ui.webcam_rgb_dsc.header.stride =
        width * 3U;

    s_ui.webcam_rgb_dsc.data =
        s_ui.webcam_rgb_data;

    s_ui.webcam_rgb_dsc.data_size =
        width * height * 3U;

    s_ui.webcam_decoded_revision = model->revision;

    /* Same fixed-address cache hazard as above, for the decoded frame. */
    lv_image_cache_drop(&s_ui.webcam_rgb_dsc);

    webcam_show_frame(
        s_ui.webcam_image,
        &s_ui.webcam_view_page
    );

    webcam_show_frame(
        s_ui.home_webcam_image,
        &s_ui.webcam_view_home
    );
}


static void create_webcam_page(lv_obj_t *page)
{
    /*
     * DT_WEBCAM_BARE_PAGE
     *
     * No page heading and no control card -- the page is the snapshot,
     * with a single tap prompt above it. The viewport takes all the
     * remaining height and the frame is scaled to fit inside it, so
     * there is never anything to scroll.
     */
    lv_obj_set_layout(page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 7, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.webcam_status_text =
        make_label(
            page,
            "TAP TO REFRESH",
            DT_COLOR_MUTED
        );

    lv_label_set_long_mode(
        s_ui.webcam_status_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    lv_obj_set_width(
        s_ui.webcam_status_text,
        LV_PCT(100)
    );

    lv_obj_set_style_text_align(
        s_ui.webcam_status_text,
        LV_TEXT_ALIGN_CENTER,
        0
    );

    s_ui.webcam_image_wrap =
        lv_obj_create(page);

    lv_obj_remove_style_all(
        s_ui.webcam_image_wrap
    );

    lv_obj_set_width(
        s_ui.webcam_image_wrap,
        LV_PCT(100)
    );

    lv_obj_set_flex_grow(
        s_ui.webcam_image_wrap,
        1
    );

    lv_obj_set_style_bg_color(
        s_ui.webcam_image_wrap,
        color(DT_COLOR_SURFACE),
        0
    );

    lv_obj_set_style_bg_opa(
        s_ui.webcam_image_wrap,
        LV_OPA_COVER,
        0
    );

    lv_obj_set_style_border_color(
        s_ui.webcam_image_wrap,
        color(DT_COLOR_BORDER),
        0
    );

    lv_obj_set_style_border_width(
        s_ui.webcam_image_wrap,
        1,
        0
    );

    lv_obj_set_style_radius(
        s_ui.webcam_image_wrap,
        8,
        0
    );

    lv_obj_remove_flag(
        s_ui.webcam_image_wrap,
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_add_flag(
        s_ui.webcam_image_wrap,
        LV_OBJ_FLAG_CLICKABLE
    );

    lv_obj_add_event_cb(
        s_ui.webcam_image_wrap,
        direct_action_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)DT_UI_ACTION_WEBCAM_REFRESH
    );

    s_ui.webcam_image =
        lv_image_create(
            s_ui.webcam_image_wrap
        );

    lv_image_set_antialias(
        s_ui.webcam_image,
        true
    );

    lv_obj_set_size(
        s_ui.webcam_image,
        LV_PCT(100),
        LV_PCT(100)
    );

    lv_obj_center(
        s_ui.webcam_image
    );

    lv_obj_add_flag(
        s_ui.webcam_image,
        LV_OBJ_FLAG_HIDDEN
    );

    memset(
        &s_ui.webcam_dsc,
        0,
        sizeof(s_ui.webcam_dsc)
    );

    memset(
        &s_ui.webcam_rgb_dsc,
        0,
        sizeof(s_ui.webcam_rgb_dsc)
    );

    render_webcam_model();
}


static void create_filament_page(lv_obj_t *page)
{
    /*
     * DT_FILAMENT_FULL_PANE
     *
     * No page heading and no status strapline -- the filament system card
     * is the whole page. create_page_heading() was what established the
     * page's flex layout, so that setup moves here.
     */
    lv_obj_set_layout(page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 7, 0);

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

    /*
     * DT_FILE_THUMBNAIL_PREVIEW
     * Details scrolls vertically to accommodate a 300x300 selected preview.
     */
    lv_obj_set_width(
        s_ui.file_panels[1],
        LV_PCT(100)
    );

    lv_obj_add_flag(
        s_ui.file_panels[1],
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_scroll_dir(
        s_ui.file_panels[1],
        LV_DIR_VER
    );

    lv_obj_set_scrollbar_mode(
        s_ui.file_panels[1],
        LV_SCROLLBAR_MODE_AUTO
    );

    /* DT_FILE_THUMBNAIL_DISPLAY_FIX_V1 */
    lv_obj_set_width(
        details,
        LV_PCT(100)
    );

    lv_obj_set_height(
        details,
        LV_SIZE_CONTENT
    );

    lv_obj_set_flex_grow(
        details,
        0
    );

    /*
     * DT_FILE_DETAILS_TWO_COLUMN
     *
     * Metadata text on the left, thumbnail on the right, as a plain flex
     * row -- there's no bordered "table" widget in this codebase, and an
     * unstyled flex row gives the two-column look with no lines drawn.
     * The text column is content-sized (see DT_FILE_DETAILS_AUTO_WIDTH_TEXT
     * below) so it's only ever as wide as its longest line, and the text
     * keeps its own LV_LABEL_LONG_MODE_WRAP as a safety net rather than
     * spanning (and visually splitting across) the full card width like
     * it did before.
     */
    lv_obj_t *detail_row = lv_obj_create(details);
    lv_obj_remove_style_all(detail_row);
    lv_obj_set_width(detail_row, LV_PCT(100));
    lv_obj_set_height(detail_row, LV_SIZE_CONTENT);
    lv_obj_set_layout(detail_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(detail_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(detail_row, 12, 0);
    lv_obj_set_flex_align(
        detail_row,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );

    lv_obj_set_parent(s_ui.file_detail_text, detail_row);

    lv_label_set_long_mode(
        s_ui.file_detail_text,
        LV_LABEL_LONG_MODE_WRAP
    );

    /*
     * DT_FILE_DETAILS_AUTO_WIDTH_TEXT
     *
     * Content-sized, not a fixed or growing width: the box sizes itself
     * to whichever line the metadata renders widest, so ordinary lines
     * never wrap and the box never carries unused blank space either.
     * LV_LABEL_LONG_MODE_WRAP stays set purely as a safety net for one
     * pathologically long unbroken line -- it won't fire for normal
     * metadata since the box already fits every real line.
     */
    lv_obj_set_width(s_ui.file_detail_text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_ui.file_detail_text, 0);

    s_ui.file_thumbnail_row =
        lv_obj_create(detail_row);

    lv_obj_remove_style_all(
        s_ui.file_thumbnail_row
    );

    /*
     * DT_FILE_DETAILS_AUTO_WIDTH_TEXT
     *
     * This column now grows to claim whatever width the auto-sized text
     * column doesn't need, and centers the fixed 150px image inside
     * itself -- so the image gets equal left/right padding rather than
     * sitting flush against either the text or the card's edge.
     */
    lv_obj_set_width(s_ui.file_thumbnail_row, 0);
    lv_obj_set_height(s_ui.file_thumbnail_row, 154);
    lv_obj_set_flex_grow(s_ui.file_thumbnail_row, 1);

    lv_obj_set_layout(
        s_ui.file_thumbnail_row,
        LV_LAYOUT_FLEX
    );

    lv_obj_set_flex_flow(
        s_ui.file_thumbnail_row,
        LV_FLEX_FLOW_ROW
    );

    lv_obj_set_flex_align(
        s_ui.file_thumbnail_row,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );

    s_ui.file_thumbnail_image =
        lv_image_create(
            s_ui.file_thumbnail_row
        );

    /*
     * DT_FILE_THUMBNAIL_150_PREVIEW
     * Fixed display box. Source can be smaller; LVGL scales it after decode.
     */
    lv_obj_set_size(
        s_ui.file_thumbnail_image,
        150,
        150
    );

    lv_image_set_antialias(
        s_ui.file_thumbnail_image,
        true
    );

    lv_obj_add_flag(
        s_ui.file_thumbnail_row,
        LV_OBJ_FLAG_HIDDEN
    );

    memset(
        &s_ui.file_thumbnail_dsc,
        0,
        sizeof(s_ui.file_thumbnail_dsc)
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



static void clear_recycled_page_refs(
    dt_ui_page_t page
)
{
    if (page == DT_UI_PAGE_HOME) {
        s_ui.quick_home_button = NULL;
        s_ui.quick_clean_nozzle_button = NULL;
        s_ui.quick_macro2_button = NULL;
        return;
    }

    if (page != DT_UI_PAGE_CONTROL) {
        return;
    }

    s_ui.home_button = NULL;

    /* DT_MOVE_STEP / DT_Z_TILT */
    s_ui.z_tilt_button = NULL;

    for (size_t i = 0; i < DT_MOVE_STEP_COUNT; ++i) {
        s_ui.move_step_boxes[i] = NULL;
        s_ui.extrude_step_boxes[i] = NULL;
    }

    for (size_t i = 0; i < DT_EXTRUDE_SPEED_COUNT; ++i) {
        s_ui.extrude_speed_boxes[i] = NULL;
    }
    s_ui.heat_button = NULL;

    /* DT_TEMP_ENTRY (the keypad itself lives on the screen, not a page) */
    s_ui.nozzle_set_button = NULL;
    s_ui.nozzle_cooldown_button = NULL;
    s_ui.bed_set_button = NULL;
    s_ui.bed_cooldown_button = NULL;
    s_ui.extrude_button = NULL;
    s_ui.retract_button = NULL;

    s_ui.axes_text = NULL;
    s_ui.nozzle_control_text = NULL;
    s_ui.bed_control_text = NULL;
    s_ui.fan_control_text = NULL;

    s_ui.aux_manual_count = 0;

    for (
        size_t row = 0;
        row < 4;
        ++row
    ) {
        s_ui.aux_manual_rows[row] = NULL;
        s_ui.aux_manual_labels[row] = NULL;
        s_ui.aux_manual_slots[row] =
            DT_UI_AUX_FAN_MAX;

        s_ui.aux_manual_sliders[row] = NULL;
        s_ui.aux_manual_names[row][0] = '\0';
        s_ui.aux_manual_dragging[row] = false;
    }

    for (
        size_t i = 0;
        i < DT_UI_AUX_FAN_MAX;
        ++i
    ) {
        s_ui.aux_fan_cards[i] = NULL;
        s_ui.aux_fan_title[i] = NULL;
        s_ui.aux_fan_status[i] = NULL;
        s_ui.aux_fan_rows[i] = NULL;

        for (
            size_t preset = 0;
            preset <
                DT_UI_AUX_FAN_PRESET_COUNT;
            ++preset
        ) {
            s_ui.aux_fan_buttons[i][preset] =
                NULL;
        }
    }

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
            s_ui.file_thumbnail_row = NULL;
            s_ui.file_thumbnail_image = NULL;

            memset(
                &s_ui.file_thumbnail_dsc,
                0,
                sizeof(s_ui.file_thumbnail_dsc)
            );

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

        if (page == DT_UI_PAGE_WEBCAM) {
            /* DT_WEBCAM_SNAPSHOT */
            s_ui.webcam_status_text = NULL;
            s_ui.webcam_image_wrap = NULL;
            s_ui.webcam_image = NULL;

            /*
             * DT_WEBCAM_HOME_PANE
             *
             * The decoded frame is deliberately NOT freed here. Home is
             * resident and its webcam pane draws this same buffer, so
             * releasing it with the webcam page would leave that pane
             * pointing at freed memory.
             *
             * DT_WEBCAM_PRESCALE
             *
             * This page's pre-scaled copy IS freed: its widget has just been
             * destroyed, nothing else draws it, and at pane size it is the
             * larger of the two. It is rebuilt on the next visit.
             */
            lv_image_cache_drop(&s_ui.webcam_view_page.dsc);

            free(s_ui.webcam_view_page.data);

            memset(
                &s_ui.webcam_view_page,
                0,
                sizeof(s_ui.webcam_view_page)
            );

            memset(
                &s_ui.webcam_dsc,
                0,
                sizeof(s_ui.webcam_dsc)
            );
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

    case DT_UI_PAGE_WEBCAM:
        create_webcam_page(
            s_ui.pages[DT_UI_PAGE_WEBCAM]
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
    /*
     * DT_TOAST
     *
     * Bottom-right, floating over the page area. Not interactive -- it must
     * never swallow a tap meant for whatever is underneath it.
     */
    s_ui.toast_panel = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.toast_panel);
    lv_obj_add_flag(s_ui.toast_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(s_ui.toast_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_ui.toast_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_ui.toast_panel, 380, 46);
    lv_obj_align(s_ui.toast_panel, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    lv_obj_set_style_bg_color(s_ui.toast_panel, color(DT_COLOR_SURFACE_2), 0);
    lv_obj_set_style_bg_opa(s_ui.toast_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_ui.toast_panel, 8, 0);
    lv_obj_set_style_pad_hor(s_ui.toast_panel, 12, 0);
    lv_obj_set_style_border_width(s_ui.toast_panel, 3, 0);
    lv_obj_set_style_border_side(s_ui.toast_panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(s_ui.toast_panel, color(DT_COLOR_MUTED), 0);
    lv_obj_add_flag(s_ui.toast_panel, LV_OBJ_FLAG_HIDDEN);

    s_ui.toast_label =
        make_label(s_ui.toast_panel, "", DT_COLOR_TEXT);

    lv_label_set_long_mode(s_ui.toast_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_ui.toast_label, LV_PCT(100));
    lv_obj_align(s_ui.toast_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_ui.toast_timer =
        lv_timer_create(toast_timer_cb, 3000, NULL);

    lv_timer_pause(s_ui.toast_timer);

    /*
     * DT_PRINTER_LIST
     *
     * Picker for the saved Moonraker instances, opened from the header
     * hostname. Lives on the screen, so page recycling never touches it.
     */
    s_ui.printer_scrim = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.printer_scrim);
    lv_obj_add_flag(s_ui.printer_scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_ui.printer_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.printer_scrim, color(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ui.printer_scrim, LV_OPA_70, 0);
    lv_obj_add_flag(s_ui.printer_scrim, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *printer_panel = lv_obj_create(s_ui.printer_scrim);
    style_surface(printer_panel);
    lv_obj_set_size(printer_panel, 460, 400);
    lv_obj_center(printer_panel);
    lv_obj_set_layout(printer_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(printer_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(printer_panel, 18, 0);
    lv_obj_set_style_pad_row(printer_panel, 8, 0);

    lv_obj_t *printer_title =
        make_label(printer_panel, "Printer", DT_COLOR_TEXT);

    lv_obj_set_style_text_font(
        printer_title,
        &lv_font_montserrat_20,
        0
    );

    make_label(
        printer_panel,
        "Switching reconnects without restarting.",
        DT_COLOR_MUTED
    );

    s_ui.printer_list = lv_obj_create(printer_panel);
    lv_obj_remove_style_all(s_ui.printer_list);
    lv_obj_set_width(s_ui.printer_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.printer_list, 1);
    lv_obj_set_layout(s_ui.printer_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_ui.printer_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.printer_list, 6, 0);
    lv_obj_set_scroll_dir(s_ui.printer_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.printer_list, LV_SCROLLBAR_MODE_AUTO);

    /*
     * All rows are built now and shown or hidden as the model changes --
     * rebuilding the list on every update would churn LVGL objects for no
     * benefit at this size.
     */
    for (size_t i = 0; i < DT_UI_PRINTER_MAX; ++i) {
        s_ui.printer_rows[i] = lv_obj_create(s_ui.printer_list);
        lv_obj_remove_style_all(s_ui.printer_rows[i]);
        lv_obj_set_width(s_ui.printer_rows[i], LV_PCT(100));
        lv_obj_set_height(s_ui.printer_rows[i], 40);
        lv_obj_set_layout(s_ui.printer_rows[i], LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(s_ui.printer_rows[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(s_ui.printer_rows[i], 6, 0);
        lv_obj_remove_flag(s_ui.printer_rows[i], LV_OBJ_FLAG_SCROLLABLE);

        s_ui.printer_entry_buttons[i] =
            make_action(s_ui.printer_rows[i], "", false);

        lv_obj_set_flex_grow(s_ui.printer_entry_buttons[i], 1);
        lv_obj_set_height(s_ui.printer_entry_buttons[i], 40);

        lv_obj_add_event_cb(
            s_ui.printer_entry_buttons[i],
            printer_select_event,
            LV_EVENT_CLICKED,
            (void *)(intptr_t)i
        );

        s_ui.printer_delete_buttons[i] =
            make_action(s_ui.printer_rows[i], "Forget", false);

        lv_obj_set_flex_grow(s_ui.printer_delete_buttons[i], 0);
        lv_obj_set_width(s_ui.printer_delete_buttons[i], 88);
        lv_obj_set_height(s_ui.printer_delete_buttons[i], 40);
        style_destructive_action(s_ui.printer_delete_buttons[i]);

        lv_obj_add_event_cb(
            s_ui.printer_delete_buttons[i],
            printer_delete_event,
            LV_EVENT_CLICKED,
            (void *)(intptr_t)i
        );

        lv_obj_add_flag(
            s_ui.printer_rows[i],
            LV_OBJ_FLAG_HIDDEN
        );
    }

    lv_obj_t *printer_actions = lv_obj_create(printer_panel);
    lv_obj_remove_style_all(printer_actions);
    lv_obj_set_size(printer_actions, LV_PCT(100), 42);
    lv_obj_set_layout(printer_actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(printer_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(printer_actions, 8, 0);

    lv_obj_add_event_cb(
        make_action(printer_actions, "Close", false),
        printer_close_event,
        LV_EVENT_CLICKED,
        NULL
    );

    s_ui.printer_add_button =
        make_action(printer_actions, "Add printer", true);

    lv_obj_add_event_cb(
        s_ui.printer_add_button,
        host_open_event,
        LV_EVENT_CLICKED,
        NULL
    );

    /*
     * DT_PRINTER_LIST
     *
     * Hostname entry. A hostname beats an IP here -- it survives DHCP
     * churn -- so this is a text field rather than a numeric pad.
     */
    s_ui.host_scrim = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.host_scrim);
    lv_obj_add_flag(s_ui.host_scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_ui.host_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.host_scrim, color(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ui.host_scrim, LV_OPA_70, 0);
    lv_obj_add_flag(s_ui.host_scrim, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *host_panel = lv_obj_create(s_ui.host_scrim);
    style_surface(host_panel);
    lv_obj_set_size(host_panel, 660, 448);
    lv_obj_center(host_panel);
    lv_obj_set_layout(host_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(host_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(host_panel, 14, 0);
    lv_obj_set_style_pad_row(host_panel, 8, 0);

    make_label(
        host_panel,
        "Hostname or IP  (port defaults to 7125)",
        DT_COLOR_MUTED
    );

    s_ui.host_textarea = lv_textarea_create(host_panel);
    lv_textarea_set_one_line(s_ui.host_textarea, true);
    lv_textarea_set_placeholder_text(
        s_ui.host_textarea,
        "printer.local"
    );
    lv_obj_set_width(s_ui.host_textarea, LV_PCT(100));

    lv_obj_add_event_cb(
        s_ui.host_textarea,
        host_field_focus_event,
        LV_EVENT_CLICKED,
        NULL
    );

    /*
     * Port and key share a row: both are usually left alone, so they get
     * placeholders rather than labels and no vertical space of their own.
     */
    lv_obj_t *host_extra = lv_obj_create(host_panel);
    lv_obj_remove_style_all(host_extra);
    lv_obj_set_width(host_extra, LV_PCT(100));
    lv_obj_set_height(host_extra, 40);
    lv_obj_set_layout(host_extra, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(host_extra, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(host_extra, 8, 0);
    lv_obj_remove_flag(host_extra, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.port_textarea = lv_textarea_create(host_extra);
    lv_textarea_set_one_line(s_ui.port_textarea, true);
    lv_textarea_set_placeholder_text(s_ui.port_textarea, "7125");
    lv_textarea_set_accepted_chars(s_ui.port_textarea, "0123456789");
    lv_textarea_set_max_length(s_ui.port_textarea, 5);
    lv_obj_set_width(s_ui.port_textarea, 130);
    lv_obj_set_flex_grow(s_ui.port_textarea, 0);

    lv_obj_add_event_cb(
        s_ui.port_textarea,
        host_field_focus_event,
        LV_EVENT_CLICKED,
        NULL
    );

    s_ui.api_key_textarea = lv_textarea_create(host_extra);
    lv_textarea_set_one_line(s_ui.api_key_textarea, true);
    lv_textarea_set_placeholder_text(
        s_ui.api_key_textarea,
        "API key (optional)"
    );
    lv_textarea_set_max_length(s_ui.api_key_textarea, 64);
    lv_obj_set_flex_grow(s_ui.api_key_textarea, 1);

    lv_obj_add_event_cb(
        s_ui.api_key_textarea,
        host_field_focus_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_t *host_actions = lv_obj_create(host_panel);
    lv_obj_remove_style_all(host_actions);
    lv_obj_set_size(host_actions, LV_PCT(100), 42);
    lv_obj_set_layout(host_actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(host_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(host_actions, 8, 0);

    lv_obj_add_event_cb(
        make_action(host_actions, "Cancel", false),
        host_close_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        make_action(host_actions, "Save", true),
        host_confirm_event,
        LV_EVENT_CLICKED,
        NULL
    );

    s_ui.host_keyboard = lv_keyboard_create(host_panel);
    lv_obj_set_width(s_ui.host_keyboard, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.host_keyboard, 1);
    lv_keyboard_set_mode(s_ui.host_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_ui.host_keyboard, s_ui.host_textarea);

    /*
     * DT_TEMP_ENTRY
     *
     * A dedicated numeric pad rather than a textarea + lv_keyboard: this
     * only ever takes three digits, and big keys beat a full keyboard on a
     * printer touchscreen. Lives on the screen, not on a page, so it is
     * never touched by page recycling.
     */
    s_ui.keypad_scrim = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(s_ui.keypad_scrim);
    lv_obj_add_flag(s_ui.keypad_scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_ui.keypad_scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.keypad_scrim, color(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ui.keypad_scrim, LV_OPA_70, 0);
    lv_obj_add_flag(s_ui.keypad_scrim, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *keypad = lv_obj_create(s_ui.keypad_scrim);
    style_surface(keypad);
    lv_obj_set_size(keypad, 300, 386);
    lv_obj_center(keypad);
    lv_obj_set_layout(keypad, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(keypad, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(keypad, 16, 0);
    lv_obj_set_style_pad_row(keypad, 10, 0);
    lv_obj_remove_flag(keypad, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.keypad_title =
        make_label(keypad, "Nozzle target", DT_COLOR_MUTED);

    s_ui.keypad_value =
        make_label(keypad, "0 °C", DT_COLOR_TEXT);

    lv_obj_set_style_text_font(s_ui.keypad_value, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_ui.keypad_value, LV_PCT(100));
    lv_obj_set_style_text_align(s_ui.keypad_value, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *pad = lv_obj_create(keypad);
    lv_obj_remove_style_all(pad);
    lv_obj_set_width(pad, LV_PCT(100));
    lv_obj_set_flex_grow(pad, 1);
    lv_obj_set_layout(pad, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(pad, 6, 0);
    lv_obj_set_style_pad_column(pad, 6, 0);
    lv_obj_remove_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

    static const char *keypad_keys[] = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "Clear", "0", "Del"
    };

    for (size_t i = 0; i < 12; ++i) {
        lv_obj_t *key =
            make_action(pad, keypad_keys[i], false);

        /* make_action() grows to fill a row; these are a fixed grid. */
        lv_obj_set_flex_grow(key, 0);
        lv_obj_set_size(key, 80, 46);

        if (i == 9) {
            lv_obj_add_event_cb(
                key, keypad_clear_event, LV_EVENT_CLICKED, NULL);
        } else if (i == 11) {
            lv_obj_add_event_cb(
                key, keypad_delete_event, LV_EVENT_CLICKED, NULL);
        } else {
            lv_obj_add_event_cb(
                key,
                keypad_digit_event,
                LV_EVENT_CLICKED,
                (void *)(uintptr_t)keypad_keys[i][0]
            );
        }
    }

    lv_obj_t *keypad_actions = lv_obj_create(keypad);
    lv_obj_remove_style_all(keypad_actions);
    lv_obj_set_size(keypad_actions, LV_PCT(100), 42);
    lv_obj_set_layout(keypad_actions, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(keypad_actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(keypad_actions, 8, 0);

    lv_obj_add_event_cb(
        make_action(keypad_actions, "Cancel", false),
        keypad_close_event,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_add_event_cb(
        make_action(keypad_actions, "Set", true),
        keypad_confirm_event,
        LV_EVENT_CLICKED,
        NULL
    );

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
        DT_NAV_ICON_WEBCAM,
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

    /*
     * DT_ESTOP
     *
     * Deliberately immediate and unconfirmed -- a stop that first asks
     * "are you sure?" is not an emergency stop. Left enabled regardless
     * of connection state too: a greyed-out e-stop is worse than one that
     * tries and reports a failure.
     */
    s_ui.estop_button =
        make_action(header, "E-STOP", true);

    lv_obj_set_size(s_ui.estop_button, 96, 36);
    lv_obj_align(s_ui.estop_button, LV_ALIGN_LEFT_MID, 152, 0);

    lv_obj_add_event_cb(
        s_ui.estop_button,
        direct_action_event,
        LV_EVENT_CLICKED,
        (void *)(uintptr_t)DT_UI_ACTION_EMERGENCY_STOP
    );

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

    /* DT_PRINTER_LIST: the hostname is the way into the picker. */
    lv_obj_add_flag(s_ui.device_name, LV_OBJ_FLAG_CLICKABLE);

    /* A one-line label is a thin target on a 7" panel -- grow the hit box
     * without disturbing the header's alignment. */
    lv_obj_set_ext_click_area(s_ui.device_name, 14);

    lv_obj_add_event_cb(
        s_ui.device_name,
        printer_open_event,
        LV_EVENT_CLICKED,
        NULL
    );

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

    } else if (
        model->connection ==
        DT_UI_CONNECTION_ERROR
    ) {
        /* DT_KLIPPER_ERROR: halted, not connecting. */
        connection = "Klipper error";
        connection_color =
            DT_COLOR_ACCENT;
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

    /*
     * DT_KLIPPER_ERROR
     *
     * When Klipper is halted there is no job to describe, so the card
     * carries the reason instead -- otherwise the only clue on screen is a
     * header pill, and the actual fault text lives in a log nobody can read
     * from here.
     */
    const bool klipper_halted =
        model->connection == DT_UI_CONNECTION_ERROR;

    lv_label_set_text(
        s_ui.job_state,
        klipper_halted
            ? "Klipper halted"
            : job_state_text(model->job_state)
    );

    lv_obj_set_style_text_color(
        s_ui.job_state,
        color(
            klipper_halted
                ? DT_COLOR_ACCENT
                : (model->job_state == DT_UI_JOB_ERROR
                    ? DT_COLOR_WARNING
                    : DT_COLOR_TEXT)
        ),
        0
    );

    const bool has_status_message =
        model->status_message != NULL &&
        model->status_message[0] != '\0';

    lv_label_set_text(
        s_ui.filename,
        klipper_halted && has_status_message
            ? model->status_message
            : (model->filename != NULL &&
               model->filename[0] != '\0'
                ? model->filename
                : "No active file")
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

    lv_label_set_text_fmt(
        s_ui.elapsed_text,
        "Elapsed  %s",
        elapsed
    );

    lv_label_set_text_fmt(
        s_ui.remaining_text,
        "Remaining  %s",
        remaining
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

        /*
         * DT_CHAMBER_TEMP
         *
         * No target to show for a plain temperature_sensor, so this is the
         * current reading alone rather than the "current / target" form.
         */
        if (isfinite(model->chamber_c)) {
            int tenths =
                (int)(model->chamber_c * 10.0f +
                      (model->chamber_c >= 0.0f ? 0.5f : -0.5f));

            lv_label_set_text_fmt(
                s_ui.chamber_text,
                "%d.%d °C",
                tenths / 10,
                tenths < 0 ? -(tenths % 10) : tenths % 10
            );
        } else {
            lv_label_set_text(
                s_ui.chamber_text,
                "Unavailable"
            );
        }

        /* The part fan reading lives on the fan page now, not on home. */
        if (
            s_ui.fan_control_text != NULL &&
            model->fan_percent <= 100
        ) {
            lv_label_set_text_fmt(
                s_ui.fan_control_text,
                "%u%%",
                model->fan_percent
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
            s_ui.chamber_text,
            "Unavailable"
        );
    }

    if (s_ui.axes_text != NULL) {
        /*
         * DT_AXES_NO_LVGL_FLOAT_FMT
         *
         * LVGL's printf wrapper is not built with reliable floating-point
         * formatting in this firmware. Format with libc instead, then pass
         * the finished string to LVGL.
         */
        char axes[128] = {0};

        snprintf(
            axes,
            sizeof(axes),
            "X %.1f   Y %.1f   Z %.1f mm   Home %c%c%c",
            model->x,
            model->y,
            model->z,
            model->homed_x ? 'X' : '-',
            model->homed_y ? 'Y' : '-',
            model->homed_z ? 'Z' : '-'
        );

        lv_label_set_text(
            s_ui.axes_text,
            axes
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

    /* DT_Z_TILT */
    if (s_ui.z_tilt_button != NULL) {
        set_button_enabled(
            s_ui.z_tilt_button,
            model->can_home
        );
    }

    if (s_ui.quick_home_button != NULL) {
        set_button_enabled(
            s_ui.quick_home_button,
            model->can_home
        );
    }

    if (s_ui.quick_clean_nozzle_button != NULL) {
        set_button_enabled(
            s_ui.quick_clean_nozzle_button,
            model->can_home
        );
    }

    if (s_ui.quick_macro2_button != NULL) {
        set_button_enabled(
            s_ui.quick_macro2_button,
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

    /* DT_TEMP_ENTRY */
    lv_obj_t *const heater_buttons[] = {
        s_ui.nozzle_set_button,
        s_ui.nozzle_cooldown_button,
        s_ui.bed_set_button,
        s_ui.bed_cooldown_button,
    };

    for (
        size_t i = 0;
        i < sizeof(heater_buttons) / sizeof(heater_buttons[0]);
        ++i
    ) {
        if (heater_buttons[i] != NULL) {
            set_button_enabled(
                heater_buttons[i],
                model->can_heat
            );
        }
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

    render_aux_fans(model);

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


esp_err_t dt_ui_update_webcam(
    const dt_ui_webcam_model_t *model
)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ui.webcam_model = *model;

    /*
     * DT_WEBCAM_HOME_PANE
     *
     * No page_built gate here any more. Two pages can host a view of the
     * frame, and gating on the webcam page meant the home pane was never
     * rendered at all -- the decode simply never ran while sitting on
     * home. render_webcam_model() checks for itself whether either view
     * actually exists, so that is the single place the decision is made.
     */
    render_webcam_model();

    return ESP_OK;
}


/* DT_TOAST */
esp_err_t dt_ui_toast(
    dt_ui_toast_kind_t kind,
    const char *text
)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (
        s_ui.toast_panel == NULL ||
        s_ui.toast_label == NULL ||
        text == NULL
    ) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t accent = DT_COLOR_MUTED;
    uint32_t linger_ms = 3000;

    if (kind == DT_UI_TOAST_SUCCESS) {
        accent = DT_COLOR_SUCCESS;
    } else if (kind == DT_UI_TOAST_ERROR) {
        accent = DT_COLOR_ACCENT;

        /* A failure is the one thing worth reading twice. */
        linger_ms = 6000;
    }

    lv_obj_set_style_border_color(
        s_ui.toast_panel,
        color(accent),
        0
    );

    lv_label_set_text(s_ui.toast_label, text);

    lv_obj_remove_flag(
        s_ui.toast_panel,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_obj_move_foreground(s_ui.toast_panel);

    if (s_ui.toast_timer != NULL) {
        lv_timer_set_period(s_ui.toast_timer, linger_ms);
        lv_timer_reset(s_ui.toast_timer);
        lv_timer_resume(s_ui.toast_timer);
    }

    return ESP_OK;
}


/* DT_PRINTER_LIST */
esp_err_t dt_ui_update_printers(
    const dt_ui_printer_model_t *model
)
{
    if (!s_ui.ready) {
        return ESP_ERR_INVALID_STATE;
    }

    if (model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ui.printer_model = *model;

    render_printer_model();

    return ESP_OK;
}


esp_err_t dt_ui_set_printer_request_handler(
    dt_ui_printer_request_handler_t handler,
    void *ctx
)
{
    s_printer_request_handler = handler;
    s_printer_request_ctx = ctx;

    return ESP_OK;
}


/* DT_MOVE_STEP */
esp_err_t dt_ui_set_move_request_handler(
    dt_ui_move_request_handler_t handler,
    void *ctx
)
{
    s_move_request_handler = handler;
    s_move_request_ctx = ctx;

    return ESP_OK;
}


/* DT_TEMP_ENTRY */
esp_err_t dt_ui_set_temperature_request_handler(
    dt_ui_temperature_request_handler_t handler,
    void *ctx
)
{
    s_temperature_request_handler = handler;
    s_temperature_request_ctx = ctx;

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
