#include "motion_input.h"

static int32_t signed_le24(const uint8_t *p)
{
    int32_t value = p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    return value & 0x800000 ? value - 0x1000000 : value;
}

bool motion_read(const uint8_t *packet, size_t length, motion_sample *out)
{
    const uint8_t *sample = NULL;
    for (size_t offset = 0; offset < length;) {
        if (length - offset < 2) return false;
        uint8_t tag = packet[offset++], size = packet[offset++];
        if (size > length - offset) return false;
        if (tag == 0x1e) {
            if (size != 22) return false;
            sample = packet + offset;
        }
        offset += size;
    }
    if (!sample) return false;
    for (unsigned axis = 0; axis < 3; ++axis) {
        out->accel[axis] = signed_le24(sample + axis * 3);
        out->gyro[axis] = signed_le24(sample + 9 + axis * 3);
    }
    out->timestamp = (uint32_t)sample[18] | ((uint32_t)sample[19] << 8) |
                     ((uint32_t)sample[20] << 16) | ((uint32_t)sample[21] << 24);
    return true;
}
