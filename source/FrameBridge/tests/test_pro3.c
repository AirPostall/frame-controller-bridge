#include "pro3.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_packet(const pad_report *r, const uint8_t expected[PRO3_INPUT_PAYLOAD_SIZE])
{
    uint8_t guarded[PRO3_INPUT_PAYLOAD_SIZE + 2];
    memset(guarded, 0xee, sizeof(guarded));
    assert(pro3_encode(r, guarded + 1) == PRO3_INPUT_PAYLOAD_SIZE);
    assert(guarded[0] == 0xee && guarded[sizeof(guarded) - 1] == 0xee);
    assert(!memcmp(guarded + 1, expected, PRO3_INPUT_PAYLOAD_SIZE));
}

static void test_trigger_travel(void)
{
    pad_profile p;
    pad_defaults(&p);
    p.travel[1][0] = (press_binding){2, 191, 127};
    hand_state hands[2] = {0};
    uint8_t controls[PRO3_CONTROL_BYTES], wired[PRO3_INPUT_PAYLOAD_SIZE], generic[11];
    uint8_t left[] = {0x14,8,0,0,0,0,0,0,0,0};
    uint8_t right[] = {0x14,8,0,0,0,0,0,0,0,0};
    bool left_pressed = false, right_pressed = false;
    assert(pad_profile_valid(&p));
    for (int pass = 0; pass <= 1; ++pass) {
        for (int i = 0; i <= 255; ++i) {
            unsigned left_raw = pass ? 255 - i : i;
            unsigned right_raw = (left_raw + 71) % 256;
            // Vary bit 7 and byte 6 separately from the travel byte on both hands.
            left[2] = (i & 1) ? 0x80 : 0;
            right[2] = (i & 2) ? 0x80 : 0;
            left[8] = 255 - left_raw;
            right[8] = 255 - right_raw;
            left[9] = left_raw;
            right[9] = right_raw;
            assert(pad_decode(&hands[0], left, sizeof(left)));
            assert(pad_decode(&hands[1], right, sizeof(right)));
            for (unsigned h = 0; h < HAND_COUNT; ++h) pad_update_travel(&hands[h], p.travel[h]);
            pad_report r = pad_compose(hands, &p);
            assert(pro3_encode(&r, wired) == sizeof(wired));
            assert(pro3_encode_controls(&r, controls) == sizeof(controls));
            assert(!memcmp(wired, controls, sizeof(controls)));
            assert(controls[6] == left_raw && controls[5] == 255 - right_raw);
            assert(!!(controls[8] & 1) == (left_raw == 255));
            assert(!!(controls[8] & 2) == (right_raw == 0));
            pad_standard_report(&r, generic);
            if (left_raw >= 128) left_pressed = true;
            if (left_raw <= 64) left_pressed = false;
            if (right_raw <= 127) right_pressed = true;
            if (right_raw >= 191) right_pressed = false;
            assert(!!(generic[8] & 8) == left_pressed);
            assert(!!(generic[8] & 16) == right_pressed);
        }
    }
    // Disconnect a hand with falling polarity. Unseen travel must also release.
    hands[1] = (hand_state){0};
    left[9] = 75;
    assert(pad_decode(&hands[0], left, sizeof(left)));
    pad_update_travel(&hands[0], p.travel[0]);
    pad_report r = pad_compose(hands, &p);
    assert(r.trigger_level[0] == 75 && !r.trigger_level[1]);
}

static void test_motion_packet(void)
{
    pad_report report = {.bytes = {[2] = 8}, .trigger_level = {16, 32}, .motion_valid = true,
        .gyro_gain = 100, .motion_tick_us = 0xff81a273U,
        .motion = {.accel = {16384, -65536, 8388607}, .gyro = {-8388608, 65536, -4}}};
    uint8_t expected[PRO3_INPUT_PAYLOAD_SIZE] = {8,127,127,127,127,32,16};
    // Payload byte 14 is wire byte 15 after the report ID. Axes are little endian.
    const uint8_t axes[] = {0x00,0x10, 0x00,0xc0, 0xff,0x7f,
                            0x00,0x80, 0x00,0x40, 0xff,0xff};
    memcpy(expected + 14, axes, sizeof(axes));
    memcpy(expected + 26, (uint8_t[]){0x73,0xa2,0x81,0xff}, 4);
    assert(pro3_feature[12] == 0xaa);
    check_packet(&report, expected);
    report.gyro_gain = 250;
    expected[22] = 0xff;
    expected[23] = 0x7f;
    expected[24] = 0xfe;
    check_packet(&report, expected);
    // At maximum gain an extreme new sample minus its opposite zero point
    // exceeds a signed 32-bit intermediate but must saturate in the right direction.
    report.motion.gyro[0] = 16777215;
    report.motion.gyro[1] = -16777215;
    expected[20] = 0xff;
    expected[21] = 0x7f;
    expected[22] = 0x00;
    expected[23] = 0x80;
    check_packet(&report, expected);
    report.motion_valid = false;
    memset(expected + 14, 0, sizeof(axes));
    check_packet(&report, expected);
}

