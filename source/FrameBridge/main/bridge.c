#include "bridge.h"
#include "pairing.h"
#include "ble_hid.h"
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

void bridge_get_output(bool bluetooth, pad_report *out)
{
    portENTER_CRITICAL(&lock);
    *out = *output_locked(bluetooth);
    portEXIT_CRITICAL(&lock);
}

static unsigned update_locked(void)
{
    pad_report next = pad_compose(status.hands, &profile);
    unsigned wake = 0;
    size_t usb_size = pairing.usb_layout == USB_LAYOUT_PRO3 ? sizeof(next) : sizeof(next.bytes);
    if (!status.bluetooth_mode && status.usb && memcmp(&next, &status.output, usb_size)) {
        if (report_queue_push(&usb_queue, &next)) ++status.overflows;
        wake |= WAKE_USB;
    }
    if (status.bluetooth_mode && ble_conn != BLE_HS_CONN_HANDLE_NONE && memcmp(&next, &status.output, sizeof(next))) {
        if (report_queue_push(&ble_queue, &next)) ++status.ble_overflows;
        wake |= WAKE_BLE;
    }
    status.output = next;
    return wake;
}

static void wake_transports(unsigned wake)
{
    if (wake & WAKE_USB) xTaskNotifyGive(tx_task);
    if (wake & WAKE_BLE) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &ble_tx_event);
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
            bool pending = report_queue_peek(&usb_queue, &r, &seq);
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
        bool pending = report_queue_peek(&ble_queue, &report, &seq);
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
    if (!up) status.hands[h] = (hand_state){0};
    unsigned wake = update_locked();
    portEXIT_CRITICAL(&lock);
    wake_transports(wake);
}

void bridge_receive(unsigned h, const uint8_t *bytes, size_t size)
{
    portENTER_CRITICAL(&lock);
    uint16_t old_touch = status.hands[h].active;
    uint32_t revision = status.hands[h].revision;
    uint8_t old_travel = status.hands[h].pressed_travel;
    uint16_t old_buttons = status.hands[h].buttons;
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
            (fields_changed && (profile.trigger_source[0] || profile.trigger_source[1])) ||
            v->active != old_touch || v->pressed_travel != old_travel) {
            wake = update_locked();
        }
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
    err = nvs_set_blob(nvs, "profile_v6", p, sizeof(*p));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&lock);
    profile = *p;
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        status.hands[h].active = 0;
        status.hands[h].pressed_travel = 0;
        pad_update_travel(&status.hands[h], p->travel[h]);
        pad_update_touch(&status.hands[h], p->touch[h]);
    }
    unsigned wake = update_locked();
    portEXIT_CRITICAL(&lock);
    wake_transports(wake);
    return ESP_OK;
}

static bool read_profile(nvs_handle_t nvs)
{
    // Read large blobs into permanent storage before invoking the migration code.
    // NVS and migration never need large buffers on the main task's stack together.
    size_t size = sizeof(profile);
    if (nvs_get_blob(nvs, "profile_v6", &profile, &size) == ESP_OK &&
        size == sizeof(profile) && pad_profile_valid(&profile)) return true;

    size = V5_PROFILE_BYTES;
    if (nvs_get_blob(nvs, "profile_v5", &profile, &size) == ESP_OK &&
        size == V5_PROFILE_BYTES && profile.version == 5 &&
        pad_profile_load(&profile, (const uint8_t *)&profile, size)) return true;

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
    status.output = pad_compose(status.hands, &profile);
    usb_submit_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(usb_submit_lock ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t created = xTaskCreate(transmit, "gamepad-tx", 3072, NULL, 6, &tx_task);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    usb_pad_init();
    setup_init();
}
