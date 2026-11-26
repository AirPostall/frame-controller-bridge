#pragma once
#include "decoder.h"

// HID transports supply the report ID separately. The wired packet pads its controls to 31 bytes.
enum { PRO3_INPUT_ID = 4, PRO3_CONTROL_BYTES = 10, PRO3_INPUT_PAYLOAD_SIZE = 31 };
size_t pro3_encode_controls(const pad_report *report, uint8_t out[PRO3_CONTROL_BYTES]);
size_t pro3_encode(const pad_report *report, uint8_t out[PRO3_INPUT_PAYLOAD_SIZE]);
