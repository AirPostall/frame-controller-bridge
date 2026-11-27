#include "bridge.h"
#include "pairing.h"
#include "ble_hid.h"
#include "ble_host.h"
#include "esp_timer.h"
#include "report_queue.h"
#include "host/ble_hs.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

enum { WAKE_USB = 1, WAKE_BLE = 2 };
enum { MOTION_TIMEOUT_US = 250000 };
static esp_timer_handle_t motion_timer;
static int32_t gyro_zero[3];
static bool gyro_sample_muted;
static bridge_status status;
static pad_profile profile;
static report_queue usb_queue, ble_queue;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
// Serialize USB submissions with mode changes without calling TinyUSB in a critical section.
static SemaphoreHandle_t usb_submit_lock;
static TaskHandle_t tx_task;
static uint16_t ble_conn = BLE_HS_CONN_HANDLE_NONE;
static struct ble_npl_event ble_tx_event;
static struct ble_npl_callout ble_retry;
static bool ble_tx_initialized;

static const pad_report neutral = {.bytes = {[2] = 8}}; // Centered sticks, hat in its null position.

static const pad_report *output_locked(bool bluetooth)
{
    return status.bluetooth_mode == bluetooth ? &status.output : &neutral;
}

// Apply the saved zero or mute only to outgoing angular rates. The raw sample
// remains available for setup, zero capture and accelerometer passthrough.
static void apply_gyro_locked(pad_report *out)
{
    if (!out->motion_valid) return;
    if (gyro_sample_muted) {
        memset(out->motion.gyro, 0, sizeof(out->motion.gyro));
    } else {
        for (unsigned axis = 0; axis < 3; ++axis) out->motion.gyro[axis] -= gyro_zero[axis];
    }
}

void bridge_get_output(bool bluetooth, pad_report *out)
{
    portENTER_CRITICAL(&lock);
    *out = *output_locked(bluetooth);
    apply_gyro_locked(out);
    portEXIT_CRITICAL(&lock);
}

static unsigned update_locked(void)
{
    pad_report next = pad_compose(status.hands, &profile);
    unsigned wake = 0;
    // Only compare controls here. Fast sensor updates have a separate, coalescing path.
    size_t pro3_controls = offsetof(pad_report, motion_valid);
    size_t usb_size = pairing.usb_layout == USB_LAYOUT_PRO3 ? pro3_controls : sizeof(next.bytes);
    if (!status.bluetooth_mode && status.usb && memcmp(&next, &status.output, usb_size)) {
        if (report_queue_push(&usb_queue, &next)) ++status.overflows;
        wake |= WAKE_USB;
    }
    if (status.bluetooth_mode && ble_conn != BLE_HS_CONN_HANDLE_NONE && memcmp(&next, &status.output, pro3_controls)) {
        if (report_queue_push(&ble_queue, &next)) ++status.ble_overflows;
        wake |= WAKE_BLE;
    }
    memcpy(status.output.bytes, next.bytes, sizeof(next.bytes));
    memcpy(status.output.trigger_level, next.trigger_level, sizeof(next.trigger_level));
    return wake;
}

static unsigned motion_wake_locked(void)
{
    if (!status.bluetooth_mode && status.usb && pairing.usb_layout == USB_LAYOUT_PRO3) {
        return report_queue_request_sample(&usb_queue, &status.output) ? WAKE_USB : 0;
    }
    if (status.bluetooth_mode && ble_conn != BLE_HS_CONN_HANDLE_NONE && ble_host_motion_ready()) {
        return report_queue_request_sample(&ble_queue, &status.output) ? WAKE_BLE : 0;
    }
    return 0;
}

static bool binding_active(unsigned binding, unsigned hand,
                           unsigned buttons, unsigned travel)
{
    if (!binding) return false;
    unsigned source = binding - 1;
    if (source < CLICK_SOURCES)
        return source / 16 == hand && (buttons & (1u << (source % 16)));
    source -= CLICK_SOURCES;
    return source / TRAVEL_SLOTS == hand && (travel & (1u << (source % TRAVEL_SLOTS)));
}

static unsigned update_gyro_hold_locked(unsigned wake)
{
    bool held = false;
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        const hand_state *hand = &status.hands[h];
        if (status.connected[h] && binding_active(profile.gyro_hold_button, h,
                                                   hand->buttons, hand->pressed_travel)) {
            held = true;
            break;
        }
    }
    if (held == status.gyro_held) return wake;
    status.gyro_held = held;
    // On release keep the last repositioning sample muted until a fresh one arrives.
    if (held) gyro_sample_muted = true;
    if (!wake && status.output.motion_valid) wake = motion_wake_locked();
    return wake;
}

