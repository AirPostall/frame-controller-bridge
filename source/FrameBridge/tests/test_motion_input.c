#include "../main/motion_input.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    // Synthetic signed 24-bit values and controller ticks exercise the wire ordering.
    const uint8_t left[] = {0x1e, 22,
        0x00, 0x00, 0xff,  0x30, 0x12, 0x00,  0x00, 0x01, 0x00,
        0x20, 0x00, 0x01,  0xf0, 0xff, 0xff,  0x00, 0x00, 0x02,
        0x78, 0x56, 0x34, 0x12};
    const uint8_t right[] = {0x1e, 22,
        0x00, 0x10, 0x00,  0x00, 0xf0, 0xff,  0x10, 0x00, 0x03,
        0xe0, 0xff, 0xff,  0x40, 0x23, 0x01,  0x00, 0x00, 0xfe,
        0xdc, 0xfe, 0x98, 0xba};
    motion_sample sample = {0};
    assert(motion_read(left, sizeof(left), &sample));
    assert(sample.accel[0] == -65536 && sample.accel[1] == 0x1230 && sample.accel[2] == 256);
    assert(sample.gyro[0] == 0x10020 && sample.gyro[1] == -16 && sample.gyro[2] == 0x20000);
    assert(sample.timestamp == 0x12345678);

    uint8_t packet[sizeof(left) + sizeof(right) + 3] = {0x24, 1, 0xea};
    memcpy(packet + 3, left, sizeof(left));
    memcpy(packet + 3 + sizeof(left), right, sizeof(right));
    assert(motion_read(packet, sizeof(packet), &sample));
    assert(sample.accel[0] == 4096 && sample.accel[1] == -4096 && sample.accel[2] == 0x30010);
    assert(sample.gyro[0] == -32 && sample.gyro[1] == 0x12340 && sample.gyro[2] == -0x20000);
    assert(sample.timestamp == 0xba98fedc);
    motion_sample previous = sample;

    // An earlier sample must not escape a packet with a broken trailing field.
    assert(!motion_read(packet, sizeof(packet) - 1, &sample));
    assert(!motion_read(packet, 3 + sizeof(left) + 1, &sample));
    packet[3 + sizeof(left) + 1] = 21;
    assert(!motion_read(packet, sizeof(packet) - 1, &sample));
    assert(!motion_read(packet, 3, &sample));
    assert(!motion_read(NULL, 0, &sample));
    assert(!memcmp(&sample, &previous, sizeof(sample)));

    uint8_t limits[24] = {0x1e, 22, 0, 0, 0x80, 0xff, 0xff, 0x7f, 0xff, 0xff, 0xff};
    memset(limits + 20, 0xff, 4);
    assert(motion_read(limits, sizeof(limits), &sample));
    assert(sample.accel[0] == -8388608 && sample.accel[1] == 8388607 && sample.accel[2] == -1);
    assert(sample.timestamp == UINT32_MAX);
    return 0;
}
