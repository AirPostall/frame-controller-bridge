#include "decoder.h"
#include <string.h>

_Static_assert(offsetof(pad_profile, trigger_source) == V3_PROFILE_BYTES, "v3 profile prefix");
_Static_assert(offsetof(pad_profile, outputs) == V4_PROFILE_BYTES, "v4 profile prefix");
_Static_assert(offsetof(pad_profile, names) == 288 && offsetof(pad_profile, sticks) == V5_PROFILE_BYTES, "v5 profile prefix");
_Static_assert(sizeof(stick_settings) == 3 && offsetof(pad_profile, motion_hand) == V7_PROFILE_BYTES, "v6/v7 profile prefix");
_Static_assert(offsetof(pad_profile, gyro_gain) == V7_PROFILE_BYTES + 1 &&
               offsetof(pad_profile, gyro_reset_button) == V8_PROFILE_BYTES, "v8 profile prefix");
_Static_assert(sizeof(pad_profile) == V8_PROFILE_BYTES + 1, "v9 profile wire layout");
_Static_assert(offsetof(pad_report, motion_valid) == 14, "packed gamepad controls");

static uint16_t u16le(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }

static void add_action(uint8_t outputs[3], unsigned action)
{
    if (action) outputs[(action-1) / 8] |= 1U << ((action-1) % 8);
}

static uint32_t output_mask(const uint8_t outputs[3])
{
    return outputs[0] | ((uint32_t)outputs[1] << 8) | ((uint32_t)outputs[2] << 16);
}

static const press_binding default_trigger_travel = {TRIGGER_TRAVEL_CHANNEL, 64, 128};
static const unsigned trigger_touch_slot[HAND_COUNT] = {2, 5};

static void default_sticks(pad_profile *p)
{
    for (unsigned h = 0; h < HAND_COUNT; ++h)
        p->sticks[h] = (stick_settings){.flags = STICK_INVERT_Y, .sensitivity = 100};
}

static void empty_profile(pad_profile *p)
{
    memset(p, 0, sizeof(*p));
    p->version = PROFILE_VERSION;
    p->gyro_gain = GYRO_GAIN_DEFAULT;
    default_sticks(p);
    for (unsigned h = 0; h < HAND_COUNT; ++h)
        for (unsigned i = 0; i < TOUCH_SLOTS; ++i)
            p->touch[h][i].channel = NO_CHANNEL;
}

void pad_defaults(pad_profile *p)
{
    static const uint8_t clicks[HAND_COUNT][16] = {
        {PAD_DOWN, PAD_RIGHT, PAD_LEFT, PAD_UP, 0, PAD_LB, PAD_L3, 0, PAD_START, PAD_SELECT},
        {PAD_A, PAD_B, PAD_X, PAD_Y, 0, PAD_RB, PAD_R3, 0, PAD_R_STEAM, PAD_HOME}
    };
    empty_profile(p);
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        p->trigger_source[h] = TRIGGER_PHYSICAL_TRAVEL;
        p->travel[h][0] = default_trigger_travel;
        add_action(p->outputs[CLICK_SOURCES + h * TRAVEL_SLOTS], PAD_LT + h);
        for (unsigned i = 0; i < 16; ++i)
            add_action(p->outputs[h * 16 + i], clicks[h][i]);
    }
}

static unsigned field_mask(unsigned mode)
{
    if (mode == WHOLE_BYTE) return 255;
    if (mode == LOW_NIBBLE) return 15;
    if (mode == HIGH_NIBBLE) return 240;
    return 1U << (mode - BIT_ZERO);
}

