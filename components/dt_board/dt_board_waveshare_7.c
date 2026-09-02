#include "dt_board.h"

#if defined(CONFIG_DT_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7)

#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "dt_waveshare_7";

static esp_lcd_panel_handle_t s_panel = NULL;
static uint8_t s_ch422g_shadow = 0;


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
    /*
     * CH422G command address 0x24:
     * 0x01 enables IO0..IO7 as push-pull outputs.
     */
    uint8_t enable = 0x01;

    esp_err_t err = ch422g_write_command(
        DT_CH422G_ADDR_CONFIG,
        enable
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "CH422G configuration write failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /*
     * CH422G command address 0x38:
     * write complete IO0..IO7 output state.
     */
    err = ch422g_write_command(
        DT_CH422G_ADDR_OUTPUT,
        s_ch422g_shadow
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "CH422G output write failed: %s",
                 esp_err_to_name(err));
    }

    return err;
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

    ESP_LOGI(TAG,
             "initializing shared I2C: SDA=%d SCL=%d @ %u Hz",
             DT_TOUCH_I2C_SDA_GPIO,
             DT_TOUCH_I2C_SCL_GPIO,
             DT_TOUCH_I2C_HZ);

    esp_err_t err = i2c_param_config(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        &config
    );

    if (err != ESP_OK) {
        return err;
    }

    err = i2c_driver_install(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        I2C_MODE_MASTER,
        0,
        0,
        0
    );

    if (err == ESP_ERR_INVALID_STATE) {
        /*
         * Bus already installed. This will become useful when the GT911
         * driver begins sharing the bus.
         */
        ESP_LOGW(TAG, "I2C driver already installed");
        return ESP_OK;
    }

    return err;
}


static esp_err_t waveshare_expander_init(void)
{
    /*
     * Conservative initial state:
     *
     * EXIO1 TP_RST    HIGH  - don't hold GT911 in reset
     * EXIO2 LCD_BL    LOW   - keep backlight dark
     * EXIO3 LCD_RST   LOW   - hold LCD in reset
     * EXIO4 SD_CS     HIGH  - SD deselected
     * EXIO5 USB_SEL   LOW   - preserve native USB mode
     * EXIO6 LCD_VDD   HIGH  - panel power enabled
     */

    s_ch422g_shadow =
        (1U << DT_IOEXP_TOUCH_RESET) |
        (1U << DT_IOEXP_SD_CS) |
        (1U << DT_IOEXP_LCD_VDD_EN);

    ESP_LOGI(TAG,
             "CH422G initial state=0x%02x "
             "(panel power on, backlight off, USB mode)",
             s_ch422g_shadow);

    ESP_RETURN_ON_ERROR(
        ch422g_write_shadow(),
        TAG,
        "failed to initialize CH422G"
    );

    /*
     * Give the panel power rail time to stabilize while LCD reset remains
     * asserted.
     */
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "releasing LCD reset");

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_LCD_RESET, true),
        TAG,
        "failed to release LCD reset"
    );

    vTaskDelay(pdMS_TO_TICKS(120));

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* RGB LCD                                                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t waveshare_rgb_init(void)
{
    ESP_LOGI(TAG,
             "creating %ux%u RGB565 panel @ %.2f MHz",
             DT_LCD_H_RES,
             DT_LCD_V_RES,
             DT_LCD_PCLK_HZ / 1000000.0);

    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,

        .timings = {
            .pclk_hz = DT_LCD_PCLK_HZ,
            .h_res = DT_LCD_H_RES,
            .v_res = DT_LCD_V_RES,

            .hsync_pulse_width = DT_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch  = DT_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = DT_LCD_HSYNC_FRONT_PORCH,

            .vsync_pulse_width = DT_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch  = DT_LCD_VSYNC_BACK_PORCH,
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
         * Two small internal-RAM bounce buffers allow EDMA to stream the
         * PSRAM framebuffer more reliably.
         */
        .bounce_buffer_size_px = DT_LCD_H_RES * 10,

        .psram_trans_align = 64,

        .hsync_gpio_num = DT_LCD_HSYNC_GPIO,
        .vsync_gpio_num = DT_LCD_VSYNC_GPIO,
        .de_gpio_num = DT_LCD_DE_GPIO,
        .pclk_gpio_num = DT_LCD_PCLK_GPIO,

        /*
         * Display enable is controlled externally through the CH422G.
         */
        .disp_gpio_num = -1,

        /*
         * D0..D15 = B0..B4, G0..G5, R0..R4
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

    ESP_RETURN_ON_ERROR(
        esp_lcd_new_rgb_panel(&config, &s_panel),
        TAG,
        "esp_lcd_new_rgb_panel failed"
    );

    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_init(s_panel),
        TAG,
        "esp_lcd_panel_init failed"
    );

    ESP_LOGI(TAG, "RGB panel initialized");

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* Test pattern                                                               */
/* -------------------------------------------------------------------------- */

static esp_err_t waveshare_draw_test_pattern(void)
{
    /*
     * Draw through esp_lcd_panel_draw_bitmap() rather than writing the
     * driver-owned PSRAM framebuffer directly. This lets the RGB driver
     * perform the required framebuffer/cache handling.
     *
     * Pattern:
     *
     * +---------+---------+---------+---------+
     * |   RED   |  GREEN  |  BLUE   |  WHITE  |
     * +---------+---------+---------+---------+
     */

    uint16_t line[DT_LCD_H_RES];

    const unsigned quarter = DT_LCD_H_RES / 4;

    for (unsigned x = 0; x < DT_LCD_H_RES; ++x) {
        if (x < quarter) {
            line[x] = 0xF800;       /* red */
        } else if (x < quarter * 2) {
            line[x] = 0x07E0;       /* green */
        } else if (x < quarter * 3) {
            line[x] = 0x001F;       /* blue */
        } else {
            line[x] = 0xFFFF;       /* white */
        }
    }

    ESP_LOGI(TAG, "drawing RGB565 color-bar test pattern");

    for (unsigned y = 0; y < DT_LCD_V_RES; ++y) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(
            s_panel,
            0,
            y,
            DT_LCD_H_RES,
            y + 1,
            line
        );

        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "draw failed at row %u: %s",
                     y,
                     esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* Public bring-up                                                            */
/* -------------------------------------------------------------------------- */

esp_err_t dt_board_waveshare_7_display_test_init(void)
{
    ESP_LOGI(TAG, "Waveshare display bring-up begin");

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
        waveshare_draw_test_pattern(),
        TAG,
        "test pattern failed"
    );

    /*
     * Backlight is the final step. Until this point the panel remains dark,
     * preventing uninitialized framebuffer data from being displayed.
     */
    ESP_LOGI(TAG, "enabling LCD backlight");

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_LCD_BACKLIGHT, true),
        TAG,
        "failed to enable LCD backlight"
    );

    ESP_LOGI(TAG, "Waveshare display bring-up complete");

    return ESP_OK;
}

#endif
