#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

ROOT = Path.cwd()

files = {
    "board_h": ROOT / "components/dt_board/include/dt_board.h",
    "board_c": ROOT / "components/dt_board/dt_board.c",
    "waveshare_c": ROOT / "components/dt_board/dt_board_waveshare_7.c",
    "board_cmake": ROOT / "components/dt_board/CMakeLists.txt",
    "main_c": ROOT / "main/app_main.c",
    "main_cmake": ROOT / "main/CMakeLists.txt",
}

for name, path in files.items():
    if not path.exists():
        sys.exit(f"ERROR: required file missing: {path}")

stamp = time.strftime("%Y%m%d-%H%M%S")
backup_dir = ROOT / "backups" / f"source-pre-lvgl-{stamp}"
backup_dir.mkdir(parents=True, exist_ok=True)

for path in files.values():
    dest = backup_dir / path.relative_to(ROOT)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, dest)

old_gt911_backup = ROOT / "components/dt_board/dt_board_waveshare_7.c.pre-gt911"
if old_gt911_backup.exists():
    shutil.move(
        old_gt911_backup,
        backup_dir / "dt_board_waveshare_7.c.pre-gt911"
    )

print(f"Backed up current sources to: {backup_dir}")

files["board_h"].write_text(r'''#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include "lvgl.h"

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

#include "dt_board_waveshare_7.h"
#define DT_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-7"

#else

#include "dt_board_ktouch.h"
#define DT_BOARD_NAME "BIGTREETECH K-Touch / PandaTouch"

#endif

/*
 * Initialize board hardware, LVGL display, and input device.
 * Backlight remains OFF until dt_board_lvgl_start().
 */
esp_err_t dt_board_lvgl_init(lv_display_t **display);

/*
 * Perform the first complete LVGL render, enable the backlight,
 * and start the LVGL service task.
 */
esp_err_t dt_board_lvgl_start(void);
''')

files["board_c"].write_text(r'''#include "dt_board.h"

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display);
esp_err_t dt_board_waveshare_7_lvgl_start(void);

#endif


esp_err_t dt_board_lvgl_init(lv_display_t **display)
{
#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)
    return dt_board_waveshare_7_lvgl_init(display);
#else
    (void)display;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


esp_err_t dt_board_lvgl_start(void)
{
#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)
    return dt_board_waveshare_7_lvgl_start();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
''')