// Validate Unicode scalar values without relying on the embedded C library's locale.
static bool valid_name(const uint8_t name[NAME_BYTES])
{
    size_t i = 0;
    while (i < NAME_BYTES && name[i]) {
        uint8_t first = name[i++];
        if (first < 0x80) {
            if (first < 0x20 || first == 0x7f) return false;
            continue;
        }
        if (first < 0xc2 || first > 0xf4) return false;
        unsigned count = first <= 0xdf ? 1 : first <= 0xef ? 2 : 3;
        if (i + count >= NAME_BYTES) return false; // reserve the terminator
        unsigned scalar = first & (0x7fU >> (count + 1));
        for (unsigned j = 0; j < count; ++j) {
            uint8_t byte = name[i++];
            if ((byte & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (byte & 0x3fU);
        }
        if (scalar < 0xa0 || (count == 2 && scalar < 0x800) || (count == 3 && scalar < 0x10000) ||
            (scalar >= 0xd800 && scalar <= 0xdfff) || scalar > 0x10ffff) return false;
    }
    if (i == NAME_BYTES) return false;
    for (; i < NAME_BYTES; ++i) if (name[i]) return false;
    return true;
}

static bool valid_profile_data(const pad_profile *p)
{
    if (p->version != PROFILE_VERSION || p->stick_flags > 3 || p->motion_hand > MOTION_RIGHT ||
        p->gyro_gain < GYRO_GAIN_MIN || p->gyro_gain > GYRO_GAIN_MAX ||
        p->gyro_reset_button > PRESS_SOURCES) return false;
    for (unsigned i = 0; i < CLICK_SOURCES; ++i)
        if (p->reserved[i]) return false;
    for (unsigned i = 0; i < PRESS_SOURCES; ++i)
        if (output_mask(p->outputs[i]) >> (ACTION_COUNT - 1)) return false;
    for (unsigned i = 0; i < NAME_SLOTS; ++i)
        if (!valid_name(p->names[i])) return false;
    for (unsigned h = 0; h < HAND_COUNT; h++) {
        const stick_settings *stick = &p->sticks[h];
        if ((stick->flags & ~(STICK_INVERT_X | STICK_INVERT_Y)) ||
            stick->deadzone > 50 || stick->sensitivity < 25 || stick->sensitivity > 200) return false;
        unsigned source = p->trigger_source[h];
        if (source && source != TRIGGER_PHYSICAL_TRAVEL) {
            unsigned index = source - 1;
            if (index >= HAND_COUNT * TOUCH_SLOTS ||
                p->touch[index / TOUCH_SLOTS][index % TOUCH_SLOTS].channel == NO_CHANNEL)
                return false;
        }
        unsigned used[RAW_CHANNELS] = {0};
        for (unsigned i = 0; i < TRAVEL_SLOTS; i++) {
            const press_binding *b = &p->travel[h][i];
            if (b->channel > 2 || (b->channel && b->release == b->engage)) return false;
        }
        for (unsigned i = 0; i < TOUCH_SLOTS; i++) {
            const touch_binding *t = &p->touch[h][i];
            if (t->action >= ACTION_COUNT) return false;
            if (t->channel == NO_CHANNEL) continue;
            if (h == LEFT_HAND && i >= LEFT_TOUCH_SLOTS) return false;
            if (t->channel >= RAW_CHANNELS || t->mode > BIT_SEVEN || t->release == t->engage)
                return false;
            unsigned mask = field_mask(t->mode);
            if (used[t->channel] & mask) return false;
            used[t->channel] |= mask;
        }
    }
    return true;
}

bool pad_profile_valid(const pad_profile *p)
{
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        if (p->trigger_source[h] != TRIGGER_PHYSICAL_TRAVEL ||
            p->travel[h][0].channel != TRIGGER_TRAVEL_CHANNEL ||
            output_mask(p->outputs[h * 16 + 7]) ||
            p->touch[h][trigger_touch_slot[h]].action != UNBOUND) return false;
    }
    return valid_profile_data(p);
}

static bool migrate_profile(pad_profile *next, const uint8_t *bytes, size_t length)
{
    if (length == 0) return false;
    unsigned version = bytes[0];
    static const uint16_t lengths[] = {0, 35, LEGACY_PROFILE_BYTES, V3_PROFILE_BYTES, V4_PROFILE_BYTES};
    if (version == 0 || version >= sizeof(lengths) / sizeof(lengths[0]) || length != lengths[version])
        return false;
    empty_profile(next);
    next->stick_flags = bytes[1];
    for (unsigned i = 0; i < CLICK_SOURCES; ++i) {
        unsigned action = bytes[2 + i];
        if (action >= ACTION_COUNT || (version <= 2 && action > PAD_LEFT)) return false;
        add_action(next->outputs[i], action);
    }
    if (version >= 3) {
        size_t touch_offset = offsetof(pad_profile, touch);
        memcpy((uint8_t *)next + touch_offset, bytes + touch_offset, length - touch_offset);
        static const uint8_t travel_actions[HAND_COUNT][TRAVEL_SLOTS] = {
            {PAD_LT, PAD_L_GRAB, PAD_L_GRAB_FULL}, {PAD_RT, PAD_R_GRAB, PAD_R_GRAB_FULL}
        };
        for (unsigned h = 0; h < HAND_COUNT; ++h)
            for (unsigned i = 0; i < TRAVEL_SLOTS; ++i)
                if (next->travel[h][i].channel)
                    add_action(next->outputs[CLICK_SOURCES + h * TRAVEL_SLOTS + i], travel_actions[h][i]);
        return true;
    }
    if (version == 1) {
        // V1 stored the Right A action only. Learning its surface keeps this action.
        next->touch[RIGHT_HAND][0].action = bytes[34];
        return bytes[34] <= PAD_LEFT;
    }

    // One shared D-pad surface starts unbound. Retired fingers have no successor.
    static const int8_t old_slot[HAND_COUNT][TOUCH_SLOTS] = {
        {-1,4,5,6,7,8,-1,12,13,-1,-1,-1},
        {0,1,2,3,4,5,6,7,8,-1,12,13}
    };
    for (unsigned h = 0; h < HAND_COUNT; h++) {
        for (unsigned i = 0; i < TOUCH_SLOTS; i++) {
            int old = old_slot[h][i];
            if (old < 0) continue;
            touch_binding prior;
            memcpy(&prior, bytes + 34 + (h * 14 + old) * sizeof(prior), sizeof(prior));
            if (prior.action > PAD_LEFT) return false;
            touch_binding *dest = &next->touch[h][i];
            *dest = prior;
            // Keep the action if a saved field is invalid or overlaps an earlier slot.
            if (!valid_profile_data(next))
                *dest = (touch_binding){.channel = NO_CHANNEL, .action = prior.action};
        }
    }
    return true;
}

static void migrate_trigger_contacts(pad_profile *p, unsigned version)
{
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        press_binding *travel = &p->travel[h][0];
        if (travel->channel != TRIGGER_TRAVEL_CHANNEL) {
            if (!travel->channel) *travel = default_trigger_travel;
            travel->channel = TRIGGER_TRAVEL_CHANNEL;
            add_action(p->outputs[CLICK_SOURCES + h * TRAVEL_SLOTS], PAD_LT + h);
        }
        p->trigger_source[h] = TRIGGER_PHYSICAL_TRAVEL;
        memset(p->outputs[h * 16 + 7], 0, sizeof(p->outputs[0]));
        p->touch[h][trigger_touch_slot[h]].action = UNBOUND;
        if (version < 7) {
            // Older profiles used digital touch assignments as analog end stops.
            for (unsigned slot = 0; slot < TOUCH_SLOTS; ++slot) {
                touch_binding *touch = &p->touch[h][slot];
                if (touch->action == PAD_LT || touch->action == PAD_RT) touch->action = UNBOUND;
            }
        }
    }
}

