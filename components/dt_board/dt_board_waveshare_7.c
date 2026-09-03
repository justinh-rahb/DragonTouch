#include "dt_board.h"

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
#include "esp_lvgl_port.h"


#define GT911_I2C_ADDR        0x5D
#define GT911_REG_PRODUCT_ID  0x8140
#define GT911_REG_STATUS      0x814E
#define GT911_REG_POINTS      0x814F
#define GT911_MAX_POINTS      5

/*
 * Hardware acquisition and LVGL consumption are deliberately decoupled.
 *
 * GT911 is sampled quickly in its own task. LVGL's read callback only
 * copies cached state and therefore never performs blocking I2C.
 */
#define DT_GT911_POLL_MS         5
#define DT_LVGL_INPUT_PERIOD_MS 10

/*
 * 48 rows = exactly 1/10 of the 800x480 screen.
 * Two RGB565 buffers use ~150 KiB total in PSRAM.
 */
/*
 * LVGL DIRECT double-framebuffer mode.
 *
 * Both screen-sized RGB565 buffers are owned by ESP-IDF's RGB driver
 * and live in PSRAM. LVGL renders directly into them.
 */
#define DT_FRAME_BUFFER_PIXELS (DT_LCD_H_RES * DT_LCD_V_RES)
#define DT_FRAME_BUFFER_BYTES  (DT_FRAME_BUFFER_PIXELS * sizeof(uint16_t))


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
static TaskHandle_t s_touch_task;

/*
 * In DIRECT double-buffer mode, LVGL must not start modifying the
 * previous buffer until the RGB engine reaches VSYNC.
 */
static volatile bool s_waiting_for_vsync;

/*
 * Protect the tiny cached touch-state structure.
 * No LVGL calls occur while this lock is held.
 */
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;

/*
 * Preserve physical touch transitions until LVGL consumes them.
 *
 * A state-only cache can lose an entire down/up sequence if both edges
 * happen between two LVGL input reads. LVGL explicitly supports buffered
 * input via data->continue_reading.
 */
#define DT_TOUCH_EVENT_QUEUE_LEN        8
#define DT_GT911_RELEASE_DEBOUNCE_MS   30
#define DT_GT911_RELEASE_DEBOUNCE_US   \
    ((int64_t)DT_GT911_RELEASE_DEBOUNCE_MS * 1000)

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} dt_touch_event_t;

static dt_touch_event_t
    s_touch_events[DT_TOUCH_EVENT_QUEUE_LEN];

static uint8_t s_touch_event_head;
static uint8_t s_touch_event_tail;
static uint8_t s_touch_event_count;
static uint32_t s_touch_event_drops;


static void touch_event_push_locked(
    uint16_t x,
    uint16_t y,
    bool pressed
)
{
    /*
     * Transition rates are tiny compared with this queue depth.
     * If it somehow fills, drop the oldest event rather than the newest.
     */
    if (s_touch_event_count == DT_TOUCH_EVENT_QUEUE_LEN) {
        s_touch_event_tail =
            (s_touch_event_tail + 1)
            % DT_TOUCH_EVENT_QUEUE_LEN;

        s_touch_event_count--;
        s_touch_event_drops++;
    }

    dt_touch_event_t *event =
        &s_touch_events[s_touch_event_head];

    event->x = x;
    event->y = y;
    event->pressed = pressed;

    s_touch_event_head =
        (s_touch_event_head + 1)
        % DT_TOUCH_EVENT_QUEUE_LEN;

    s_touch_event_count++;
}


static bool touch_event_pop_locked(
    dt_touch_event_t *event
)
{
    if (s_touch_event_count == 0) {
        return false;
    }

    *event =
        s_touch_events[s_touch_event_tail];

    s_touch_event_tail =
        (s_touch_event_tail + 1)
        % DT_TOUCH_EVENT_QUEUE_LEN;

    s_touch_event_count--;

    return true;
}



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
        .bounce_buffer_size_px = (DT_LCD_H_RES * 10),

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