int main(void)
{
    test_motion_packet();
    test_trigger_travel();
    assert(PRO3_INPUT_ID == 4 && PRO3_INPUT_PAYLOAD_SIZE == 31);
    uint8_t expected[PRO3_INPUT_PAYLOAD_SIZE] = {8, 127, 127, 127, 127};
    hand_state hands[HAND_COUNT] = {
        {.x = -16384, .y = INT16_MIN}, {.x = 16384, .y = INT16_MAX}
    };
    pad_profile profile;
    pad_defaults(&profile);
    profile.stick_flags = 3;
    pad_report r = pad_compose(hands, &profile);
    check_packet(&r, (uint8_t[PRO3_INPUT_PAYLOAD_SIZE]){8, 63, 255, 191, 0});
    profile.sticks[0] = (stick_settings){.flags = STICK_INVERT_X, .sensitivity = 200};
    profile.sticks[1] = (stick_settings){.deadzone = 50, .sensitivity = 25};
    r = pad_compose(hands, &profile);
    uint8_t controls[PRO3_CONTROL_BYTES];
    assert(pro3_encode_controls(&r, controls) == sizeof(controls));
    const uint8_t tuned[PRO3_INPUT_PAYLOAD_SIZE] = {8, 255, 0, 127, 159};
    assert(!memcmp(controls, tuned, sizeof(controls)));
    check_packet(&r, tuned);
    profile.stick_flags = 0;
    r = pad_compose(hands, &profile);
    check_packet(&r, expected);

    // Each legacy action drives exactly its corresponding HID Button usage.
    static const unsigned expected_buttons[] = {
        0x00001, 0x00002, 0x00008, 0x00010, 0x00040, 0x00080,
        0x00400, 0x00800, 0x02000, 0x04000, 0x01000, 0x00100,
        0x00200, 0x00020, 0x00004, 0x01000, 0x10000, 0x20000
    };
    for (unsigned i = 0; i < sizeof(expected_buttons) / sizeof(expected_buttons[0]); ++i) {
        unsigned action = 1U << i;
        r.bytes[0] = action;
        r.bytes[1] = action >> 8;
        r.bytes[11] = action >> 16;
        expected[5] = r.trigger_level[1] = i == 12 ? 255 : 0;
        expected[6] = r.trigger_level[0] = i == 11 ? 255 : 0;
        expected[7] = expected_buttons[i];
        expected[8] = expected_buttons[i] >> 8;
        expected[9] = expected_buttons[i] >> 16;
        check_packet(&r, expected);
    }

    r.bytes[0] = r.bytes[1] = r.bytes[11] = 0xff;
    expected[5] = expected[6] = r.trigger_level[0] = r.trigger_level[1] = 255;
    expected[7] = 0xff;
    expected[8] = 0x7f;  // No dedicated Capture button.
    expected[9] = 0x03;
    for (unsigned hat = 0; hat <= 9; ++hat) {
        r.bytes[2] = hat;
        expected[0] = hat < 8 ? hat : 8;
        check_packet(&r, expected);
    }

    // Mix axes in the same report to detect axis swaps and signed overflow.
    const uint8_t sticks[8] = {0, 0x80, 0xff, 0x7f, 0, 0xc0, 0, 0x40};
    memcpy(r.bytes + 3, sticks, sizeof(sticks));
    expected[1] = 0;
    expected[2] = 255;
    expected[3] = 63;
    expected[4] = 191;
    for (unsigned level = 0; level <= 255; ++level) {
        r.trigger_level[0] = level;
        r.trigger_level[1] = 255 - level;
        expected[8] = 0x7c | (level == 0 ? 2 : level == 255 ? 1 : 0);
        expected[5] = 255 - level;
        expected[6] = level;
        check_packet(&r, expected);
    }

    puts("Pro 3 USB input mappings passed.");
    return 0;
}
