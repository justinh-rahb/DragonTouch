#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "dt_ui.h"
#include "lvgl.h"
#include "src/drivers/sdl/lv_sdl_mouse.h"
#include "src/drivers/sdl/lv_sdl_window.h"

static int save_renderer_bmp(lv_display_t *display, const char *path)
{
    SDL_Renderer *renderer = (SDL_Renderer *)lv_sdl_window_get_renderer(display);
    const int width = 800;
    const int height = 480;
    const int pitch = width * 4;
    uint8_t *pixels = malloc((size_t)pitch * height);
    if (renderer == NULL || pixels == NULL) {
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

int main(int argc, char **argv)
{
    const bool interactive = argc > 1 && strcmp(argv[1], "--interactive") == 0;
    const char *output = argc > 1 && !interactive ? argv[1] : "dragon-touch-ui.bmp";
    lv_init();

    lv_display_t *display = lv_sdl_window_create(800, 480);
    if (display == NULL || dt_ui_create(display) != ESP_OK) {
        fprintf(stderr, "Could not create the DragonTouch preview\n");
        return 1;
    }
    lv_sdl_window_set_title(display, "DragonTouch UI preview");
    lv_sdl_mouse_create();

    const dt_ui_model_t model = {
        .device_name = "Workshop U1",
        .connection = DT_UI_CONNECTION_ONLINE,
        .job_state = DT_UI_JOB_PRINTING,
        .filename = "dragon_duct_v7.3mf",
        .progress_percent = 42,
        .elapsed_seconds = 3672,
        .remaining_seconds = 5088,
        .nozzle_c = 219.6f,
        .nozzle_target_c = 220.0f,
        .bed_c = 59.8f,
        .bed_target_c = 60.0f,
        .fan_percent = 78,
        .can_pause = true,
        .can_resume = false,
        .can_cancel = true,
    };
    dt_ui_update(&model);

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

    int result = save_renderer_bmp(display, output);
    lv_sdl_quit();
    return result;
}