static unsigned clear_motion_locked(void)
{
    if (!status.output.motion_valid) return 0;
    status.output.motion_valid = false;
    status.output.motion = (motion_sample){0};
    status.output.motion_tick_us = (uint32_t)esp_timer_get_time();
    return motion_wake_locked();
}

static unsigned reset_source_locked(void)
{
    status.motion_samples = 0;
    status.motion_time_us = 0;
    status.gyro_zero_count = 0;
    memset(gyro_zero, 0, sizeof(gyro_zero));
    return clear_motion_locked();
}

static bool peek_locked(report_queue *queue, bool bluetooth, pad_report *r, uint32_t *seq)
{
    if (!report_queue_peek(queue, r, seq)) return false;
    const pad_report *live = output_locked(bluetooth);
    r->motion_valid = live->motion_valid;
    r->motion = live->motion;
    r->gyro_gain = live->gyro_gain;
    r->motion_tick_us = live->motion_tick_us;
    apply_gyro_locked(r);
    return true;
}

static void wake_transports(unsigned wake)
{
    if (wake & WAKE_USB) xTaskNotifyGive(tx_task);
    if (wake & WAKE_BLE) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &ble_tx_event);
}

static void expire_motion(void *arg)
{
    portENTER_CRITICAL(&lock);
    unsigned wake = 0;
    if (status.output.motion_valid && esp_timer_get_time() - status.motion_time_us >= MOTION_TIMEOUT_US)
        wake = clear_motion_locked();
    portEXIT_CRITICAL(&lock);
    wake_transports(wake);
}

static void transmit(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            uint32_t seq;
            pad_report r;
            xSemaphoreTake(usb_submit_lock, portMAX_DELAY);
            portENTER_CRITICAL(&lock);
            bool pending = peek_locked(&usb_queue, false, &r, &seq);
            portEXIT_CRITICAL(&lock);
            bool submitted = pending && usb_pad_send(&r);
            if (submitted) {
                portENTER_CRITICAL(&lock);
                report_queue_sent(&usb_queue, seq);
                portEXIT_CRITICAL(&lock);
            }
            xSemaphoreGive(usb_submit_lock);
            // The completion callback wakes us for the next queued report.
            if (!pending || submitted) break;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
        }
    }
}

void bridge_usb_online(bool online)
{
    portENTER_CRITICAL(&lock);
    status.usb = online;
    report_queue_sync(&usb_queue, online, output_locked(false));
    portEXIT_CRITICAL(&lock);
    xTaskNotifyGive(tx_task);
}

void bridge_usb_complete(void) { xTaskNotifyGive(tx_task); }

// BLE connection/CCCD changes and this drain all run on the NimBLE host queue.
// A reused handle cannot receive a queued report from the previous host.
void bridge_ble_flush(void)
{
    if (ble_conn == BLE_HS_CONN_HANDLE_NONE || ble_npl_callout_is_active(&ble_retry)) return;
    for (unsigned burst = 0; burst < 4; ++burst) {
        uint32_t seq;
        pad_report report;
        portENTER_CRITICAL(&lock);
        bool pending = peek_locked(&ble_queue, true, &report, &seq);
        portEXIT_CRITICAL(&lock);
        if (!pending) return;
        int rc = ble_hid_notify(ble_conn, &report);
        if (rc) {
            // Keep the head (including a final release) until the link accepts it.
            // A disconnect will clear it. For other persistent errors, GAP
            // maintenance can check again without running a tight retry loop.
            if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY || rc == BLE_HS_EAGAIN)
                ble_npl_callout_reset(&ble_retry, ble_npl_time_ms_to_ticks32(10));
            return;
        }
        portENTER_CRITICAL(&lock);
        report_queue_sent(&ble_queue, seq);
        portEXIT_CRITICAL(&lock);
    }
    // Yield to inbound hand reports and GAP events between short bursts.
    wake_transports(WAKE_BLE);
}

static void ble_tx_run(struct ble_npl_event *event)
{
    bridge_ble_flush();
}

void bridge_ble_online(uint16_t conn, bool ready)
{
    uint16_t next = ready ? conn : BLE_HS_CONN_HANDLE_NONE;
    if (ble_conn == next) return;
    if (!ble_tx_initialized) {
        struct ble_npl_eventq *events = nimble_port_get_dflt_eventq();
        ble_npl_event_init(&ble_tx_event, ble_tx_run, NULL);
        ESP_ERROR_CHECK(ble_npl_callout_init(&ble_retry, events, ble_tx_run, NULL) == 0 ?
                        ESP_OK : ESP_ERR_NO_MEM);
        ble_tx_initialized = true;
    }
    ble_npl_callout_stop(&ble_retry);
    portENTER_CRITICAL(&lock);
    ble_conn = next;
    report_queue_sync(&ble_queue, ready && status.bluetooth_mode, output_locked(true));
    portEXIT_CRITICAL(&lock);
    if (ready) wake_transports(WAKE_BLE);
}