bool pad_profile_load(pad_profile *out, const uint8_t *bytes, size_t length)
{
    pad_profile next = {.gyro_gain = GYRO_GAIN_DEFAULT};
    if (length == sizeof(next) && bytes[0] == PROFILE_VERSION) {
        memcpy(&next, bytes, length);
    } else if (length == V8_PROFILE_BYTES && bytes[0] == 8) {
        memcpy(&next, bytes, length);
    } else if (length == V7_PROFILE_BYTES && (bytes[0] == 6 || bytes[0] == 7)) {
        memcpy(&next, bytes, length);
    } else if (length == V5_PROFILE_BYTES && bytes[0] == 5) {
        memcpy(&next, bytes, length);
        default_sticks(&next);
    } else if (!migrate_profile(&next, bytes, length)) return false;
    next.version = PROFILE_VERSION;
    if (bytes[0] < 7 &&
        (next.trigger_source[0] >= TRIGGER_PHYSICAL_TRAVEL ||
         next.trigger_source[1] >= TRIGGER_PHYSICAL_TRAVEL)) return false;
    if (!valid_profile_data(&next)) return false;
    migrate_trigger_contacts(&next, bytes[0]);
    *out = next;
    return true;
}

static bool control_active(unsigned value, unsigned release, unsigned engage, bool was_active)
{
    if (engage > release) return value >= engage || (was_active && value > release);
    return value <= engage || (was_active && value < release);
}