files["waveshare_c"].write_text(r'''#include "dt_board.h"

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"


#define GT911_I2C_ADDR        0x5D
#define GT911_REG_PRODUCT_ID  0x8140
#define GT911_REG_STATUS      0x814E
#define GT911_REG_POINTS      0x814F
#define GT911_MAX_POINTS      5

/*
 * 48 rows = exactly 1/10 of the 800x480 screen.
 * Two RGB565 buffers use ~150 KiB total in PSRAM.
 */
#define DT_LVGL_BUFFER_ROWS   48
#define DT_LVGL_BUFFER_PIXELS (DT_LCD_H_RES * DT_LVGL_BUFFER_ROWS)
#define DT_LVGL_BUFFER_BYTES  (DT_LVGL_BUFFER_PIXELS * sizeof(uint16_t))


static const char *TAG = "dt_waveshare_7";

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_lv_display;
static lv_indev_t *s_lv_touch;

static void *s_lv_buf1;
static void *s_lv_buf2;

static uint8_t s_ch422g_shadow;

static bool s_touch_pressed;
static uint16_t s_touch_x;
static uint16_t s_touch_y;

static TaskHandle_t s_lvgl_task;


/* -------------------------------------------------------------------------- */
/* CH422G                                                                     */
/* -------------------------------------------------------------------------- */

static esp_err_t ch422g_write_command(uint8_t address, uint8_t value)
{
    return i2c_master_write_to_device(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        address,
        &value,
        sizeof(value),
        pdMS_TO_TICKS(100)
    );
}


static esp_err_t ch422g_write_shadow(void)
{
    uint8_t enable = 0x01;

    ESP_RETURN_ON_ERROR(
        ch422g_write_command(DT_CH422G_ADDR_CONFIG, enable),
        TAG,
        "CH422G configuration failed"
    );

    ESP_RETURN_ON_ERROR(
        ch422g_write_command(DT_CH422G_ADDR_OUTPUT, s_ch422g_shadow),
        TAG,
        "CH422G output failed"
    );

    return ESP_OK;
}


static esp_err_t ch422g_set(unsigned pin, bool state)
{
    if (pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }

    if (state) {
        s_ch422g_shadow |= (uint8_t)(1U << pin);
    } else {
        s_ch422g_shadow &= (uint8_t)~(1U << pin);
    }

    return ch422g_write_shadow();
}


/* -------------------------------------------------------------------------- */
/* I2C                                                                        */
/* -------------------------------------------------------------------------- */

static esp_err_t waveshare_i2c_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = DT_TOUCH_I2C_SDA_GPIO,
        .scl_io_num = DT_TOUCH_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = DT_TOUCH_I2C_HZ,
        },
        .clk_flags = 0,
    };

    ESP_LOGI(
        TAG,
        "I2C SDA=%d SCL=%d @ %u Hz",
        DT_TOUCH_I2C_SDA_GPIO,
        DT_TOUCH_I2C_SCL_GPIO,
        DT_TOUCH_I2C_HZ
    );

    ESP_RETURN_ON_ERROR(
        i2c_param_config((i2c_port_t)DT_TOUCH_I2C_PORT, &config),
        TAG,
        "I2C config failed"
    );

    esp_err_t err = i2c_driver_install(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        I2C_MODE_MASTER,
        0,
        0,
        0
    );

    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return err;
}


/* -------------------------------------------------------------------------- */
/* Board power/reset                                                          */
/* -------------------------------------------------------------------------- */

static esp_err_t waveshare_expander_init(void)
{
    /*
     * EXIO1 touch reset      HIGH
     * EXIO2 backlight        LOW
     * EXIO3 LCD reset        LOW
     * EXIO4 SD CS            HIGH
     * EXIO5 USB/CAN select   LOW  (USB)
     * EXIO6 LCD VDD          HIGH
     */

    s_ch422g_shadow =
        (1U << DT_IOEXP_TOUCH_RESET) |
        (1U << DT_IOEXP_SD_CS) |
        (1U << DT_IOEXP_LCD_VDD_EN);

    ESP_LOGI(TAG, "CH422G initial state=0x%02x", s_ch422g_shadow);

    ESP_RETURN_ON_ERROR(
        ch422g_write_shadow(),
        TAG,
        "CH422G init failed"
    );

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_LCD_RESET, true),
        TAG,
        "LCD reset release failed"
    );

    vTaskDelay(pdMS_TO_TICKS(120));

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* RGB panel                                                                  */
/* -------------------------------------------------------------------------- */

static esp_err_t waveshare_rgb_init(void)
{
    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,

        .timings = {
            .pclk_hz = DT_LCD_PCLK_HZ,
            .h_res = DT_LCD_H_RES,
            .v_res = DT_LCD_V_RES,

            .hsync_pulse_width = DT_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = DT_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = DT_LCD_HSYNC_FRONT_PORCH,

            .vsync_pulse_width = DT_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = DT_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = DT_LCD_VSYNC_FRONT_PORCH,

            .flags = {
                .hsync_idle_low = 0,
                .vsync_idle_low = 0,
                .de_idle_high = 0,
                .pclk_active_neg = DT_LCD_PCLK_ACTIVE_NEG,
                .pclk_idle_high = DT_LCD_PCLK_IDLE_HIGH,
            },
        },

        .data_width = 16,
        .bits_per_pixel = 16,

        .num_fbs = 1,

        /*
         * Internal-SRAM bounce buffer feeding the RGB EDMA engine.
         */
        .bounce_buffer_size_px = DT_LCD_H_RES * 10,

        .psram_trans_align = 64,

        .hsync_gpio_num = DT_LCD_HSYNC_GPIO,
        .vsync_gpio_num = DT_LCD_VSYNC_GPIO,
        .de_gpio_num = DT_LCD_DE_GPIO,
        .pclk_gpio_num = DT_LCD_PCLK_GPIO,
        .disp_gpio_num = -1,

        /*
         * D0..D15:
         * B0..B4, G0..G5, R0..R4
         */
        .data_gpio_nums = {
            DT_LCD_B3_GPIO,
            DT_LCD_B4_GPIO,
            DT_LCD_B5_GPIO,
            DT_LCD_B6_GPIO,
            DT_LCD_B7_GPIO,

            DT_LCD_G2_GPIO,
            DT_LCD_G3_GPIO,
            DT_LCD_G4_GPIO,
            DT_LCD_G5_GPIO,
            DT_LCD_G6_GPIO,
            DT_LCD_G7_GPIO,

            DT_LCD_R3_GPIO,
            DT_LCD_R4_GPIO,
            DT_LCD_R5_GPIO,
            DT_LCD_R6_GPIO,
            DT_LCD_R7_GPIO,
        },

        .flags = {
            .disp_active_low = 0,
            .refresh_on_demand = 0,
            .fb_in_psram = 1,
            .double_fb = 0,
            .no_fb = 0,
            .bb_invalidate_cache = 0,
        },
    };

    ESP_LOGI(
        TAG,
        "RGB panel %ux%u @ %.2f MHz",
        DT_LCD_H_RES,
        DT_LCD_V_RES,
        DT_LCD_PCLK_HZ / 1000000.0
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_rgb_panel(&config, &s_panel),
        TAG,
        "RGB panel create failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(s_panel),
        TAG,
        "RGB panel init failed"
    );

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* GT911                                                                      */
/* -------------------------------------------------------------------------- */

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
    };

    return i2c_master_write_read_device(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        GT911_I2C_ADDR,
        addr,
        sizeof(addr),
        data,
        len,
        pdMS_TO_TICKS(100)
    );
}


static esp_err_t gt911_write_u8(uint16_t reg, uint8_t value)
{
    uint8_t data[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
        value,
    };

    return i2c_master_write_to_device(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        GT911_I2C_ADDR,
        data,
        sizeof(data),
        pdMS_TO_TICKS(100)
    );
}


static esp_err_t gt911_reset_select_address(void)
{
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << DT_TOUCH_INTERRUPT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&int_cfg),
        TAG,
        "GT911 INT config failed"
    );

    ESP_RETURN_ON_ERROR(
        gpio_set_level(DT_TOUCH_INTERRUPT_GPIO, 0),
        TAG,
        "GT911 INT low failed"
    );

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_TOUCH_RESET, false),
        TAG,
        "GT911 reset assert failed"
    );

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_TOUCH_RESET, true),
        TAG,
        "GT911 reset release failed"
    );

    vTaskDelay(pdMS_TO_TICKS(10));

    int_cfg.mode = GPIO_MODE_INPUT;

    ESP_RETURN_ON_ERROR(
        gpio_config(&int_cfg),
        TAG,
        "GT911 INT input failed"
    );

    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}


static esp_err_t gt911_probe(void)
{
    uint8_t product_id[4] = {0};

    ESP_RETURN_ON_ERROR(
        gt911_read(
            GT911_REG_PRODUCT_ID,
            product_id,
            sizeof(product_id)
        ),
        TAG,
        "GT911 probe failed"
    );

    ESP_LOGI(
        TAG,
        "GT911 product='%c%c%c%c' address=0x%02x",
        product_id[0],
        product_id[1],
        product_id[2],
        product_id[3],
        GT911_I2C_ADDR
    );

    return ESP_OK;
}


static esp_err_t gt911_sample(void)
{
    uint8_t status = 0;

    ESP_RETURN_ON_ERROR(
        gt911_read(GT911_REG_STATUS, &status, 1),
        TAG,
        "GT911 status read failed"
    );

    if ((status & 0x80) == 0) {
        return ESP_OK;
    }

    uint8_t count = status & 0x0F;

    if (count > GT911_MAX_POINTS) {
        (void)gt911_write_u8(GT911_REG_STATUS, 0);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (count == 0) {
        s_touch_pressed = false;
    } else {
        uint8_t point[8];

        ESP_RETURN_ON_ERROR(
            gt911_read(GT911_REG_POINTS, point, sizeof(point)),
            TAG,
            "GT911 point read failed"
        );

        uint16_t x =
            (uint16_t)point[1] |
            ((uint16_t)point[2] << 8);

        uint16_t y =
            (uint16_t)point[3] |
            ((uint16_t)point[4] << 8);

        /*
         * Hardware testing established native landscape orientation:
         * no axis swap and no inversion required.
         */
        if (x >= DT_LCD_H_RES) {
            x = DT_LCD_H_RES - 1;
        }

        if (y >= DT_LCD_V_RES) {
            y = DT_LCD_V_RES - 1;
        }

        s_touch_x = x;
        s_touch_y = y;
        s_touch_pressed = true;
    }

    return gt911_write_u8(GT911_REG_STATUS, 0);
}


/* -------------------------------------------------------------------------- */
/* LVGL bridge                                                                */
/* -------------------------------------------------------------------------- */

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}


static void lvgl_flush_cb(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *px_map
)
{
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        px_map
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LVGL flush failed: %s", esp_err_to_name(err));
    }

    /*
     * esp_lcd_panel_draw_bitmap() has copied the partial LVGL buffer
     * into the RGB driver's framebuffer at this point.
     */
    lv_display_flush_ready(display);
}


static void lvgl_touch_read_cb(
    lv_indev_t *indev,
    lv_indev_data_t *data
)
{
    (void)indev;

    esp_err_t err = gt911_sample();

    if (err != ESP_OK) {
        /*
         * Preserve the last known state on a transient I2C failure.
         * Do not spam the console from the high-frequency LVGL callback.
         */
    }

    data->point.x = s_touch_x;
    data->point.y = s_touch_y;
    data->state =
        s_touch_pressed
            ? LV_INDEV_STATE_PRESSED
            : LV_INDEV_STATE_RELEASED;
}


static void lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "LVGL service task started");

    while (true) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}


/* -------------------------------------------------------------------------- */
/* Public board API                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t dt_board_waveshare_7_lvgl_init(lv_display_t **display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Waveshare LVGL hardware initialization");

    ESP_RETURN_ON_ERROR(
        waveshare_i2c_init(),
        TAG,
        "I2C initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        waveshare_expander_init(),
        TAG,
        "CH422G initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        waveshare_rgb_init(),
        TAG,
        "RGB initialization failed"
    );

    ESP_RETURN_ON_ERROR(
        gt911_reset_select_address(),
        TAG,
        "GT911 reset failed"
    );

    ESP_RETURN_ON_ERROR(
        gt911_probe(),
        TAG,
        "GT911 probe failed"
    );

    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);

    s_lv_buf1 = heap_caps_malloc(
        DT_LVGL_BUFFER_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    s_lv_buf2 = heap_caps_malloc(
        DT_LVGL_BUFFER_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (s_lv_buf1 == NULL || s_lv_buf2 == NULL) {
        ESP_LOGE(TAG, "LVGL draw-buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "LVGL buffers: 2 x %u bytes in PSRAM",
        (unsigned)DT_LVGL_BUFFER_BYTES
    );

    s_lv_display = lv_display_create(
        DT_LCD_H_RES,
        DT_LCD_V_RES
    );

    if (s_lv_display == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_color_format(
        s_lv_display,
        LV_COLOR_FORMAT_RGB565
    );

    lv_display_set_flush_cb(
        s_lv_display,
        lvgl_flush_cb
    );

    lv_display_set_buffers(
        s_lv_display,
        s_lv_buf1,
        s_lv_buf2,
        DT_LVGL_BUFFER_BYTES,
        LV_DISPLAY_RENDER_MODE_PARTIAL
    );

    lv_display_set_default(s_lv_display);

    s_lv_touch = lv_indev_create();

    if (s_lv_touch == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_indev_set_type(
        s_lv_touch,
        LV_INDEV_TYPE_POINTER
    );

    lv_indev_set_read_cb(
        s_lv_touch,
        lvgl_touch_read_cb
    );

    lv_indev_set_display(
        s_lv_touch,
        s_lv_display
    );

    *display = s_lv_display;

    ESP_LOGI(TAG, "LVGL display and GT911 input registered");

    return ESP_OK;
}


esp_err_t dt_board_waveshare_7_lvgl_start(void)
{
    if (s_lv_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lvgl_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Render DragonTouch completely while the backlight is still off.
     */
    ESP_LOGI(TAG, "rendering first DragonTouch frame");

    lv_refr_now(s_lv_display);

    ESP_LOGI(TAG, "enabling LCD backlight");

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_LCD_BACKLIGHT, true),
        TAG,
        "backlight enable failed"
    );

    BaseType_t result = xTaskCreate(
        lvgl_task,
        "dt_lvgl",
        8192,
        NULL,
        5,
        &s_lvgl_task
    );

    if (result != pdPASS) {
        s_lvgl_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DragonTouch LVGL hardware path active");

    return ESP_OK;
}

#endif
''')

