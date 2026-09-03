#!/usr/bin/env python3

from pathlib import Path
import sys

path = Path("components/dt_board/dt_board_waveshare_7.c")

if not path.exists():
    sys.exit(f"ERROR: {path} not found")

src = path.read_text()

if "GT911 raw touch monitor active" in src:
    print("GT911 raw-touch patch already applied.")
    sys.exit(0)

# ---------------------------------------------------------------------
# Add GPIO include
# ---------------------------------------------------------------------

anchor = '#include "driver/i2c.h"\n'

if anchor not in src:
    sys.exit("ERROR: Could not find driver/i2c.h include anchor")

src = src.replace(
    anchor,
    '#include "driver/i2c.h"\n'
    '#include "driver/gpio.h"\n',
    1
)

# ---------------------------------------------------------------------
# Add GT911 register definitions
# ---------------------------------------------------------------------

anchor = 'static const char *TAG = "dt_waveshare_7";\n'

if anchor not in src:
    sys.exit("ERROR: Could not find TAG declaration anchor")

definitions = r'''
#define GT911_I2C_ADDR        0x5D

#define GT911_REG_PRODUCT_ID  0x8140
#define GT911_REG_STATUS      0x814E
#define GT911_REG_POINTS      0x814F

#define GT911_MAX_POINTS      5
'''

src = src.replace(
    anchor,
    anchor + definitions + "\n",
    1
)

# ---------------------------------------------------------------------
# Add GT911 implementation immediately before RGB LCD section
# ---------------------------------------------------------------------

anchor = '''/* -------------------------------------------------------------------------- */
/* RGB LCD                                                                    */
/* -------------------------------------------------------------------------- */
'''

if anchor not in src:
    sys.exit("ERROR: Could not find RGB LCD section anchor")

gt911_code = r'''
/* -------------------------------------------------------------------------- */
/* GT911                                                                      */
/* -------------------------------------------------------------------------- */

static esp_err_t gt911_read(
    uint16_t reg,
    uint8_t *data,
    size_t len
)
{
    uint8_t reg_addr[2] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
    };

    return i2c_master_write_read_device(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        GT911_I2C_ADDR,
        reg_addr,
        sizeof(reg_addr),
        data,
        len,
        pdMS_TO_TICKS(100)
    );
}


static esp_err_t gt911_write_u8(
    uint16_t reg,
    uint8_t value
)
{
    uint8_t buf[3] = {
        (uint8_t)(reg >> 8),
        (uint8_t)(reg & 0xFF),
        value,
    };

    return i2c_master_write_to_device(
        (i2c_port_t)DT_TOUCH_I2C_PORT,
        GT911_I2C_ADDR,
        buf,
        sizeof(buf),
        pdMS_TO_TICKS(100)
    );
}


static esp_err_t gt911_reset_select_address(void)
{
    ESP_LOGI(TAG, "resetting GT911 and selecting I2C address 0x5D");

    /*
     * GT911 address selection:
     *
     * INT LOW during reset -> 0x5D
     * INT HIGH during reset -> 0x14
     */

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
        "failed to configure GT911 INT"
    );

    ESP_RETURN_ON_ERROR(
        gpio_set_level(DT_TOUCH_INTERRUPT_GPIO, 0),
        TAG,
        "failed to drive GT911 INT low"
    );

    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_TOUCH_RESET, false),
        TAG,
        "failed to assert GT911 reset"
    );

    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(
        ch422g_set(DT_IOEXP_TOUCH_RESET, true),
        TAG,
        "failed to release GT911 reset"
    );

    vTaskDelay(pdMS_TO_TICKS(10));

    int_cfg.mode = GPIO_MODE_INPUT;

    ESP_RETURN_ON_ERROR(
        gpio_config(&int_cfg),
        TAG,
        "failed to return GT911 INT to input"
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
        "GT911 did not respond at 0x5D"
    );

    ESP_LOGI(
        TAG,
        "GT911 detected: product ID '%c%c%c%c'",
        product_id[0],
        product_id[1],
        product_id[2],
        product_id[3]
    );

    return ESP_OK;
}


static void gt911_touch_task(void *arg)
{
    (void)arg;

    uint8_t status = 0;
    uint8_t point_data[GT911_MAX_POINTS * 8];

    ESP_LOGI(TAG, "GT911 raw touch monitor active");

    while (true) {
        esp_err_t err = gt911_read(
            GT911_REG_STATUS,
            &status,
            1
        );

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "GT911 status read failed: %s",
                esp_err_to_name(err)
            );

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if ((status & 0x80) != 0) {
            uint8_t count = status & 0x0F;

            if (count > GT911_MAX_POINTS) {
                ESP_LOGW(
                    TAG,
                    "GT911 invalid touch count: %u",
                    count
                );

            } else if (count > 0) {
                size_t bytes = count * 8;

                err = gt911_read(
                    GT911_REG_POINTS,
                    point_data,
                    bytes
                );

                if (err == ESP_OK) {
                    for (uint8_t i = 0; i < count; ++i) {
                        const uint8_t *p = &point_data[i * 8];

                        uint8_t track_id = p[0];

                        uint16_t x =
                            (uint16_t)p[1] |
                            ((uint16_t)p[2] << 8);

                        uint16_t y =
                            (uint16_t)p[3] |
                            ((uint16_t)p[4] << 8);

                        uint16_t strength =
                            (uint16_t)p[5] |
                            ((uint16_t)p[6] << 8);

                        ESP_LOGI(
                            TAG,
                            "TOUCH id=%u x=%u y=%u strength=%u",
                            track_id,
                            x,
                            y,
                            strength
                        );
                    }

                } else {
                    ESP_LOGW(
                        TAG,
                        "GT911 coordinate read failed: %s",
                        esp_err_to_name(err)
                    );
                }
            }

            err = gt911_write_u8(
                GT911_REG_STATUS,
                0
            );

            if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "GT911 status clear failed: %s",
                    esp_err_to_name(err)
                );
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}


static esp_err_t waveshare_touch_test_init(void)
{
    ESP_RETURN_ON_ERROR(
        gt911_reset_select_address(),
        TAG,
        "GT911 reset/address selection failed"
    );

    ESP_RETURN_ON_ERROR(
        gt911_probe(),
        TAG,
        "GT911 probe failed"
    );

    BaseType_t task_result = xTaskCreate(
        gt911_touch_task,
        "gt911_test",
        4096,
        NULL,
        5,
        NULL
    );

    if (task_result != pdPASS) {
        ESP_LOGE(TAG, "failed to start GT911 touch task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


'''

src = src.replace(
    anchor,
    gt911_code + anchor,
    1
)

# ---------------------------------------------------------------------
# Start touch test after the display/backlight have been initialized
# ---------------------------------------------------------------------

anchor = '    ESP_LOGI(TAG, "Waveshare display bring-up complete");'

if anchor not in src:
    sys.exit("ERROR: Could not find display completion anchor")

touch_start = r'''
    ESP_LOGI(TAG, "starting GT911 touch test");

    ESP_RETURN_ON_ERROR(
        waveshare_touch_test_init(),
        TAG,
        "GT911 touch test initialization failed"
    );

'''

src = src.replace(
    anchor,
    touch_start + anchor,
    1
)

# ---------------------------------------------------------------------
# Write only after every anchor succeeded
# ---------------------------------------------------------------------

backup = path.with_suffix(".c.pre-gt911")

if not backup.exists():
    backup.write_text(path.read_text())
    print(f"Backup created: {backup}")

path.write_text(src)

print(f"Patched: {path}")
print("GT911 raw-touch test added successfully.")