void pad_update_travel(hand_state *state, const press_binding bindings[TRAVEL_SLOTS])
{
    for (unsigned i = 0; i < TRAVEL_SLOTS; ++i) {
        const press_binding *b = &bindings[i];
        unsigned flag = 1U << i;
        if (b->channel && (state->seen & 2) &&
            control_active(state->raw[CONTACT_BYTES + b->channel - 1], b->release, b->engage,
                           state->pressed_travel & flag))
            state->pressed_travel |= flag;
        else state->pressed_travel &= ~flag;
    }
}

int pad_touch_level(const hand_state *state, const touch_binding *binding)
{
    if (binding->channel >= RAW_CHANNELS || binding->mode > BIT_SEVEN ||
        !(state->seen & (binding->channel < CONTACT_BYTES ? 1 : 2))) return -1;
    unsigned value = state->raw[binding->channel];
    if (binding->mode == LOW_NIBBLE) return (value & 15) * 17;
    if (binding->mode == HIGH_NIBBLE) return (value >> 4) * 17;
    if (binding->mode >= BIT_ZERO) return (value & field_mask(binding->mode)) ? 255 : 0;
    return value;
}

void pad_update_touch(hand_state *state, const touch_binding bindings[TOUCH_SLOTS])
{
    for (unsigned i = 0; i < TOUCH_SLOTS; i++) {
        const touch_binding *t = &bindings[i];
        unsigned flag = 1U << i;
        int value = pad_touch_level(state, t);
        if (value < 0) {
            state->active &= ~flag;
            continue;
        }
        if (control_active(value, t->release, t->engage, state->active & flag)) state->active |= flag;
        else state->active &= ~flag;
    }
}

bool pad_decode(hand_state *state, const uint8_t *bytes, size_t length)
{
    hand_state next = *state;
    for (size_t i = 0; i < length;) {
        if (length - i < 2) return false;
        uint8_t tag = bytes[i++], n = bytes[i++];
        if (n > length - i) return false;
        const uint8_t *p = &bytes[i];
        if (tag == 0x1e && n != 22) return false;
        if (tag == 0x14) {
            if (n != 8) return false;
            next.raw[14] = p[6];
            next.raw[TRIGGER_RAW] = p[7];
            next.raw[16] = p[0];
            next.raw[17] = p[1];
            next.seen |= 2;
            next.buttons = u16le(p);
            next.x = (int16_t)u16le(p + 2);
            next.y = (int16_t)u16le(p + 4);
        } else if (tag == 0x19) {
            if (n != 14) return false;
            memcpy(next.raw, p, CONTACT_BYTES);
            next.seen |= 1;
        }
        i += n;
    }
    if (state->seen != next.seen || memcmp(state->raw, next.raw, sizeof(next.raw)))
        ++next.revision;
    *state = next;
    return true;
}

static int16_t stick_axis(int16_t raw, const stick_settings *settings, unsigned invert_flag)
{
    int32_t value = raw;
    if (settings->deadzone || settings->sensitivity != 100) {
        int32_t limit = raw < 0 ? 32768 : 32767;
        int32_t magnitude = raw < 0 ? -value : value;
        int32_t threshold = limit * settings->deadzone / 100;
        if (magnitude <= threshold) return 0;
        int32_t range = limit - threshold;
        magnitude = ((magnitude - threshold) * limit + range / 2) / range;
        magnitude = (magnitude * settings->sensitivity + 50) / 100;
        if (magnitude > limit) magnitude = limit;
        value = raw < 0 ? -magnitude : magnitude;
    }
    if (!(settings->flags & invert_flag)) return (int16_t)value;
    // Swapping both end stops retains the full signed range when inverting.
    if (value == INT16_MIN) return INT16_MAX;
    if (value == INT16_MAX) return INT16_MIN;
    return (int16_t)-value;
}