void bridge_set_bluetooth_mode(bool enabled)
{
    // Both mode changes and BLE sends run on the NimBLE task. Wait out any USB
    // submission before replacing the old queues with a release and a fresh snapshot.
    xSemaphoreTake(usb_submit_lock, portMAX_DELAY);
    portENTER_CRITICAL(&lock);
    if (status.bluetooth_mode == enabled) {
        portEXIT_CRITICAL(&lock);
        xSemaphoreGive(usb_submit_lock);
        return;
    }
    status.bluetooth_mode = enabled;
    bool usb_online = status.usb;
    bool ble_online = ble_conn != BLE_HS_CONN_HANDLE_NONE;
    report_queue_sync(&usb_queue, usb_online, output_locked(false));
    report_queue_sync(&ble_queue, ble_online, output_locked(true));
    portEXIT_CRITICAL(&lock);
    xSemaphoreGive(usb_submit_lock);
    if (usb_online) xTaskNotifyGive(tx_task);
    if (ble_online) {
        ble_npl_callout_stop(&ble_retry);
        bridge_ble_flush(); // Send the neutral report while the departing host is still authorized.
    }
}

void bridge_connection(unsigned h, bool up)
{
    portENTER_CRITICAL(&lock);
    if (status.connected[h] != up) ++status.epoch[h];
    status.connected[h] = up;
    unsigned wake = 0;
    if (!up) {
        status.hands[h] = (hand_state){0};
        if (profile.motion_hand == h + 1) wake = reset_source_locked();
    }
    wake |= update_locked();
    wake = update_gyro_hold_locked(wake);
    portEXIT_CRITICAL(&lock);
    wake_transports(wake);
}

static void accept_sample_locked(const motion_sample *sample)
{
    pad_report *out = &status.output;
    int64_t now = esp_timer_get_time();
    // Both hands have independent 16 MHz, 32-bit clocks. Anchor the first sample
    // to board time, then preserve device intervals across counter wrap and BLE jitter.
    uint32_t ticks = sample->timestamp - out->motion.timestamp;
    if (out->motion_valid && now - status.motion_time_us < MOTION_TIMEOUT_US &&
        ticks < 16U * MOTION_TIMEOUT_US) {
        out->motion_tick_us += (ticks + (out->motion.timestamp & 15U)) / 16U;
    } else {
        out->motion_tick_us = (uint32_t)now;
    }
    out->motion = *sample;
    out->motion_valid = true;
    gyro_sample_muted = status.gyro_held;
    status.motion_time_us = now;
    ++status.motion_samples;
}

void bridge_receive(unsigned h, const uint8_t *bytes, size_t size)
{
    portENTER_CRITICAL(&lock);
    uint16_t old_touch = status.hands[h].active;
    uint32_t revision = status.hands[h].revision;
    uint8_t old_travel = status.hands[h].pressed_travel;
    uint16_t old_buttons = status.hands[h].buttons;
    uint8_t old_trigger = status.hands[h].raw[TRIGGER_RAW];
    bool had_press_report = status.hands[h].seen & 2;
    int16_t old_x = status.hands[h].x, old_y = status.hands[h].y;
    unsigned wake = 0;
    if (pad_decode(&status.hands[h], bytes, size)) {
        ++status.packets[h];
        bool fields_changed = status.hands[h].revision != revision;
        if (fields_changed) {
            pad_update_travel(&status.hands[h], profile.travel[h]);
            pad_update_touch(&status.hands[h], profile.touch[h]);
        }
        const hand_state *v = &status.hands[h];
        if (v->buttons != old_buttons ||
            ((profile.stick_flags & (1U << h)) && (v->x != old_x || v->y != old_y)) ||
            v->raw[TRIGGER_RAW] != old_trigger || (!had_press_report && (v->seen & 2)) ||
            v->active != old_touch || v->pressed_travel != old_travel) {
            wake = update_locked();
        }
        motion_sample sample;
        if (profile.motion_hand == h + 1 && status.connected[h] && motion_read(bytes, size, &sample)) {
            accept_sample_locked(&sample);
            // A control transition queued above already carries the latest sample at send time.
            if (!wake) wake = motion_wake_locked();
        }
        // The first button snapshot after reconnect establishes the initial state.
        // A combined click/IMU packet may carry the best reference for this press.
        if (had_press_report && status.connected[h] && status.output.motion_valid &&
            binding_active(profile.gyro_reset_button, h, v->buttons & ~old_buttons,
                           v->pressed_travel & ~old_travel) &&
            esp_timer_get_time() - status.motion_time_us < MOTION_TIMEOUT_US) {
            memcpy(gyro_zero, status.output.motion.gyro, sizeof(gyro_zero));
            ++status.gyro_zero_count;
            if (!wake) wake = motion_wake_locked();
        }
        // A release packet can include an IMU reading from the held interval.
        // Process this level transition after accepting the packet's sensor data.
        if (v->buttons != old_buttons || v->pressed_travel != old_travel)
            wake = update_gyro_hold_locked(wake);
    } else {
        ++status.bad;
    }
    portEXIT_CRITICAL(&lock);
    wake_transports(wake);
}

