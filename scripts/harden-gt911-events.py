#!/usr/bin/env python3

from pathlib import Path
import shutil
import sys
import time

PATH = Path("components/dt_board/dt_board_waveshare_7.c")

if not PATH.exists():
    sys.exit(f"ERROR: {PATH} missing")

src = PATH.read_text()

if "GT911 buffered edge queue active" in src:
    print("GT911 event hardening already applied.")
    sys.exit(0)

required = [
    "static void gt911_poll_task(void *arg)",
    "static void lvgl_touch_read_cb(",
    "static portMUX_TYPE s_touch_lock",
    ".num_fbs = 1,",
    ".bounce_buffer_size_px = (DT_LCD_H_RES * 10),",
    ".bb_mode = true,",
    ".avoid_tearing = false,",
]

for marker in required:
    if marker not in src:
        sys.exit(
            f"ERROR: expected stable-baseline marker missing:\n"
            f"  {marker}\n"
            "No changes made."
        )


def find_function(text, signature):
    start = text.find(signature)

    if start < 0:
        raise RuntimeError(f"Function not found: {signature}")

    brace = text.find("{", start)

    if brace < 0:
        raise RuntimeError(f"Opening brace missing: {signature}")

    depth = 0
    i = brace

    in_string = False
    in_char = False
    in_line = False
    in_block = False
    escape = False

    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ""

        if in_line:
            if c == "\n":
                in_line = False
            i += 1
            continue

        if in_block:
            if c == "*" and n == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue

        if in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == "'":
                in_char = False
            i += 1
            continue

        if c == "/" and n == "/":
            in_line = True
            i += 2
            continue

        if c == "/" and n == "*":
            in_block = True
            i += 2
            continue

        if c == '"':
            in_string = True
            i += 1
            continue

        if c == "'":
            in_char = True
            i += 1
            continue

        if c == "{":
            depth += 1

        elif c == "}":
            depth -= 1

            if depth == 0:
                return start, i + 1

        i += 1

    raise RuntimeError(f"Closing brace missing: {signature}")


def replace_function(text, signature, replacement):
    a, b = find_function(text, signature)
    return text[:a] + replacement + text[b:]


# ------------------------------------------------------------
# Safety backup
# ------------------------------------------------------------

stamp = time.strftime("%Y%m%d-%H%M%S")

backup = (
    Path("backups")
    / f"pre-gt911-event-buffer-{stamp}"
    / PATH
)

backup.parent.mkdir(parents=True, exist_ok=True)
shutil.copy2(PATH, backup)

print(f"Backup: {backup}")


# ------------------------------------------------------------
# Add buffered transition queue.
# ------------------------------------------------------------

anchor = "static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;\n"

addition = r'''
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

'''

if anchor not in src:
    sys.exit("ERROR: touch-lock anchor not found")

src = src.replace(
    anchor,
    anchor + addition,
    1
)


# ------------------------------------------------------------
# Replace GT911 acquisition task.
#
# Important changes:
# - queue DOWN edge
# - update coordinates continuously
# - do not trust one zero-contact packet as immediate UP
# - require 30 ms without another touch report before UP
# ------------------------------------------------------------

new_poll = r'''static void gt911_poll_task(void *arg)
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
}'''

try:
    src = replace_function(
        src,
        "static void gt911_poll_task(void *arg)",
        new_poll
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


# ------------------------------------------------------------
# Replace LVGL input callback.
#
# Buffered physical transitions are always delivered before the current
# level state. continue_reading tells LVGL to immediately consume the
# next queued transition rather than waiting another 10 ms.
# ------------------------------------------------------------

new_read = r'''static void lvgl_touch_read_cb(
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
}'''

try:
    src = replace_function(
        src,
        "static void lvgl_touch_read_cb(",
        new_read
    )
except RuntimeError as e:
    sys.exit(f"ERROR: {e}")


PATH.write_text(src)

print(f"Patched: {PATH}")
print()
print("RGB configuration: UNCHANGED")
print()
print("GT911 changes:")
print("  - ordered press/release event queue")
print("  - LVGL buffered-reading support")
print("  - 30 ms release debounce")
print("  - coordinate movement still updates continuously")
print("  - 10 ms polling remains unchanged")