static void gt911_poll_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "GT911 buffered edge queue active; "
        "poll=%d ms release_debounce=%d ms",
        DT_GT911_POLL_MS,
        DT_GT911_RELEASE_DEBOUNCE_MS
    );

    TickType_t wake_time =
        xTaskGetTickCount();

    TickType_t poll_ticks =
        pdMS_TO_TICKS(DT_GT911_POLL_MS);

    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    ESP_LOGI(
        TAG,
        "GT911 effective poll interval=%u ms (%u ticks)",
        (unsigned)(poll_ticks * portTICK_PERIOD_MS),
        (unsigned)poll_ticks
    );

    bool debounced_down = false;
    bool release_pending = false;

    int64_t release_started_us = 0;

    uint32_t read_errors = 0;

    while (true) {
        uint8_t status = 0;

        esp_err_t err = gt911_read(
            GT911_REG_STATUS,
            &status,
            1
        );

        if (
            err == ESP_OK &&
            (status & 0x80) != 0
        ) {
            const uint8_t count =
                status & 0x0F;

            if (
                count > 0 &&
                count <= GT911_MAX_POINTS
            ) {
                uint8_t point[8];

                err = gt911_read(
                    GT911_REG_POINTS,
                    point,
                    sizeof(point)
                );

                if (err == ESP_OK) {
                    uint16_t x =
                        (uint16_t)point[1] |
                        ((uint16_t)point[2] << 8);

                    uint16_t y =
                        (uint16_t)point[3] |
                        ((uint16_t)point[4] << 8);

                    if (x >= DT_LCD_H_RES) {
                        x = DT_LCD_H_RES - 1;
                    }

                    if (y >= DT_LCD_V_RES) {
                        y = DT_LCD_V_RES - 1;
                    }

                    /*
                     * Any valid contact cancels a possible transient
                     * zero-contact/release report.
                     */
                    release_pending = false;

                    taskENTER_CRITICAL(
                        &s_touch_lock
                    );

                    s_touch_x = x;
                    s_touch_y = y;
                    s_touch_pressed = true;

                    if (!debounced_down) {
                        /*
                         * Preserve the DOWN edge until LVGL consumes it.
                         */
                        touch_event_push_locked(
                            x,
                            y,
                            true
                        );

                        debounced_down = true;
                    }

                    taskEXIT_CRITICAL(
                        &s_touch_lock
                    );

                    err = gt911_write_u8(
                        GT911_REG_STATUS,
                        0
                    );
                }

            } else if (count == 0) {
                /*
                 * Do not turn one zero-contact report directly into UP.
                 *
                 * GT911 installations can occasionally produce a
                 * one-report dropout while a finger is still present.
                 */
                if (
                    debounced_down &&
                    !release_pending
                ) {
                    release_pending = true;

                    release_started_us =
                        esp_timer_get_time();
                }

                err = gt911_write_u8(
                    GT911_REG_STATUS,
                    0
                );

            } else {
                ESP_LOGW(
                    TAG,
                    "GT911 invalid touch count=%u",
                    count
                );

                err = gt911_write_u8(
                    GT911_REG_STATUS,
                    0
                );
            }
        }

        /*
         * Confirm UP only if no valid contact report arrived during the
         * debounce interval.
         *
         * This runs outside the "new data ready" condition because an
         * actual release may be followed by no further GT911 reports.
         */
        if (
            release_pending &&
            (
                esp_timer_get_time()
                - release_started_us
            ) >= DT_GT911_RELEASE_DEBOUNCE_US
        ) {
            release_pending = false;

            taskENTER_CRITICAL(
                &s_touch_lock
            );

            if (debounced_down) {
                debounced_down = false;
                s_touch_pressed = false;

                /*
                 * Preserve the UP edge too. The coordinates remain the
                 * last valid contact position, which is exactly what
                 * LVGL expects for a pointer release.
                 */
                touch_event_push_locked(
                    s_touch_x,
                    s_touch_y,
                    false
                );
            }

            taskEXIT_CRITICAL(
                &s_touch_lock
            );
        }

        if (err != ESP_OK) {
            read_errors++;

            if (
                read_errors == 1 ||
                (read_errors % 100) == 0
            ) {
                ESP_LOGW(
                    TAG,
                    "GT911 I2C error #%u: %s",
                    (unsigned)read_errors,
                    esp_err_to_name(err)
                );
            }
        }

        vTaskDelayUntil(
            &wake_time,
            poll_ticks
        );
    }
}


