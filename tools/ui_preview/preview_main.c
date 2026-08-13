#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "dt_ui.h"
#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"

void dt_ui_preview_show_confirmation(const char *name);

static int save_renderer_bmp(lv_display_t *display, const char *path)
{
    SDL_Renderer *renderer = (SDL_Renderer *)lv_sdl_window_get_renderer(display);
    const int width = 800;
    const int height = 480;
    const int pitch = width * 4;
    uint8_t *pixels = malloc((size_t)pitch * height);
    if (renderer == NULL || pixels == NULL) {
        fprintf(stderr, "Could not read preview renderer (renderer=%p, pixels=%p)\n",
                (void *)renderer, (void *)pixels);
        free(pixels);
        return 1;
    }

    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels, pitch) != 0) {
        fprintf(stderr, "SDL_RenderReadPixels: %s\n", SDL_GetError());
        free(pixels);
        return 1;
    }

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, width, height, 32, pitch, SDL_PIXELFORMAT_ARGB8888);
    if (surface == NULL) {
        fprintf(stderr, "SDL_CreateRGBSurfaceWithFormatFrom: %s\n", SDL_GetError());
        free(pixels);
        return 1;
    }

    const int result = SDL_SaveBMP(surface, path);
    if (result != 0) {
        fprintf(stderr, "SDL_SaveBMP: %s\n", SDL_GetError());
    }
    SDL_FreeSurface(surface);
    free(pixels);
    return result != 0;
}

typedef struct {
    const char *name;
    dt_ui_model_t model;
} preview_scenario_t;

static const preview_scenario_t SCENARIOS[] = {
    {"printing", {
        .device_name = "Workshop U1", .connection = DT_UI_CONNECTION_ONLINE,
        .job_state = DT_UI_JOB_PRINTING, .filename = "dragon_duct_v7.3mf",
        .progress_percent = 42, .elapsed_seconds = 3672, .remaining_seconds = 5088,
        .nozzle_c = 219.6f, .nozzle_target_c = 220.0f, .bed_c = 59.8f,
        .bed_target_c = 60.0f, .fan_percent = 78, .can_pause = true,
        .can_resume = false, .can_cancel = true,
    }},
    {"disconnected", {
        .device_name = "Workshop U1", .connection = DT_UI_CONNECTION_OFFLINE,
        .job_state = DT_UI_JOB_IDLE, .filename = "State unavailable",
    }},
    {"idle", {
        .device_name = "Workshop U1", .connection = DT_UI_CONNECTION_ONLINE,
        .job_state = DT_UI_JOB_IDLE, .filename = "No active file",
        .nozzle_c = 24.3f, .bed_c = 23.8f,
    }},
    {"paused", {
        .device_name = "Workshop U1", .connection = DT_UI_CONNECTION_ONLINE,
        .job_state = DT_UI_JOB_PAUSED, .filename = "dragon_duct_v7.3mf",
        .progress_percent = 58, .elapsed_seconds = 5214, .remaining_seconds = 3770,
        .nozzle_c = 219.4f, .nozzle_target_c = 220.0f, .bed_c = 59.9f,
        .bed_target_c = 60.0f, .fan_percent = 40, .can_resume = true,
        .can_cancel = true,
    }},
    {"fault", {
        .device_name = "Workshop U1", .connection = DT_UI_CONNECTION_ONLINE,
        .job_state = DT_UI_JOB_ERROR, .filename = "HEATER FAULT - controls locked",
        .progress_percent = 17, .elapsed_seconds = 1462,
        .nozzle_c = 31.2f, .bed_c = 28.1f,
    }},
};

static const preview_scenario_t *find_scenario(const char *name)
{
    for (size_t i = 0; i < sizeof(SCENARIOS) / sizeof(SCENARIOS[0]); ++i) {
        if (strcmp(name, SCENARIOS[i].name) == 0) {
            return &SCENARIOS[i];
        }
    }
    return NULL;
}

static bool parse_page(const char *name, dt_ui_page_t *page)
{
    static const char *names[] = {"home", "control", "files", "filament", "devices", "settings"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0) {
            *page = (dt_ui_page_t)i;
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    bool interactive = false;
    const char *output = "dragon-touch-ui.bmp";
    const char *scenario_name = "printing";
    const char *confirmation_name = NULL;
    dt_ui_page_t page = DT_UI_PAGE_HOME;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--interactive") == 0) {
            interactive = true;
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            scenario_name = argv[++i];
        } else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            if (!parse_page(argv[++i], &page)) {
                fprintf(stderr, "Unknown page: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--confirmation") == 0 && i + 1 < argc) {
            confirmation_name = argv[++i];
        } else if (argv[i][0] != '-') {
            output = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [--interactive] [--scenario NAME] [--page NAME] [output.bmp]\n", argv[0]);
            return 2;
        }
    }
    const preview_scenario_t *scenario = find_scenario(scenario_name);
    if (scenario == NULL) {
        fprintf(stderr, "Unknown scenario '%s' (use printing, disconnected, idle, paused, or fault)\n",
                scenario_name);
        return 2;
    }
    lv_init();

    lv_display_t *display = lv_sdl_window_create(800, 480);
    if (display == NULL || dt_ui_create(display) != ESP_OK) {
        fprintf(stderr, "Could not create the DragonTouch preview\n");
        return 1;
    }
    if (lv_sdl_window_get_renderer(display) == NULL) {
        fprintf(stderr, "Could not create SDL renderer: %s\n", SDL_GetError());
        return 1;
    }
    lv_sdl_window_set_title(display, "DragonTouch UI preview");
    if (interactive) {
        lv_sdl_mouse_create();
    }

    dt_ui_update(&scenario->model);
    dt_ui_show_page(page);
    if (confirmation_name != NULL) {
        dt_ui_preview_show_confirmation(confirmation_name);
    }

    if (interactive) {
        while (lv_display_get_default() != NULL) {
            uint32_t delay_ms = lv_timer_handler();
            if (delay_ms < 1 || delay_ms > 16) {
                delay_ms = 16;
            }
            SDL_Delay(delay_ms);
        }
        return 0;
    }

    for (int i = 0; i < 8; ++i) {
        lv_timer_handler();
        SDL_Delay(20);
        lv_tick_inc(20);
    }
    lv_refr_now(display);
    SDL_Delay(100);

    int result = save_renderer_bmp(display, output);
    lv_sdl_quit();
    return result;
}
