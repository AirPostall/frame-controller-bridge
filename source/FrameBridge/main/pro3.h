#pragma once
#include "decoder.h"

// TinyUSB supplies the report ID separately on both interrupt and control transfers.
enum { PRO3_INPUT_ID = 4, PRO3_INPUT_PAYLOAD_SIZE = 31 };
size_t pro3_encode(const pad_report *report, uint8_t out[PRO3_INPUT_PAYLOAD_SIZE]);
