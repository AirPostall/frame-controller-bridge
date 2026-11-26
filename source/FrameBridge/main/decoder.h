#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { LEFT_HAND, RIGHT_HAND, HAND_COUNT };
enum { UNBOUND, PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_SELECT, PAD_START,
       PAD_L3, PAD_R3, PAD_HOME, PAD_UP, PAD_RIGHT, PAD_DOWN, PAD_LEFT,
       PAD_LT, PAD_RT, PAD_L_GRAB, PAD_R_GRAB, PAD_R_STEAM, PAD_L_GRAB_FULL, PAD_R_GRAB_FULL, ACTION_COUNT };
enum { CONTACT_BYTES = 14, RAW_CHANNELS = 18, TOUCH_SLOTS = 12, LEFT_TOUCH_SLOTS = 9, NO_CHANNEL = 255,
       TRAVEL_SLOTS = 3, LEGACY_PROFILE_BYTES = 174, V3_PROFILE_BYTES = 172, V4_PROFILE_BYTES = 174,
       CLICK_SOURCES = HAND_COUNT * 16, PRESS_SOURCES = CLICK_SOURCES + HAND_COUNT * TRAVEL_SLOTS,
       NAME_SLOTS = PRESS_SOURCES + ACTION_COUNT - 1, NAME_BYTES = 24,
       V5_PROFILE_BYTES = 1728, PROFILE_VERSION = 6 };
enum { WHOLE_BYTE, LOW_NIBBLE, HIGH_NIBBLE, BIT_ZERO, BIT_SEVEN = BIT_ZERO + 7 };

typedef struct {
    uint16_t buttons;
    int16_t x, y;
    uint8_t raw[RAW_CHANNELS], seen;  // 1 = block 0x19, 2 = block 0x14
    uint16_t active;                 // one independent hysteresis latch per touch slot
    uint8_t pressed_travel;
    uint32_t revision;
} hand_state;

typedef struct {
    uint8_t channel, mode, release, engage, action;
} touch_binding;

// channel 0 disables travel. Channels 1 and 2 read press-report bytes 6 and 7.
typedef struct { uint8_t channel, release, engage; } press_binding;

enum { STICK_INVERT_X = 1, STICK_INVERT_Y = 2 };
// Deadzone and sensitivity are whole percentages, applied independently to each axis.
typedef struct { uint8_t flags, deadzone, sensitivity; } stick_settings;

typedef struct {
    uint8_t version, stick_flags;
    uint8_t reserved[CLICK_SOURCES]; // V1–V4 stored one action per click here.
    touch_binding touch[HAND_COUNT][TOUCH_SLOTS];
    press_binding travel[HAND_COUNT][TRAVEL_SLOTS];
    // LT/RT each optionally read touch slot 1 + hand * TOUCH_SLOTS + slot; zero uses digital only.
    uint8_t trigger_source[2];
    // Clicks first, then travel; each group is hand-major. Bit (action - 1) enables an output.
    uint8_t outputs[PRESS_SOURCES][3];
    // UTF-8, NUL-terminated and padded. Press source aliases precede output aliases A..R4.
    uint8_t names[NAME_SLOTS][NAME_BYTES];
    stick_settings sticks[HAND_COUNT];
} pad_profile;

typedef struct {
    uint8_t bytes[12];         // Existing extended USB report layout.
    uint8_t trigger_level[2];  // Pro 3 analog triggers, in LT/RT order.
} pad_report;

void pad_defaults(pad_profile *profile);
bool pad_profile_valid(const pad_profile *profile);
bool pad_profile_load(pad_profile *out, const uint8_t *bytes, size_t length);
bool pad_decode(hand_state *state, const uint8_t *bytes, size_t length);
// Returns a normalized 0..255 field reading, or -1 when unavailable.
int pad_touch_level(const hand_state *state, const touch_binding *binding);
void pad_update_touch(hand_state *state, const touch_binding bindings[TOUCH_SLOTS]);
void pad_update_travel(hand_state *state, const press_binding bindings[TRAVEL_SLOTS]);
unsigned pad_usb_buttons(const pad_report *report);
pad_report pad_compose(const hand_state hands[HAND_COUNT], const pad_profile *profile);
size_t pad_standard_report(const pad_report *report, uint8_t out[11]);
