#pragma once
#include "decoder.h"
#include "esp_err.h"

typedef struct {
    hand_state hands[HAND_COUNT];
    bool connected[HAND_COUNT], usb, bluetooth_mode, gyro_held;
    uint32_t epoch[HAND_COUNT], packets[HAND_COUNT], bad, overflows, ble_overflows;
    uint8_t motion_hand;
    uint32_t motion_samples, gyro_zero_count;
    int64_t motion_time_us;
    pad_report output;
} bridge_status;

void bridge_init(void);
void bridge_connection(unsigned hand, bool up);
void bridge_receive(unsigned hand, const uint8_t *bytes, size_t size);
void bridge_get_status(bridge_status *out);
void bridge_get_output(bool bluetooth, pad_report *out);
void bridge_get_profile(pad_profile *out);
esp_err_t bridge_save(const pad_profile *profile);
void bridge_usb_online(bool online);
void bridge_usb_complete(void);
// Invoke on the NimBLE host queue. Switch routes before closing the old BLE host.
void bridge_set_bluetooth_mode(bool enabled);
void bridge_ble_online(uint16_t conn, bool ready);
void bridge_ble_flush(void);
void usb_pad_init(void);
bool usb_pad_send(const pad_report *report);
void setup_init(void);