/* -------------------------------------------------------------------------- */
/* LVGL bridge                                                                */
/* -------------------------------------------------------------------------- */

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}


static bool rgb_vsync_cb(
    esp_lcd_panel_handle_t panel,
    const esp_lcd_rgb_panel_event_data_t *event_data,
    void *user_ctx
)
{
    (void)panel;
    (void)event_data;
    (void)user_ctx;

    BaseType_t task_woken = pdFALSE;

    if (s_waiting_for_vsync && s_lvgl_task != NULL) {
        s_waiting_for_vsync = false;

        vTaskNotifyGiveFromISR(
            s_lvgl_task,
            &task_woken
        );
    }

    return task_woken == pdTRUE;
}


static void lvgl_flush_cb(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *px_map
)
{
    (void)area;

    /*
     * DIRECT mode can generate several dirty-area flush callbacks for one
     * frame. LVGL has already rendered those areas into the complete
     * hardware framebuffer. Switch buffers only on the final callback.
     */
    if (!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    /*
     * Discard a stale task notification left by an earlier VSYNC.
     */
    (void)ulTaskNotifyTake(pdTRUE, 0);

    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        0,
        0,
        DT_LCD_H_RES,
        DT_LCD_V_RES,
        px_map
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "DIRECT framebuffer switch failed: %s",
            esp_err_to_name(err)
        );

        lv_display_flush_ready(display);
        return;
    }

    /*
     * The next VSYNC confirms that the RGB engine has had an opportunity
     * to switch to the newly rendered framebuffer.
     */
    s_waiting_for_vsync = true;
}


static void lvgl_flush_wait_cb(lv_display_t *display)
{
    if (lv_display_flush_is_last(display)) {
        if (ulTaskNotifyTake(
                pdTRUE,
                pdMS_TO_TICKS(100)
            ) == 0) {

            ESP_LOGW(
                TAG,
                "VSYNC wait timed out"
            );

            s_waiting_for_vsync = false;
        }
    }

    lv_display_flush_ready(display);
}


static void lvgl_touch_read_cb(
    lv_indev_t *indev,
    lv_indev_data_t *data
)
{
    (void)indev;

    dt_touch_event_t event;

    bool have_event;
    bool more_events;

    taskENTER_CRITICAL(
        &s_touch_lock
    );

    have_event =
        touch_event_pop_locked(
            &event
        );

    if (!have_event) {
        event.x = s_touch_x;
        event.y = s_touch_y;
        event.pressed =
            s_touch_pressed;
    }

    more_events =
        s_touch_event_count > 0;

    taskEXIT_CRITICAL(
        &s_touch_lock
    );

    data->point.x = event.x;
    data->point.y = event.y;

    data->state =
        event.pressed
            ? LV_INDEV_STATE_PRESSED
            : LV_INDEV_STATE_RELEASED;

    /*
     * LVGL 9 buffered-input mechanism.
     *
     * If DOWN and UP both occurred since the previous input timer tick,
     * LVGL immediately invokes us again and receives both edges in order.
     */
    data->continue_reading =
        more_events;
}


