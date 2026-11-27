#include "pro3.h"
#include <string.h>

static uint8_t stick_value(const uint8_t little_endian[2])
{
    int32_t value = (int16_t)(little_endian[0] | ((uint16_t)little_endian[1] << 8));
    // Enhanced USB reports use 0x7f for center, 0x00 and 0xff at the stops.
    if (value < 0) return 127 - ((-value * 127 + 16384) / 32768);
    return 127 + ((value * 128 + 16383) / 32767);
}

size_t pro3_encode_controls(const pad_report *report, uint8_t out[PRO3_CONTROL_BYTES])
{
    memset(out, 0, PRO3_CONTROL_BYTES);
    out[0] = report->bytes[2] < 8 ? report->bytes[2] : 8;
    for (unsigned i = 0; i < 4; ++i) {
        out[1 + i] = stick_value(&report->bytes[3 + i * 2]);
    }
    out[5] = report->trigger_level[1];  // Accelerator (RT)
    out[6] = report->trigger_level[0];  // Brake (LT)

    // Each entry is the 1-based Pro 3 HID Button usage for the corresponding
    // 1-based Frame Bridge USB button. The two Steam actions share Home.
    static const uint8_t usage[] = {
        1, 2, 4, 5,    // A, B, X, Y
        7, 8, 11, 12,  // LB, RB, Select, Start
        14, 15, 13,    // L3, R3, Home
        9, 10,         // LT, RT digital contacts
        6, 3, 13,      // PL, PR, right Steam
        17, 18,        // L4, R4
    };
    unsigned buttons = pad_usb_buttons(report);
    // Generic USB exposes the calibrated trigger thresholds as buttons. On Pro 3,
    // keep their digital contacts released while the corresponding axis is partial.
    for (unsigned trigger = 0; trigger < 2; ++trigger) {
        if (report->trigger_level[trigger] < 255) buttons &= ~(1U << (11 + trigger));
    }
    for (unsigned i = 0; i < sizeof(usage); ++i) {
        if (buttons & (1U << i)) {
            unsigned bit = usage[i] - 1;
            out[7 + bit / 8] |= 1U << (bit % 8);
        }
    }
    return PRO3_CONTROL_BYTES;
}

size_t pro3_encode(const pad_report *report, uint8_t out[PRO3_INPUT_PAYLOAD_SIZE])
{
    pro3_encode_controls(report, out);
    memset(out + PRO3_CONTROL_BYTES, 0, PRO3_INPUT_PAYLOAD_SIZE - PRO3_CONTROL_BYTES);
    return PRO3_INPUT_PAYLOAD_SIZE;
}
