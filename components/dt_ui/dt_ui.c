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

typedef enum {
    DT_PAGE_HOME = 0,
    DT_PAGE_CONTROL,
    DT_PAGE_FILES,
    DT_PAGE_FILAMENT,
    DT_PAGE_DEVICES,
    DT_PAGE_SETTINGS,
    DT_PAGE_COUNT,
} dt_page_t;

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
    lv_obj_t *pause_button;
    lv_obj_t *pause_label;
    lv_obj_t *cancel_button;
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
    lv_obj_set_height(button, 42);
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_color(button,
                              color(primary ? DT_COLOR_ACCENT : DT_COLOR_SURFACE_2), 0);
    lv_obj_t *label = make_label(button, text, DT_COLOR_TEXT);
    lv_obj_center(label);
    return button;
}

static void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static void show_page(dt_page_t selected)
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
    dt_page_t page = (dt_page_t)(uintptr_t)lv_event_get_user_data(event);
    show_page(page);
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
    make_label(printer, "No paired printer", DT_COLOR_TEXT);
    make_label(printer, "Pair a same-LAN device to begin.", DT_COLOR_MUTED);
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

static void create_pages(lv_obj_t *content)
{
    for (int i = 0; i < DT_PAGE_COUNT; ++i) {
        s_ui.pages[i] = make_page(content);
    }

    create_home_page(s_ui.pages[DT_PAGE_HOME]);

    static const char *control_cards[] = {"MOTION", "TEMPERATURE", "EXTRUSION", "FANS"};
    create_stub_page(s_ui.pages[DT_PAGE_CONTROL], "Control",
                     "Manual controls remain locked until the selected printer declares support.",
                     control_cards, 4);

    static const char *file_cards[] = {"RECENT FILES", "STORAGE"};
    create_stub_page(s_ui.pages[DT_PAGE_FILES], "Print files",
                     "Browse, inspect, and start jobs from the selected printer.", file_cards, 2);

    static const char *filament_cards[] = {"ACTIVE TOOL", "MATERIAL SLOTS", "LOAD / UNLOAD"};
    create_stub_page(s_ui.pages[DT_PAGE_FILAMENT], "Filament",
                     "Tool and material controls adapt to the selected printer's capabilities.",
                     filament_cards, 3);

    static const char *device_cards[] = {"SELECTED PRINTER", "DISCOVERED DEVICES", "DRAGON GROUP"};
    create_stub_page(s_ui.pages[DT_PAGE_DEVICES], "Devices",
                     "Discover and explicitly pair same-LAN printers and Dragon-family siblings.",
                     device_cards, 3);

    static const char *settings_cards[] = {"WI-FI", "DISPLAY", "UPDATE & RECOVERY", "ABOUT"};
    create_stub_page(s_ui.pages[DT_PAGE_SETTINGS], "Settings",
                     "Device-local preferences, provisioning, diagnostics, and recovery.",
                     settings_cards, 4);
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
    show_page(DT_PAGE_HOME);

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

static const char *job_state_text(dt_ui_job_state_t state)
{
    switch (state) {
    case DT_UI_JOB_PRINTING: return "Printing";
    case DT_UI_JOB_PAUSED: return "Paused";
    case DT_UI_JOB_COMPLETE: return "Complete";
    case DT_UI_JOB_ERROR: return "Print error";
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
    lv_label_set_text(s_ui.filename,
                      model->filename != NULL && model->filename[0] != '\0'
                          ? model->filename : "No active file");
    uint8_t progress = model->progress_percent > 100 ? 100 : model->progress_percent;
    lv_bar_set_value(s_ui.progress, progress, LV_ANIM_ON);
    lv_label_set_text_fmt(s_ui.progress_text, "%u%%", progress);

    char elapsed[20];
    char remaining[20];
    char time_line[64];
    format_duration(elapsed, sizeof(elapsed), model->elapsed_seconds);
    format_duration(remaining, sizeof(remaining), model->remaining_seconds);
    snprintf(time_line, sizeof(time_line), "Elapsed %s  •  Remaining %s", elapsed, remaining);
    lv_label_set_text(s_ui.time_text, time_line);
    lv_label_set_text_fmt(s_ui.nozzle_text, "%.1f / %.0f °C",
                          model->nozzle_c, model->nozzle_target_c);
    lv_label_set_text_fmt(s_ui.bed_text, "%.1f / %.0f °C",
                          model->bed_c, model->bed_target_c);
    lv_label_set_text_fmt(s_ui.fan_text, "%u%%", model->fan_percent);

    const bool paused = model->job_state == DT_UI_JOB_PAUSED;
    lv_label_set_text(s_ui.pause_label, paused ? "Resume" : "Pause");
    set_button_enabled(s_ui.pause_button, paused ? model->can_resume : model->can_pause);
    set_button_enabled(s_ui.cancel_button, model->can_cancel);
    return ESP_OK;
}
