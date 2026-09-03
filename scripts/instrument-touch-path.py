#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

P = Path("components/dt_board/dt_board_waveshare_7.c")

if not P.exists():
    sys.exit(f"ERROR: missing {P}")

src = P.read_text()

if "TOUCH_DIAG irq=" in src:
    print("Touch instrumentation already installed.")
    sys.exit(0)

required = [
    "static void gt911_poll_task(void *arg)",
    "static void lvgl_touch_read_cb(",
    "touch_event_push_locked",
    "static lv_indev_t *s_lv_touch",
    ".num_fbs = 1,",
    ".bb_mode = true,",
]

for marker in required:
    if marker not in src:
        sys.exit(f"ERROR: expected marker missing: {marker}")

stamp = time.strftime("%Y%m%d-%H%M%S")
backup = (
    Path("backups")
    / f"pre-touch-instrumentation-{stamp}"
    / P
)
backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(P, backup)

print(f"Backup: {backup}")


# ----------------------------------------------------------
# Diagnostic counters + GPIO4 IRQ observer.
#
# This ISR does NOT wake LVGL and does NOT alter touch input.
# It only counts falling edges on the physical GT911 IRQ pin.
# ----------------------------------------------------------

anchor = "static lv_indev_t *s_lv_touch;"

addition = r'''
static volatile uint32_t s_diag_irq;
static volatile uint32_t s_diag_raw_down;
static volatile uint32_t s_diag_queue_down;
static volatile uint32_t s_diag_queue_up;
static volatile uint32_t s_diag_read_down;
static volatile uint32_t s_diag_read_up;
static volatile uint32_t s_diag_lv_pressed;
static volatile uint32_t s_diag_lv_clicked;
static volatile uint32_t s_diag_lv_released;
static volatile uint32_t s_diag_lv_press_lost;
static volatile uint32_t s_diag_i2c_errors;


static void IRAM_ATTR gt911_diag_irq_isr(void *arg)
{
    (void)arg;
    s_diag_irq++;
}


static void gt911_diag_lv_event(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            s_diag_lv_pressed++;
            break;

        case LV_EVENT_CLICKED:
            s_diag_lv_clicked++;
            break;

        case LV_EVENT_RELEASED:
            s_diag_lv_released++;
            break;

        case LV_EVENT_PRESS_LOST:
            s_diag_lv_press_lost++;
            break;

        default:
            break;
    }
}


static void gt911_diag_task(void *arg)
{
    (void)arg;

    uint32_t last_irq = UINT32_MAX;
    uint32_t last_raw = UINT32_MAX;
    uint32_t last_qd = UINT32_MAX;
    uint32_t last_qu = UINT32_MAX;
    uint32_t last_rd = UINT32_MAX;
    uint32_t last_ru = UINT32_MAX;
    uint32_t last_lp = UINT32_MAX;
    uint32_t last_lc = UINT32_MAX;
    uint32_t last_lr = UINT32_MAX;
    uint32_t last_ll = UINT32_MAX;
    uint32_t last_err = UINT32_MAX;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (
            last_irq != s_diag_irq ||
            last_raw != s_diag_raw_down ||
            last_qd != s_diag_queue_down ||
            last_qu != s_diag_queue_up ||
            last_rd != s_diag_read_down ||
            last_ru != s_diag_read_up ||
            last_lp != s_diag_lv_pressed ||
            last_lc != s_diag_lv_clicked ||
            last_lr != s_diag_lv_released ||
            last_ll != s_diag_lv_press_lost ||
            last_err != s_diag_i2c_errors
        ) {
            ESP_LOGI(
                TAG,
                "TOUCH_DIAG irq=%u raw=%u "
                "queue=%u/%u read=%u/%u "
                "lv=P%u C%u R%u L%u i2cerr=%u",
                (unsigned)s_diag_irq,
                (unsigned)s_diag_raw_down,
                (unsigned)s_diag_queue_down,
                (unsigned)s_diag_queue_up,
                (unsigned)s_diag_read_down,
                (unsigned)s_diag_read_up,
                (unsigned)s_diag_lv_pressed,
                (unsigned)s_diag_lv_clicked,
                (unsigned)s_diag_lv_released,
                (unsigned)s_diag_lv_press_lost,
                (unsigned)s_diag_i2c_errors
            );

            last_irq = s_diag_irq;
            last_raw = s_diag_raw_down;
            last_qd = s_diag_queue_down;
            last_qu = s_diag_queue_up;
            last_rd = s_diag_read_down;
            last_ru = s_diag_read_up;
            last_lp = s_diag_lv_pressed;
            last_lc = s_diag_lv_clicked;
            last_lr = s_diag_lv_released;
            last_ll = s_diag_lv_press_lost;
            last_err = s_diag_i2c_errors;
        }
    }
}

'''