void bridge_get_status(bridge_status *out)
{
    portENTER_CRITICAL(&lock);
    *out = status;
    portEXIT_CRITICAL(&lock);
}

void bridge_get_profile(pad_profile *out)
{
    portENTER_CRITICAL(&lock);
    *out = profile;
    portEXIT_CRITICAL(&lock);
}

esp_err_t bridge_save(const pad_profile *p)
{
    if (!pad_profile_valid(p)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("framebridge", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "profile_v10", p, sizeof(*p));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&lock);
    uint8_t previous_hand = profile.motion_hand;
    unsigned wake = previous_hand != p->motion_hand ? reset_source_locked() : 0;
    if (profile.gyro_gain != p->gyro_gain && status.output.motion_valid) wake |= motion_wake_locked();
    status.motion_hand = p->motion_hand;
    status.output.gyro_gain = p->gyro_gain;
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        status.hands[h].active = 0;
        // A held trigger between its press and release thresholds stays held when
        // saving unrelated settings. Reevaluate changed calibrations from rest.
        for (unsigned i = 0; i < TRAVEL_SLOTS; ++i) {
            if (memcmp(&profile.travel[h][i], &p->travel[h][i], sizeof(press_binding)))
                status.hands[h].pressed_travel &= ~(1u << i);
        }
        pad_update_travel(&status.hands[h], p->travel[h]);
        pad_update_touch(&status.hands[h], p->touch[h]);
    }
    profile = *p;
    wake |= update_locked();
    wake = update_gyro_hold_locked(wake);
    portEXIT_CRITICAL(&lock);
    if (previous_hand == MOTION_OFF && p->motion_hand != MOTION_OFF)
        ESP_ERROR_CHECK(esp_timer_start_periodic(motion_timer, MOTION_TIMEOUT_US / 4));
    if (previous_hand != MOTION_OFF && p->motion_hand == MOTION_OFF)
        ESP_ERROR_CHECK(esp_timer_stop(motion_timer));
    wake_transports(wake);
    return ESP_OK;
}

static bool read_profile(nvs_handle_t nvs)
{
    // Read large blobs into permanent storage before invoking the migration code.
    // NVS and migration never need large buffers on the main task's stack together.
    static const struct { const char *key; unsigned version; size_t length; } formats[] = {
        {"profile_v10", 10, sizeof(profile)}, {"profile_v9", 9, V9_PROFILE_BYTES},
        {"profile_v8", 8, V8_PROFILE_BYTES}, {"profile_v7", 7, V7_PROFILE_BYTES},
        {"profile_v6", 6, V7_PROFILE_BYTES}, {"profile_v5", 5, V5_PROFILE_BYTES}
    };
    size_t size;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        size = formats[i].length;
        if (nvs_get_blob(nvs, formats[i].key, &profile, &size) == ESP_OK &&
            size == formats[i].length && profile.version == formats[i].version &&
            pad_profile_load(&profile, (const uint8_t *)&profile, size)) return true;
    }

    uint8_t bytes[V4_PROFILE_BYTES];
    const char *keys[] = {"profile_v4", "profile"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        size = sizeof(bytes);
        if (nvs_get_blob(nvs, keys[i], bytes, &size) != ESP_OK) continue;
        if (i == 0 && (size != V4_PROFILE_BYTES || bytes[0] != 4)) continue;
        if (pad_profile_load(&profile, bytes, size)) return true;
    }
    return false;
}

void bridge_init(void)
{
    bool loaded = false;
    nvs_handle_t nvs;
    if (nvs_open("framebridge", NVS_READONLY, &nvs) == ESP_OK) {
        loaded = read_profile(nvs);
        nvs_close(nvs);
    }
    if (!loaded) pad_defaults(&profile);
    status.motion_hand = profile.motion_hand;
    status.output = pad_compose(status.hands, &profile);
    status.output.gyro_gain = profile.gyro_gain;
    usb_submit_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(usb_submit_lock ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t created = xTaskCreate(transmit, "gamepad-tx", 3072, NULL, 6, &tx_task);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    const esp_timer_create_args_t timer_args = {.callback = expire_motion, .name = "motion-timeout",
                                                .skip_unhandled_events = true};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &motion_timer));
    if (profile.motion_hand != MOTION_OFF)
        ESP_ERROR_CHECK(esp_timer_start_periodic(motion_timer, MOTION_TIMEOUT_US / 4));
    usb_pad_init();
    setup_init();
}
