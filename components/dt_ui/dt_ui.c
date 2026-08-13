#include "dt_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

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

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *pages[DT_PAGE_COUNT];
    lv_obj_t *nav_buttons[DT_PAGE_COUNT];
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
    bool ready;
} dt_ui_state_t;

static const char *TAG = "dt_ui";
static dt_ui_state_t s_ui;

static lv_color_t color(uint32_t hex)
{
    return lv_color_hex(hex);
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

static void show_page(dt_ui_page_t selected)
{
    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        const bool active = i == selected;
        if (active) {
            lv_obj_remove_flag(s_ui.pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_text_color(s_ui.nav_buttons[i],
                                    color(active ? DT_COLOR_ACCENT : DT_COLOR_MUTED), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_ui.nav_buttons[i], 0),
                                    color(active ? DT_COLOR_ACCENT : DT_COLOR_MUTED), 0);
        lv_obj_set_style_bg_opa(s_ui.nav_buttons[i], active ? LV_OPA_20 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_ui.nav_buttons[i], active ? 3 : 0, 0);
        lv_obj_set_style_border_side(s_ui.nav_buttons[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(s_ui.nav_buttons[i], color(DT_COLOR_ACCENT), 0);
    }
}

static void nav_event(lv_event_t *event)
{
    dt_ui_page_t page = (dt_ui_page_t)(uintptr_t)lv_event_get_user_data(event);
    show_page(page);
}

typedef struct {
    const char *title;
    const char *body;
    const char *confirm_label;
    bool destructive;
} dt_confirmation_t;

static const dt_confirmation_t CONFIRM_CANCEL = {
    "Stop this print?",
    "The printer remains authoritative. Stopping cannot be undone and requires an attached command handler.",
    "Stop print",
    true,
};
static const dt_confirmation_t CONFIRM_HOME = {
    "Home all axes?",
    "The printer will validate its motion state and safety policy before accepting this request.",
    "Request homing",
    false,
};
static const dt_confirmation_t CONFIRM_HEAT = {
    "Set nozzle to 220 °C?",
    "Heating is performed and supervised by the printer. Keep the tool area clear.",
    "Request heating",
    false,
};
static const dt_confirmation_t CONFIRM_EXTRUDE = {
    "Extrude 10 mm?",
    "The printer must confirm a safe nozzle temperature before moving filament.",
    "Request extrusion",
    false,
};

static void close_dialog(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(s_ui.dialog_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void show_confirmation(const dt_confirmation_t *confirmation)
{
    lv_label_set_text(s_ui.dialog_title, confirmation->title);
    lv_label_set_text(s_ui.dialog_body, confirmation->body);
    lv_label_set_text(s_ui.dialog_confirm_label, confirmation->confirm_label);
    lv_obj_set_style_bg_color(s_ui.dialog_confirm,
                              color(confirmation->destructive ? DT_COLOR_WARNING : DT_COLOR_ACCENT), 0);
    lv_obj_set_style_text_color(s_ui.dialog_confirm_label, color(DT_COLOR_TEXT), 0);
    set_button_enabled(s_ui.dialog_confirm, false);
    lv_obj_remove_flag(s_ui.dialog_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_ui.dialog_scrim);
}

static void open_dialog(lv_event_t *event)
{
    show_confirmation(lv_event_get_user_data(event));
}

#ifdef DT_UI_HOST_PREVIEW
void dt_ui_preview_show_confirmation(const char *name)
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

static lv_obj_t *make_guarded_action(lv_obj_t *parent, const char *text,
                                     const dt_confirmation_t *confirmation)
{
    lv_obj_t *button = make_action(parent, text, false);
    lv_obj_add_event_cb(button, open_dialog, LV_EVENT_CLICKED, (void *)confirmation);
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
    s_ui.cancel_button = make_action(actions, "Cancel", false);
    style_destructive_action(s_ui.cancel_button);
    lv_obj_add_event_cb(s_ui.cancel_button, open_dialog, LV_EVENT_CLICKED, (void *)&CONFIRM_CANCEL);
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

static void create_stub_page(lv_obj_t *page, const char *title, const char *description,
                             const char *const *cards, size_t card_count)
{
    lv_obj_set_layout(page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *heading = make_label(page, title, DT_COLOR_TEXT);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
    make_label(page, description, DT_COLOR_MUTED);

    lv_obj_t *grid = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);

    for (size_t i = 0; i < card_count; ++i) {
        lv_obj_t *card = make_card(grid, cards[i]);
        lv_obj_set_size(card, 220, 116);
        lv_obj_set_flex_grow(card, 1);
        make_label(card, "Available after printer pairing", DT_COLOR_MUTED);
        lv_obj_t *button = make_action(card, "Unavailable", false);
        lv_obj_set_width(button, LV_PCT(100));
        set_button_enabled(button, false);
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
    static const char *names[] = {"Motion", "Temperature", "Extrusion", "Fans"};
    lv_obj_t *tabs = create_page_heading(
        page, "Control", "Capability-gated requests; the selected printer owns safety decisions.");
    for (size_t i = 0; i < DT_CONTROL_TAB_COUNT; ++i) {
        s_ui.control_tabs[i] = create_tab(tabs, names[i], control_tab_event, i);
        s_ui.control_panels[i] = create_tab_panel(page);
    }

    lv_obj_t *motion = make_control_card(s_ui.control_panels[0], "AXES", "X 120.0   Y 95.5   Z 12.4 mm");
    lv_obj_t *home = make_guarded_action(motion, "Review home all", &CONFIRM_HOME);
    size_card_action(home);
    lv_obj_t *motion_hint = make_label(
        motion, "Jog controls unavailable: no motion capability handler.", DT_COLOR_MUTED);
    lv_label_set_long_mode(motion_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(motion_hint, LV_PCT(100));
    lv_obj_t *jog = make_control_card(s_ui.control_panels[0], "JOG DISTANCE", "0.1 mm   1 mm   10 mm");
    lv_obj_t *jog_button = make_action(jog, "Jog pad unavailable", false);
    size_card_action(jog_button);
    set_button_enabled(jog_button, false);

    lv_obj_t *nozzle = make_control_card(s_ui.control_panels[1], "NOZZLE", "Current 219.6 °C   Target 220 °C");
    lv_obj_t *heat = make_guarded_action(nozzle, "Review 220 °C", &CONFIRM_HEAT);
    size_card_action(heat);
    lv_obj_t *bed = make_control_card(s_ui.control_panels[1], "BED", "Current 59.8 °C   Target 60 °C");
    lv_obj_t *bed_button = make_action(bed, "Temperature presets unavailable", false);
    size_card_action(bed_button);
    set_button_enabled(bed_button, false);

    lv_obj_t *extrude = make_control_card(s_ui.control_panels[2], "ACTIVE TOOL", "Tool 0   PLA   10 mm request");
    lv_obj_t *extrude_button = make_guarded_action(extrude, "Review extrude 10 mm", &CONFIRM_EXTRUDE);
    size_card_action(extrude_button);
    lv_obj_t *retract = make_control_card(s_ui.control_panels[2], "RETRACT", "Requires an explicit extrusion capability and safe temperature.");
    lv_obj_t *retract_button = make_action(retract, "Unavailable", false);
    size_card_action(retract_button);
    set_button_enabled(retract_button, false);

    lv_obj_t *part_fan = make_control_card(s_ui.control_panels[3], "PART FAN", "78%   Authoritative state from printer");
    lv_obj_t *fan_button = make_action(part_fan, "Fan controls unavailable", false);
    size_card_action(fan_button);
    set_button_enabled(fan_button, false);
    make_control_card(s_ui.control_panels[3], "AUXILIARY FANS", "No declared auxiliary fan capabilities.");
    select_tab(s_ui.control_tabs, s_ui.control_panels, DT_CONTROL_TAB_COUNT, 0);
}

static void create_files_page(lv_obj_t *page)
{
    static const char *names[] = {"Recent", "Printer", "USB"};
    lv_obj_t *tabs = create_page_heading(
        page, "Print files", "Browse and inspect files before requesting a printer-owned start.");
    for (size_t i = 0; i < DT_FILES_TAB_COUNT; ++i) {
        s_ui.file_tabs[i] = create_tab(tabs, names[i], file_tab_event, i);
        s_ui.file_panels[i] = create_tab_panel(page);
    }

    lv_obj_t *recent = make_control_card(s_ui.file_panels[0], "RECENT", "dragon_duct_v7.3mf\nToday | 2h 26m | 38 g");
    lv_obj_t *inspect = make_action(recent, "Inspect file", true);
    size_card_action(inspect);
    lv_obj_t *history = make_control_card(s_ui.file_panels[0], "HISTORY", "calibration_cube.3mf\nCompleted yesterday");
    lv_obj_t *again = make_action(history, "Start unavailable", false);
    size_card_action(again);
    set_button_enabled(again, false);

    lv_obj_t *printer = make_control_card(s_ui.file_panels[1], "PRINTER STORAGE", "Folder navigation will use the selected printer's file capability.");
    lv_obj_t *printer_button = make_action(printer, "Browse unavailable", false);
    size_card_action(printer_button);
    set_button_enabled(printer_button, false);
    make_control_card(s_ui.file_panels[1], "FILE DETAILS", "Select a file to review metadata, preview, and start confirmation.");

    lv_obj_t *usb = make_control_card(s_ui.file_panels[2], "USB STORAGE", "No removable storage detected in the desktop preview.");
    lv_obj_t *usb_button = make_action(usb, "Rescan unavailable", false);
    size_card_action(usb_button);
    set_button_enabled(usb_button, false);
    make_control_card(s_ui.file_panels[2], "IMPORT", "Local import remains outside the scaffold until board storage is known.");
    select_tab(s_ui.file_tabs, s_ui.file_panels, DT_FILES_TAB_COUNT, 0);
}

static void create_pages(lv_obj_t *content)
{
    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        s_ui.pages[i] = make_page(content);
    }

    create_home_page(s_ui.pages[DT_UI_PAGE_HOME]);
    create_control_page(s_ui.pages[DT_UI_PAGE_CONTROL]);
    create_files_page(s_ui.pages[DT_UI_PAGE_FILES]);

    static const char *filament_cards[] = {"ACTIVE TOOL", "MATERIAL SLOTS", "LOAD / UNLOAD"};
    create_stub_page(s_ui.pages[DT_UI_PAGE_FILAMENT], "Filament",
                     "Tool and material controls adapt to the selected printer's capabilities.",
                     filament_cards, 3);

    static const char *device_cards[] = {"SELECTED PRINTER", "DISCOVERED DEVICES", "DRAGON GROUP"};
    create_stub_page(s_ui.pages[DT_UI_PAGE_DEVICES], "Devices",
                     "Discover and explicitly pair same-LAN printers and Dragon-family siblings.",
                     device_cards, 3);

    static const char *settings_cards[] = {"WI-FI", "DISPLAY", "UPDATE & RECOVERY", "ABOUT"};
    create_stub_page(s_ui.pages[DT_UI_PAGE_SETTINGS], "Settings",
                     "Device-local preferences, provisioning, diagnostics, and recovery.",
                     settings_cards, 4);
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
    lv_obj_t *guard = make_label(dialog, "PREVIEW ONLY | NO COMMAND HANDLER", DT_COLOR_WARNING);
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
    set_button_enabled(s_ui.dialog_confirm, false);
    lv_obj_add_flag(s_ui.dialog_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void create_shell(lv_display_t *display)
{
    static const char *nav_names[DT_PAGE_COUNT] = {
        "Home", "Control", "Files", "Filament", "Devices", "Settings"
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
    lv_obj_set_size(rail, 76, LV_PCT(100));
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
        lv_obj_t *button = lv_button_create(rail);
        lv_obj_remove_style_all(button);
        s_ui.nav_buttons[i] = button;
        lv_obj_set_size(button, LV_PCT(100), 50);
        lv_obj_set_style_bg_color(button, color(DT_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_radius(button, 4, 0);
        lv_obj_set_style_text_color(button, color(DT_COLOR_MUTED), 0);
        lv_obj_add_event_cb(button, nav_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *label = make_label(button, nav_names[i], DT_COLOR_MUTED);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_obj_center(label);
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

    lv_label_set_text(s_ui.device_name,
                      model->device_name != NULL ? model->device_name : "No printer");
    const bool has_device = model->device_name != NULL && model->device_name[0] != '\0';
    lv_label_set_text(s_ui.printer_name, has_device ? model->device_name : "No paired printer");
    lv_label_set_text(s_ui.printer_hint,
                      has_device ? "Selected same-LAN printer" : "Pair a same-LAN device to begin.");
    const char *connection = "Offline";
    uint32_t connection_color = DT_COLOR_MUTED;
    if (model->connection == DT_UI_CONNECTION_CONNECTING) {
        connection = "Connecting";
        connection_color = DT_COLOR_WARNING;
    } else if (model->connection == DT_UI_CONNECTION_ONLINE) {
        connection = "Online";
        connection_color = DT_COLOR_SUCCESS;
    }
    lv_label_set_text(s_ui.connection_text, connection);
    lv_obj_set_style_bg_color(s_ui.connection_dot, color(connection_color), 0);

    lv_label_set_text(s_ui.job_state, job_state_text(model->job_state));
    lv_obj_set_style_text_color(s_ui.job_state,
                                color(model->job_state == DT_UI_JOB_ERROR
                                          ? DT_COLOR_WARNING : DT_COLOR_TEXT), 0);
    lv_label_set_text(s_ui.filename,
                      model->filename != NULL && model->filename[0] != '\0'
                          ? model->filename : "No active file");
    uint8_t progress = model->progress_percent > 100 ? 100 : model->progress_percent;
    lv_bar_set_value(s_ui.progress, progress, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ui.progress_text, "%u%%", progress);

    char elapsed[20];
    char remaining[20];
    char time_line[64];
    format_duration(elapsed, sizeof(elapsed), model->elapsed_seconds);
    format_duration(remaining, sizeof(remaining), model->remaining_seconds);
    snprintf(time_line, sizeof(time_line), "Elapsed %s  •  Remaining %s", elapsed, remaining);
    lv_label_set_text(s_ui.time_text, time_line);
    char temperature[32];
    if (model->connection == DT_UI_CONNECTION_ONLINE) {
        format_temperature(temperature, sizeof(temperature),
                           model->nozzle_c, model->nozzle_target_c);
        lv_label_set_text(s_ui.nozzle_text, temperature);
        format_temperature(temperature, sizeof(temperature), model->bed_c, model->bed_target_c);
        lv_label_set_text(s_ui.bed_text, temperature);
        lv_label_set_text_fmt(s_ui.fan_text, "%u%%", model->fan_percent);
    } else {
        lv_label_set_text(s_ui.nozzle_text, "Unavailable");
        lv_label_set_text(s_ui.bed_text, "Unavailable");
        lv_label_set_text(s_ui.fan_text, "Unavailable");
    }

    const bool paused = model->job_state == DT_UI_JOB_PAUSED;
    lv_label_set_text(s_ui.pause_label, paused ? "Resume" : "Pause");
    set_button_enabled(s_ui.pause_button, paused ? model->can_resume : model->can_pause);
    set_button_enabled(s_ui.cancel_button, model->can_cancel);
    return ESP_OK;
}