src = src.replace(anchor, anchor + "\n" + addition, 1)


# Count valid raw GT911 contacts.
needle = '''if (
                count > 0 &&
                count <= GT911_MAX_POINTS
            ) {'''

replace = needle + '''
                s_diag_raw_down++;'''

if needle not in src:
    sys.exit("ERROR: GT911 valid-contact block not found")

src = src.replace(needle, replace, 1)


# Count queue transitions. These phrases exist in the buffered
# implementation we restored.
needle = '''touch_event_push_locked(
                            x,
                            y,
                            true
                        );'''

replace = needle + '''
                        s_diag_queue_down++;'''

if needle not in src:
    sys.exit("ERROR: queued DOWN not found")

src = src.replace(needle, replace, 1)


needle = '''touch_event_push_locked(
                    s_touch_x,
                    s_touch_y,
                    false
                );'''

replace = needle + '''
                s_diag_queue_up++;'''

if needle not in src:
    sys.exit("ERROR: queued UP not found")

src = src.replace(needle, replace, 1)


# Count actual buffered events handed to LVGL.
needle = '''if (!have_event) {
        event.x = s_touch_x;'''

replace = '''if (have_event) {
        if (event.pressed) {
            s_diag_read_down++;
        } else {
            s_diag_read_up++;
        }
    }

    if (!have_event) {
        event.x = s_touch_x;'''

if needle not in src:
    sys.exit("ERROR: LVGL buffered read block not found")

src = src.replace(needle, replace, 1)


# Count I2C failures.
needle = '''if (err != ESP_OK) {
            read_errors++;'''

replace = '''if (err != ESP_OK) {
            read_errors++;
            s_diag_i2c_errors++;'''

if needle not in src:
    print("WARNING: I2C error counter block not found")
else:
    src = src.replace(needle, replace, 1)


# Attach LVGL event observer.
needle = '''lv_indev_set_read_cb(
        s_lv_touch,
        lvgl_touch_read_cb
    );'''

replace = needle + '''

    /*
     * Diagnostic only: observe LVGL's interpretation of the
     * pointer sequence.
     */
    lv_indev_add_event_cb(
        s_lv_touch,
        gt911_diag_lv_event,
        LV_EVENT_ALL,
        NULL
    );'''

if needle not in src:
    sys.exit("ERROR: LVGL indev setup not found")

src = src.replace(needle, replace, 1)


# Start passive GPIO4 IRQ observer and statistics task.
#
# Place this immediately before the normal GT911 acquisition
# task is created so it cannot affect initialization ordering.
needle = '''if (s_touch_task == NULL) {'''

replace = r'''
    /*
     * Passive GT911 IRQ instrumentation.
     *
     * GPIO4 is still NOT used to drive LVGL. It only lets us
     * compare physical GT911 interrupts with the polling path.
     */
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLUP_ONLY);
    gpio_set_intr_type(GPIO_NUM_4, GPIO_INTR_NEGEDGE);

    esp_err_t diag_isr_result =
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM);

    if (
        diag_isr_result == ESP_OK ||
        diag_isr_result == ESP_ERR_INVALID_STATE
    ) {
        esp_err_t add_result =
            gpio_isr_handler_add(
                GPIO_NUM_4,
                gt911_diag_irq_isr,
                NULL
            );

        if (add_result != ESP_OK) {
            ESP_LOGW(
                TAG,
                "GT911 diagnostic IRQ handler: %s",
                esp_err_to_name(add_result)
            );
        }
    } else {
        ESP_LOGW(
            TAG,
            "GT911 diagnostic ISR service: %s",
            esp_err_to_name(diag_isr_result)
        );
    }

    xTaskCreatePinnedToCore(
        gt911_diag_task,
        "touch_diag",
        4096,
        NULL,
        1,
        NULL,
        1
    );

    ESP_LOGI(
        TAG,
        "GT911 end-to-end touch instrumentation enabled"
    );

''' + needle

if needle not in src:
    sys.exit("ERROR: GT911 task creation block not found")

src = src.replace(needle, replace, 1)

P.write_text(src)

print(f"Patched: {P}")
print()
print("NO RGB settings changed.")
print("NO GT911 behavior changed.")
print("Added passive counters for:")
print("  IRQ -> raw -> queue -> LVGL read -> LVGL events")
