#pragma once
#include "decoder.h"

// HID transports supply the report ID separately. Enhanced input has a 31-byte payload.
enum {
    PRO3_INPUT_ID = 4, PRO3_CONTROL_BYTES = 10, PRO3_INPUT_PAYLOAD_SIZE = 31,
    PRO3_FEATURE_ID = 6, PRO3_FEATURE_SIZE = 13
};
// Readable vendor feature used by SDL's 8BitDo driver to discover the IMU report.
// The signed-integer motion fields do not require a separate sensor-enable write.
#define PRO3_FEATURE_DESCRIPTOR \
    0x06,0x00,0xff, 0x09,0x06, 0x85,PRO3_FEATURE_ID, 0x15,0x00, 0x26,0xff,0x00, \
    0x75,0x08, 0x95,PRO3_FEATURE_SIZE, 0xb1,0x02
size_t pro3_encode_controls(const pad_report *report, uint8_t out[PRO3_CONTROL_BYTES]);
size_t pro3_encode(const pad_report *report, uint8_t out[PRO3_INPUT_PAYLOAD_SIZE]);
extern const uint8_t pro3_feature[PRO3_FEATURE_SIZE];