static void lvgl_task(void *arg)
{
    (void)arg;

    ESP_LOGI(
        TAG,
        "LVGL service task started (stack=24576 bytes)"
    );

    /*
     * First rendering is deliberately done here rather than from app_main().
     * Rendering the complete DragonTouch UI requires considerably more stack
     * than ESP-IDF's default main task provides.
     */
    ESP_LOGI(TAG, "rendering first DragonTouch frame");

    int64_t render_start_us = esp_timer_get_time();

    lv_refr_now(s_lv_display);

    int64_t render_elapsed_us =
        esp_timer_get_time() - render_start_us;

    ESP_LOGI(
        TAG,
        "first render complete in %lld ms; LVGL stack free=%u bytes",
        (long long)(render_elapsed_us / 1000),
        (unsigned)uxTaskGetStackHighWaterMark(NULL)
    );

    /*
     * Keep the LCD dark until a valid frame has been rendered.
     */
    esp_err_t err = ch422g_set(
        DT_IOEXP_LCD_BACKLIGHT,
        true
    );

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "backlight enable failed: %s",
            esp_err_to_name(err)
        );

        s_lvgl_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "LCD backlight enabled");

    /*
     * Start physical touch acquisition only after initial display
     * bring-up is complete.
     */
    BaseType_t touch_result = xTaskCreatePinnedToCore(
        gt911_poll_task,
        "gt911",
        4096,
        NULL,
        5,
        &s_touch_task,
        0
    );

    if (touch_result != pdPASS) {
        s_touch_task = NULL;

        ESP_LOGE(
            TAG,
            "failed to create GT911 acquisition task"
        );

    } else {
        ESP_LOGI(
            TAG,
            "GT911 acquisition task created"
        );
    }

    ESP_LOGI(TAG, "DragonTouch LVGL hardware path active");

    TickType_t next_stack_report =
        xTaskGetTickCount() + pdMS_TO_TICKS(5000);

    while (true) {
        uint32_t wait_ms = lv_timer_handler();

        /*
         * Keep touch/UI response quick while still yielding CPU time.
         */
        if (wait_ms < 5) {
            wait_ms = 5;
        }

        if (wait_ms > 20) {
            wait_ms = 20;
        }

        TickType_t delay_ticks =
            pdMS_TO_TICKS(wait_ms);

        if (delay_ticks == 0) {
            delay_ticks = 1;
        }

        vTaskDelay(delay_ticks);

        if (xTaskGetTickCount() >= next_stack_report) {
            ESP_LOGI(
                TAG,
                "LVGL stack free=%u bytes",
                (unsigned)uxTaskGetStackHighWaterMark(NULL)
            );

            next_stack_report =
                xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        }
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

    ESP_LOGI(
        TAG,
        "Waveshare hardware init + Espressif LVGL port"
    );

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

    /*
     * GT911 hardware bring-up.
     * This is the same reset/address sequence validated during raw-touch
     * bring-up. It does not modify RGB/LCD configuration.
     */
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

    /*
     * Let Espressif own:
     *
     * - lv_init()
     * - LVGL tick
     * - LVGL service task
     * - display flush synchronization
     * - RGB VSYNC/bounce-buffer synchronization
     */
    lvgl_port_cfg_t port_cfg =
        ESP_LVGL_PORT_INIT_CONFIG();
    /*
     * Keep the LVGL task stack in PSRAM so dc_portal can retain
     * its required 8192-byte internal HTTPD stack concurrently.
     * Draw buffers remain internal DMA SRAM; only task stack moves.
     */
    port_cfg.task_stack_caps =
        MALLOC_CAP_SPIRAM |
        MALLOC_CAP_8BIT;


    port_cfg.task_priority = 4;
    port_cfg.task_stack = 16384;
    port_cfg.task_affinity = 0;
    port_cfg.task_max_sleep_ms = 20;
    port_cfg.timer_period_ms = 5;

    ESP_RETURN_ON_ERROR(
        lvgl_port_init(&port_cfg),
        TAG,
        "esp_lvgl_port initialization failed"
    );

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = NULL,
        .panel_handle = s_panel,

        /*
         * avoid_tearing causes esp_lvgl_port to use the RGB driver's
         * actual full-screen framebuffers instead of allocating its
         * own unrelated buffers.
         */
        .buffer_size =
            DT_LCD_H_RES * 10,

        .double_buffer = true,

        .hres = DT_LCD_H_RES,
        .vres = DT_LCD_V_RES,

        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,

        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },

        .flags = {
            /*
             * LVGL renders into PSRAM scratch buffers.
             * The RGB driver's internal 10-line bounce buffers
             * remain DMA-capable and feed the LCD EDMA engine.
             */
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            /*
             * Our RGB driver uses internal DMA bounce buffers.
             * IDF 5.3 uses the bounce-frame-finish event for proper
             * synchronization in this mode.
             */
            .bb_mode = true,

            /*
             * Critical: obtain the RGB driver's two real framebuffers
             * and synchronize presentation at frame boundaries.
             */
            .avoid_tearing = false,
        },
    };

    s_lv_display =
        lvgl_port_add_disp_rgb(
            &display_cfg,
            &rgb_cfg
        );

    if (s_lv_display == NULL) {
        return ESP_FAIL;
    }

    lv_display_set_default(
        s_lv_display
    );

    /*
     * app_main() creates its UI between init() and start().
     * Hold the Espressif LVGL recursive mutex across that interval.
     */
    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    /*
     * GT911 I2C is NOT performed from this callback.
     *
     * gt911_poll_task() updates cached x/y/pressed state asynchronously;
     * lvgl_touch_read_cb() only copies that cached state.
     */
    s_lv_touch = lv_indev_create();

    if (s_lv_touch == NULL) {
        lvgl_port_unlock();
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

    lv_timer_t *touch_timer =
        lv_indev_get_read_timer(s_lv_touch);

    if (touch_timer != NULL) {
        lv_timer_set_period(
            touch_timer,
            DT_LVGL_INPUT_PERIOD_MS
        );
    }

    ESP_LOGI(
        TAG,
        "stable GT911 LVGL input registered (%d ms)",
        DT_LVGL_INPUT_PERIOD_MS
    );

    ESP_LOGI(
        TAG,
        "Espressif RGB/LVGL port registered"
    );

    ESP_LOGI(
        TAG,
        "Waveshare stable RGB mode active: "
        "1 FB + bounce, internal LVGL buffers, CPU0"
    );

    ESP_LOGI(
        TAG,
        "RGB mode: 1 FB + 10-line bounce buffer"
    );

    *display = s_lv_display;

    return ESP_OK;
}