pad_report pad_compose(const hand_state hands[HAND_COUNT], const pad_profile *p)
{
    pad_report report = {0};
    uint32_t actions = 0;
    for (unsigned h = 0; h < HAND_COUNT; h++) {
        unsigned bits = hands[h].buttons;
        while (bits) {
            unsigned bit = __builtin_ctz(bits);
            actions |= output_mask(p->outputs[h * 16 + bit]);
            bits &= bits - 1;
        }
        unsigned travel = hands[h].pressed_travel;
        while (travel) {
            unsigned slot = __builtin_ctz(travel);
            actions |= output_mask(p->outputs[CLICK_SOURCES + h * TRAVEL_SLOTS + slot]);
            travel &= travel - 1;
        }
        unsigned touches = hands[h].active;
        while (touches) {
            unsigned slot = __builtin_ctz(touches);
            unsigned action = p->touch[h][slot].action;
            if (action) actions |= 1U << (action - 1);
            touches &= touches - 1;
        }
        if (hands[h].seen & 2) {
            // Only the direction comes from calibration. Keep the entire analog range.
            const press_binding *travel = &p->travel[h][0];
            uint8_t raw = hands[h].raw[TRIGGER_RAW];
            report.trigger_level[h] = travel->engage > travel->release ? raw : 255 - raw;
        }
        if (p->stick_flags & (1U << h)) {
            uint8_t *axis = &report.bytes[3 + 4*h];
            uint16_t x = (uint16_t)stick_axis(hands[h].x, &p->sticks[h], STICK_INVERT_X);
            uint16_t y = (uint16_t)stick_axis(hands[h].y, &p->sticks[h], STICK_INVERT_Y);
            axis[0] = (uint8_t)x; axis[1] = (uint8_t)(x >> 8);
            axis[2] = (uint8_t)y; axis[3] = (uint8_t)(y >> 8);
        }
    }
    // Actions 12..15 drive the hat. New USB buttons follow button 11.
    uint32_t buttons = (actions & 0x7ffU) | ((actions >> (PAD_LT-1)) << 11);
    report.bytes[0] = (uint8_t)buttons;
    report.bytes[1] = (uint8_t)(buttons >> 8);
    report.bytes[11] = (uint8_t)(buttons >> 16);
    // Opposite directions cancel. Index rows are up/neutral/down; columns left/neutral/right.
    int dx = !!(actions & (1U << (PAD_RIGHT-1))) - !!(actions & (1U << (PAD_LEFT-1)));
    int dy = !!(actions & (1U << (PAD_DOWN-1))) - !!(actions & (1U << (PAD_UP-1)));
    static const uint8_t hats[3][3] = {{7,0,1},{6,8,2},{5,4,3}};
    report.bytes[2] = hats[dy+1][dx+1];
    return report;
}

unsigned pad_usb_buttons(const pad_report *report)
{
    return report->bytes[0] | (report->bytes[1] << 8) | (report->bytes[11] << 16);
}

// TinyUSB's standard gamepad uses signed 8-bit X, Y, Z, Rz, Rx and Ry,
// followed by its one-based hat and a contiguous 32-bit button bitmap.
size_t pad_standard_report(const pad_report *r, uint8_t out[11])
{
    for (unsigned i = 0; i < 4; ++i) {
        int32_t axis = (int16_t)u16le(&r->bytes[3 + i * 2]);
        out[i] = (uint8_t)((axis * 127) / (axis < 0 ? 32768 : 32767));
    }
    out[4] = out[5] = 0;
    out[6] = r->bytes[2] < 8 ? r->bytes[2] + 1 : 0;
    out[7] = r->bytes[0];
    out[8] = r->bytes[1];
    out[9] = r->bytes[11];
    out[10] = 0;
    return 11;
}