files["board_cmake"].write_text(r'''idf_component_register(
    SRCS
        "dt_board.c"
        "dt_board_waveshare_7.c"

    INCLUDE_DIRS
        "include"

    REQUIRES
        esp_lcd
        driver
        esp_timer
        heap
        lvgl
)
''')

files["main_cmake"].write_text(r'''idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        dt_board
        dt_ui
        esp_psram
        spi_flash
        heap
)
''')

files["main_c"].write_text(r'''#include <inttypes.h>

#include "dt_board.h"
#include "dt_ui.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"


static const char *TAG = "dragon_touch";


void app_main(void)
{
    esp_chip_info_t chip = {0};
    uint32_t flash_bytes = 0;

    esp_chip_info(&chip);

    ESP_ERROR_CHECK(
        esp_flash_get_size(NULL, &flash_bytes)
    );

    ESP_LOGI(TAG, "DragonTouch hardware UI boot");

    ESP_LOGI(
        TAG,
        "chip cores=%u revision=%u flash=%" PRIu32 " bytes",
        chip.cores,
        chip.revision,
        flash_bytes
    );

    ESP_LOGI(
        TAG,
        "PSRAM=%u bytes; internal heap=%u bytes",
        (unsigned)esp_psram_get_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
    );

    ESP_LOGI(TAG, "board: %s", DT_BOARD_NAME);

    lv_display_t *display = NULL;

    ESP_ERROR_CHECK(
        dt_board_lvgl_init(&display)
    );

    /*
     * Build the actual DragonTouch UI while the backlight remains dark.
     */
    ESP_ERROR_CHECK(
        dt_ui_create(display)
    );

    /*
     * Temporary passive model until dragon-core is attached.
     */
    const dt_ui_model_t model = {
        .device_name = NULL,
        .connection = DT_UI_CONNECTION_OFFLINE,
        .job_state = DT_UI_JOB_IDLE,
        .filename = NULL,
        .progress_percent = 0,
        .elapsed_seconds = 0,
        .remaining_seconds = 0,
        .nozzle_c = 0.0f,
        .nozzle_target_c = 0.0f,
        .bed_c = 0.0f,
        .bed_target_c = 0.0f,
        .fan_percent = 0,
        .can_pause = false,
        .can_resume = false,
        .can_cancel = false,
    };

    ESP_ERROR_CHECK(
        dt_ui_update(&model)
    );

    ESP_ERROR_CHECK(
        dt_board_lvgl_start()
    );

    ESP_LOGI(TAG, "DragonTouch UI running");
}
''')

print()
print("LVGL integration patch applied.")
print("The temporary raw-touch task has been replaced by an LVGL pointer device.")
print("No firmware has been flashed.")
