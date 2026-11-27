#pragma once
#include "pro3.h"

// Apple's com.8BitDo.Pro3.Bluetooth personality uses revision 1.00.
enum {
    BLE_PAD_VENDOR_ID = 0x2dc8, BLE_PAD_PRODUCT_ID = 0x6009, BLE_PAD_VERSION = 0x0100,
    BLE_PAD_INPUT_ID = PRO3_INPUT_ID, BLE_PAD_INPUT_SIZE = PRO3_CONTROL_BYTES,
    BLE_PAD_ENHANCED_ID = 1, BLE_PAD_ENHANCED_MTU = PRO3_INPUT_PAYLOAD_SIZE + 3,
};

extern const uint8_t ble_pad_report_map[];
extern const size_t ble_pad_report_map_size;