esp_err_t dt_board_waveshare_7_lvgl_start(void)
{
    if (s_lv_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * UI creation is complete. Release the port task.
     *
     * Importantly, there is NO synchronous lv_refr_now() here.
     * The maintained LVGL port performs normal refresh scheduling.
     */
    lvgl_port_unlock();

    /*
     * Give the first normal refresh a short opportunity to complete
     * before exposing the panel.
     */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(
        ch422g_set(
            DT_IOEXP_LCD_BACKLIGHT,
            true
        ),
        TAG,
        "LCD backlight enable failed"
    );

    ESP_LOGI(
        TAG,
        "LCD backlight enabled; Espressif LVGL task owns display"
    );

    /*
     * Keep I2C touch acquisition off the display/LVGL core.
     *
     * CPU0:
     *   RGB initialization + esp_lvgl_port
     *
     * CPU1:
     *   low-cost GT911 polling
     */
    if (s_touch_task == NULL) {
        BaseType_t result = xTaskCreatePinnedToCore(
            gt911_poll_task,
            "gt911",
            4096,
            NULL,
            4,
            &s_touch_task,
            1
        );

        if (result != pdPASS) {
            s_touch_task = NULL;

            ESP_LOGE(
                TAG,
                "failed to create GT911 task"
            );

            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(
            TAG,
            "GT911 acquisition task started on CPU1"
        );
    }

    return ESP_OK;
}

#endif
