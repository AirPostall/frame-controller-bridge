#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_HOST_OFF, BLE_HOST_PAIRING, BLE_HOST_RECONNECTING, BLE_HOST_CONNECTED, BLE_HOST_READY
} ble_host_state_t;

// BOOT toggles the output route. These APIs are safe outside the NimBLE task.
void ble_host_toggle_mode(void);
ble_host_state_t ble_host_state(void);

// Initialize before starting the NimBLE task. All remaining calls run on its event queue.
void ble_host_init(void);
void ble_host_tick(uint8_t own_addr_type);
void ble_host_reset(void);
bool ble_host_authorized(uint16_t conn);
void ble_host_suspend(uint16_t conn, bool suspended);
