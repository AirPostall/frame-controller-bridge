#pragma once
#include "decoder.h"

// Register once after nimble_port_init(), before starting the host.
int ble_hid_register(void);
uint16_t ble_hid_input_handle(void);
uint16_t ble_hid_enhanced_handle(void);

// Called on the NimBLE host queue for a subscribed, encrypted output host.
int ble_hid_notify(uint16_t conn, const pad_report *report);
