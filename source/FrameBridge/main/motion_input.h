#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Original controller counts, in wire-axis order. Each hand has its own wrapping
// 16 MHz clock, as measured against the ESP32 receive times in both captures.
typedef struct {
    int32_t accel[3], gyro[3];
    uint32_t timestamp;
} motion_sample;

// Return the last sample in a complete TLV notification. Leave out untouched on
// a missing sample or damaged notification, even if a valid sample preceded it.
bool motion_read(const uint8_t *packet, size_t length, motion_sample *out);
