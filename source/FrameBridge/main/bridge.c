#include "bridge.h"
#include "pairing.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

enum { QUEUE_SIZE = 32 };
static bridge_status status;
static pad_report reports[QUEUE_SIZE];
static uint32_t first, end;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t tx_task;

// Monotonic indices let a completion race safely with USB detach or queue overflow.
static void enqueue_locked(void)
{
    if (end - first == QUEUE_SIZE) {
        first = end;  // A stalled host should catch up to the latest physical state.
        ++status.overflows;
    }
    reports[end++ % QUEUE_SIZE] = status.output;
}

static bool update_locked(void)
{
    pad_report next = pad_compose(status.hands, &status.profile);
    // Other layouts only send the original 12 bytes. Keep analog levels visible in setup
    // without queueing identical USB reports for those hosts.
    size_t size = pairing.usb_layout == USB_LAYOUT_PRO3 ? sizeof(next) : sizeof(next.bytes);
    bool send = status.usb && memcmp(&next, &status.output, size) != 0;
    status.output = next;
    if (send) enqueue_locked();
    return send;
}

static void transmit(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            portENTER_CRITICAL(&lock);
            uint32_t seq = first;
            bool pending = status.usb && seq != end;
            pad_report r = pending ? reports[seq % QUEUE_SIZE] : status.output;
            portEXIT_CRITICAL(&lock);
            if (!pending) break;
            if (usb_pad_send(&r)) {
                portENTER_CRITICAL(&lock);
                if (first == seq) ++first;
                portEXIT_CRITICAL(&lock);
                // The completion callback wakes us for the next queued report.
                break;
            }
            // Only retry while a report is pending and the host endpoint is busy.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
        }
    }
}

void bridge_usb_online(bool online)
{
    portENTER_CRITICAL(&lock);
    status.usb = online;
    first = end;
    if (online) enqueue_locked();
    portEXIT_CRITICAL(&lock);
    xTaskNotifyGive(tx_task);
}

void bridge_usb_complete(void) { xTaskNotifyGive(tx_task); }

void bridge_connection(unsigned h, bool up)
{
    portENTER_CRITICAL(&lock);
    if (status.connected[h] != up) ++status.epoch[h];
    status.connected[h] = up;
    if (!up) status.hands[h] = (hand_state){0};
    bool send = update_locked();
    portEXIT_CRITICAL(&lock);
    if (send) xTaskNotifyGive(tx_task);
}

void bridge_receive(unsigned h, const uint8_t *bytes, size_t size)
{
    portENTER_CRITICAL(&lock);
    uint16_t old_touch = status.hands[h].active;
    uint32_t revision = status.hands[h].revision;
    uint8_t old_travel = status.hands[h].pressed_travel;
    uint16_t old_buttons = status.hands[h].buttons;
    int16_t old_x = status.hands[h].x, old_y = status.hands[h].y;
    bool send = false;
    if (pad_decode(&status.hands[h], bytes, size)) {
        ++status.packets[h];
        bool fields_changed = status.hands[h].revision != revision;
        if (fields_changed) {
            pad_update_travel(&status.hands[h], status.profile.travel[h]);
            pad_update_touch(&status.hands[h], status.profile.touch[h]);
        }
        const hand_state *v = &status.hands[h];
        if (v->buttons != old_buttons ||
            ((status.profile.stick_flags & (1U << h)) && (v->x != old_x || v->y != old_y)) ||
            (fields_changed && (status.profile.trigger_source[0] || status.profile.trigger_source[1])) ||
            v->active != old_touch || v->pressed_travel != old_travel) {
            send = update_locked();
        }
    } else {
        ++status.bad;
    }
    portEXIT_CRITICAL(&lock);
    if (send) xTaskNotifyGive(tx_task);
}

void bridge_get_status(bridge_status *out)
{
    portENTER_CRITICAL(&lock);
    *out = status;
    portEXIT_CRITICAL(&lock);
}

esp_err_t bridge_save(const pad_profile *p)
{
    if (!pad_profile_valid(p)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("framebridge", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "profile_v4", p, sizeof(*p));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&lock);
    status.profile = *p;
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        status.hands[h].active = 0;
        status.hands[h].pressed_travel = 0;
        pad_update_travel(&status.hands[h], p->travel[h]);
        pad_update_touch(&status.hands[h], p->touch[h]);
    }
    bool send = update_locked();
    portEXIT_CRITICAL(&lock);
    if (send) xTaskNotifyGive(tx_task);
    return ESP_OK;
}

void bridge_init(void)
{
    status.profile = pad_defaults();
    nvs_handle_t nvs;
    if (nvs_open("framebridge", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t saved[LEGACY_PROFILE_BYTES > sizeof(pad_profile) ? LEGACY_PROFILE_BYTES : sizeof(pad_profile)];
        const char *keys[] = {"profile_v4", "profile"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
            size_t size = sizeof(saved);
            if (nvs_get_blob(nvs, keys[i], saved, &size) != ESP_OK) continue;
            if (i == 0 && (size != sizeof(pad_profile) || saved[0] != PROFILE_VERSION)) continue;
            if (pad_profile_load(&status.profile, saved, size)) break;
        }
        nvs_close(nvs);
    }
    status.output = pad_compose(status.hands, &status.profile);
    BaseType_t created = xTaskCreate(transmit, "gamepad-tx", 3072, NULL, 6, &tx_task);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    usb_pad_init();
    setup_init();
}
