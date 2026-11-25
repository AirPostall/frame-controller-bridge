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

int main(void)
{
    assert(PRO3_INPUT_ID == 4 && PRO3_INPUT_PAYLOAD_SIZE == 31);
    uint8_t expected[PRO3_INPUT_PAYLOAD_SIZE] = {8, 127, 127, 127, 127};
    hand_state hands[HAND_COUNT] = {
        {.x = -16384, .y = INT16_MIN}, {.x = 16384, .y = INT16_MAX}
    };
    pad_profile profile = pad_defaults();
    profile.stick_flags = 3;
    pad_report r = pad_compose(hands, &profile);
    check_packet(&r, (uint8_t[PRO3_INPUT_PAYLOAD_SIZE]){8, 63, 255, 191, 0});
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
        expected[7] = expected_buttons[i];
        expected[8] = expected_buttons[i] >> 8;
        expected[9] = expected_buttons[i] >> 16;
        check_packet(&r, expected);
    }

    r.bytes[0] = r.bytes[1] = r.bytes[11] = 0xff;
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
        expected[5] = 255 - level;
        expected[6] = level;
        check_packet(&r, expected);
    }

    puts("Pro 3 USB input mappings passed.");
    return 0;
}
