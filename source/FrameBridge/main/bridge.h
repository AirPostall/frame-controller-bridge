#pragma once
#include "decoder.h"
#include "esp_err.h"

typedef struct {
    hand_state hands[HAND_COUNT];
    bool connected[HAND_COUNT], usb;
    uint32_t epoch[HAND_COUNT], packets[HAND_COUNT], bad, overflows;
    pad_profile profile;
    pad_report output;
} bridge_status;

void bridge_init(void);
void bridge_connection(unsigned hand, bool up);
void bridge_receive(unsigned hand, const uint8_t *bytes, size_t size);
void bridge_get_status(bridge_status *out);
esp_err_t bridge_save(const pad_profile *profile);
void bridge_usb_online(bool online);
void bridge_usb_complete(void);
void usb_pad_init(void);
bool usb_pad_send(const pad_report *report);
void setup_init(void);
