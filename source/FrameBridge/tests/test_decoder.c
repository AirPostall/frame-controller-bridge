#include "../main/decoder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t bit_for(unsigned action)
{
    assert(action > UNBOUND && action < ACTION_COUNT);
    return 1U << (action - 1);
}

static void bind_outputs(pad_profile *profile, unsigned source, uint32_t mask)
{
    for (unsigned i = 0; i < 3; i++)
        profile->outputs[source][i] = (uint8_t)(mask >> (8 * i));
}

static uint32_t outputs_at(const pad_profile *profile, unsigned source)
{
    const uint8_t *bytes = profile->outputs[source];
    return bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16);
}

static void check_sticks(const pad_report *report, const int16_t expected[4])
{
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *axis = &report->bytes[3 + i * 2];
        assert((axis[0] | (axis[1] << 8)) == (uint16_t)expected[i]);
    }
}

static void test_touch_triggers(void)
{
    pad_profile p;
    pad_defaults(&p);
    p.trigger_source[0] = 13;  // Right A touch drives left trigger.
    p.trigger_source[1] = 1;   // Left D-pad touch drives right trigger.
    p.touch[1][0] = (touch_binding){3, WHOLE_BYTE, 50, 150, PAD_A};
    p.touch[0][0] = (touch_binding){5, HIGH_NIBBLE, 170, 34, 0};
    assert(pad_profile_valid(&p));

    hand_state hands[HAND_COUNT] = {0};
    uint8_t left[16] = {0x19, CONTACT_BYTES};
    uint8_t right[16] = {0x19, CONTACT_BYTES};
    left[7] = 0x6f;
    assert(pad_touch_level(&hands[0], &p.touch[0][0]) == -1);
    assert(pad_decode(&hands[0], left, sizeof(left)));
    assert(pad_touch_level(&hands[0], &p.touch[0][0]) == 102);
    assert(pad_touch_level(&hands[0], &p.touch[0][1]) == -1);
    pad_report r = pad_compose(hands, &p);
    assert(r.trigger_level[0] == 0 && r.trigger_level[1] == 128);

    // Both endpoints saturate, and movement between them reaches USB before the
    // digital touch threshold fires. The calibrated touch can still have its own action.
    const uint8_t inputs[] = {40, 50, 51, 100, 149, 150, 255};
    const uint8_t expected[] = {0, 0, 3, 128, 252, 255, 255};
    for (size_t i = 0; i < sizeof(inputs); ++i) {
        right[5] = inputs[i];
        assert(pad_decode(&hands[1], right, sizeof(right)));
        pad_update_touch(&hands[1], p.touch[1]);
        r = pad_compose(hands, &p);
        assert(r.trigger_level[0] == expected[i]);
        assert(!!(pad_usb_buttons(&r) & 1) == (inputs[i] >= 150));
        assert(r.trigger_level[1] == 128);
    }

    // A real digital trigger binding overrides the current analog touch level.
    right[5] = 100;
    assert(pad_decode(&hands[1], right, sizeof(right)));
    bind_outputs(&p, 0, bit_for(PAD_LT));
    hands[0].buttons = 1;
    assert(pad_compose(hands, &p).trigger_level[0] == 255);
    hands[0].buttons = 0;
    assert(pad_compose(hands, &p).trigger_level[0] == 128);

    left[7] = 0x0f; // Falling calibration caps low raw input at full output.
    assert(pad_decode(&hands[0], left, sizeof(left)));
    assert(pad_compose(hands, &p).trigger_level[1] == 255);
    left[7] = 0xb0;
    assert(pad_decode(&hands[0], left, sizeof(left)));
    assert(pad_compose(hands, &p).trigger_level[1] == 0);

    // A press-block signal must first be seen on its own hand. Packed modes
    // share the decoder used for thresholded touch. Unknown blocks retain state.
    p.touch[0][0] = (touch_binding){14, LOW_NIBBLE, 0, 255, 0};
    assert(pad_profile_valid(&p));
    assert(pad_compose(hands, &p).trigger_level[1] == 0);
    uint8_t press[10] = {0x14, 8};
    press[8] = 0xaf;
    assert(pad_decode(&hands[0], press, sizeof(press)));
    p.trigger_source[0] = p.trigger_source[1];
    r = pad_compose(hands, &p);
    assert(r.trigger_level[0] == 255 && r.trigger_level[1] == 255);
    press[8] = 0xa2;
    assert(pad_decode(&hands[0], press, sizeof(press)));
    assert(pad_decode(&hands[0], (uint8_t[]){0x1a, 0}, 2));
    assert(pad_compose(hands, &p).trigger_level[0] == 34);
    p.touch[0][0].mode = BIT_ZERO + 1;
    assert(pad_compose(hands, &p).trigger_level[0] == 255);
    p.touch[0][0].mode = BIT_ZERO;
    assert(pad_compose(hands, &p).trigger_level[0] == 0);
    p.touch[0][0] = (touch_binding){14, WHOLE_BYTE, 255, 0, 0};
    hands[0] = (hand_state){0};
    r = pad_compose(hands, &p);
    assert(r.trigger_level[0] == 0 && r.trigger_level[1] == 0);

    for (unsigned source = 1; source <= 255; ++source) {
        p.trigger_source[0] = (uint8_t)source;
        bool valid = source == 1 || source == 13;
        assert(pad_profile_valid(&p) == valid);
    }
}

static void test_legacy_profile_upgrade(void)
{
    // Build the old wire payload explicitly. The former one-action-per-bit array
    // is reserved in V5; an unassigned bit 7 in a stored profile must stay unassigned.
    uint8_t bytes[174] = {0, 3};
    bytes[2 + 16 + 14] = PAD_R_GRAB_FULL;
    for (unsigned i = 0; i < 24; ++i) bytes[34 + 5*i] = NO_CHANNEL;
    memcpy(bytes + 34 + 23*5, (uint8_t[]){4, HIGH_NIBBLE, 51, 204, PAD_X}, 5);
    memcpy(bytes + 154 + 4*3, (uint8_t[]){2, 170, 42}, 3);
    bytes[173] = 24;
    const unsigned legacy_travel[] = {
        PAD_LT, PAD_L_GRAB, PAD_L_GRAB_FULL, PAD_RT, PAD_R_GRAB, PAD_R_GRAB_FULL
    };
    for (unsigned version = 3; version <= 4; ++version) {
        bytes[0] = version;
        if (version == 4) {
            for (unsigned i = 0; i < 6; ++i)
                memcpy(bytes + 154 + 3*i, (uint8_t[]){1, 25, 100}, 3);
        }
        const size_t length = version == 3 ? 172 : sizeof(bytes);
        pad_profile destination;
        pad_defaults(&destination);
        assert(pad_profile_load(&destination, bytes, length));
        assert(destination.version == PROFILE_VERSION && destination.stick_flags == 3);
        assert(!memcmp(destination.touch, bytes + 34, 120));
        assert(!memcmp(destination.travel, bytes + 154, 18));
        assert(destination.trigger_source[0] == 0);
        assert(destination.trigger_source[1] == (version == 4 ? 24 : 0));
        for (unsigned i = 0; i < 38; ++i) {
            uint32_t expected = 0;
            if (i == 16 + 14) expected = bit_for(PAD_R_GRAB_FULL);
            if (i >= 32 && bytes[154 + 3*(i - 32)]) expected = bit_for(legacy_travel[i - 32]);
            assert(outputs_at(&destination, i) == expected);
        }
        assert(!memcmp(destination.names, (uint8_t[60][24]){{0}}, sizeof(destination.names)));
        memcpy(destination.names[59], "Paddle", 7);
        pad_profile saved = destination;
        assert(pad_profile_load(&destination, (const uint8_t *)&saved, sizeof(saved)));
        assert(!memcmp(&destination, &saved, sizeof(saved)));
        bytes[2] = ACTION_COUNT;
        assert(!pad_profile_load(&destination, bytes, length));
        assert(!memcmp(&destination, &saved, sizeof(saved)));
        bytes[2] = 0;
        assert(!pad_profile_load(&destination, bytes, 0));
        assert(!pad_profile_load(&destination, (const uint8_t *)&saved, sizeof(saved) - 1));
    }
}

static void test_stick_settings(void)
{
    pad_profile p;
    pad_defaults(&p);
    assert(!p.stick_flags);
    assert(!memcmp(p.sticks, (uint8_t[]){2, 0, 100, 2, 0, 100}, sizeof(p.sticks)));

    // Load in place as at boot, retaining every V5 field including names and bindings.
    p.stick_flags = 3;
    memcpy(p.names[59], "Right rear", 11);
    bind_outputs(&p, 17, bit_for(PAD_X) | bit_for(PAD_Y));
    p.version = 5;
    uint8_t original[V5_PROFILE_BYTES];
    memcpy(original, &p, sizeof(original));
    memset(p.sticks, 255, sizeof(p.sticks));
    assert(pad_profile_load(&p, (const uint8_t *)&p, sizeof(original)));
    assert(!memcmp((const uint8_t *)&p + 1, original + 1, sizeof(original) - 1));
    assert(!memcmp(p.sticks, (uint8_t[]){2, 0, 100, 2, 0, 100}, sizeof(p.sticks)));
    original[1] = 4;
    assert(!pad_profile_load(&p, original, sizeof(original)));
    assert(p.version == PROFILE_VERSION && p.stick_flags == 3);

    hand_state hands[2] = {{.x = -16384, .y = 16383}, {.x = INT16_MAX, .y = INT16_MIN}};
    p.sticks[0] = (stick_settings){.flags = STICK_INVERT_X, .deadzone = 50, .sensitivity = 100};
    p.sticks[1] = (stick_settings){.sensitivity = 25};
    assert(pad_profile_valid(&p));
    pad_report report = pad_compose(hands, &p);
    check_sticks(&report, (int16_t[]){0, 0, 8192, -8192});
    hands[0].x--; hands[0].y++;
    report = pad_compose(hands, &p);
    check_sticks(&report, (int16_t[]){2, 2, 8192, -8192});

    hands[0].x = -24576; hands[0].y = 24576;
    p.sticks[0] = (stick_settings){.flags = STICK_INVERT_X | STICK_INVERT_Y, .deadzone = 10, .sensitivity = 200};
    report = pad_compose(hands, &p);
    check_sticks(&report, (int16_t[]){INT16_MAX, INT16_MIN, 8192, -8192});
    uint8_t standard[11];
    pad_standard_report(&report, standard);
    assert(!memcmp(standard, (uint8_t[]){127, 129, 31, 225}, 4));
    p.stick_flags = 2;
    report = pad_compose(hands, &p);
    check_sticks(&report, (int16_t[]){0, 0, 8192, -8192});
    const stick_settings invalid[] = {{4, 0, 100}, {0, 51, 100}, {0, 0, 24}, {0, 0, 201}};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        p.sticks[0] = invalid[i];
        assert(!pad_profile_valid(&p));
    }
}

static void test_many_to_many(void)
{
    pad_profile p;
    pad_defaults(&p);
    bind_outputs(&p, 4, bit_for(PAD_A) | bit_for(PAD_RT));
    bind_outputs(&p, 16 + 4, bit_for(PAD_A) | bit_for(PAD_R_GRAB_FULL));
    p.travel[0][0] = (press_binding){1, 30, 80};
    bind_outputs(&p, 32, bit_for(PAD_B) | bit_for(PAD_RT));
    assert(pad_profile_valid(&p));
    hand_state hands[HAND_COUNT] = {0};
    hands[0].buttons = 1U << 4;
    hands[1].buttons = 1U << 4;
    pad_report r = pad_compose(hands, &p);
    const unsigned rt = 1U << 12, full = 1U << 17;
    assert(pad_usb_buttons(&r) == (1U | rt | full) && r.trigger_level[1] == 255);
    hands[0].buttons = 0;
    r = pad_compose(hands, &p);
    assert(pad_usb_buttons(&r) == (1U | full) && r.trigger_level[1] == 0);

    // Independent click/travel mappings share a trigger. The travel latch uses
    // the configured engage/release values and keeps it held after the click releases.
    hands[0].buttons = 1U << 4;
    hands[1].buttons = 0;
    hands[0].seen = 2;
    hands[0].raw[14] = 80;
    pad_update_travel(&hands[0], p.travel[0]);
    r = pad_compose(hands, &p);
    assert(pad_usb_buttons(&r) == (1U | 2U | rt));
    hands[0].buttons = 0;
    hands[0].raw[14] = 31;
    pad_update_travel(&hands[0], p.travel[0]);
    r = pad_compose(hands, &p);
    assert(pad_usb_buttons(&r) == (2U | rt) && r.trigger_level[1] == 255);
    hands[0].raw[14] = 30;
    pad_update_travel(&hands[0], p.travel[0]);
    r = pad_compose(hands, &p);
    assert(!pad_usb_buttons(&r) && !r.trigger_level[1]);
    hands[0].raw[14] = 79;
    pad_update_travel(&hands[0], p.travel[0]);
    assert(!hands[0].pressed_travel);

    p.travel[0][0] = (press_binding){1, 190, 110};
    const uint8_t positions[] = {200, 111, 110, 150, 189, 190};
    const uint8_t pressed[] = {0, 0, 1, 1, 1, 0};
    for (size_t i = 0; i < sizeof(positions); ++i) {
        hands[0].raw[14] = positions[i];
        pad_update_travel(&hands[0], p.travel[0]);
        assert(hands[0].pressed_travel == pressed[i]);
    }
    p.travel[0][0].engage = 190;
    assert(!pad_profile_valid(&p));
}

static void test_names(void)
{
    pad_profile p;
    pad_defaults(&p);
    memcpy(p.names[0], "Hyppy 🎮", sizeof("Hyppy 🎮"));
    memset(p.names[59], 'a', 23);
    assert(pad_profile_valid(&p));
    pad_profile copy;
    assert(pad_profile_load(&copy, (const uint8_t *)&p, sizeof(p)));
    assert(!memcmp(copy.names, p.names, sizeof(p.names)));
    p.names[59][23] = 'a';
    assert(!pad_profile_valid(&p)); // missing terminator
    memset(p.names[59], 0, 24);
    p.names[59][1] = 'x';
    assert(!pad_profile_valid(&p)); // nonzero padding after terminator
    static const uint8_t invalid[][24] = {
        {0x7f}, {'A', '\n'}, {0xc2, 0x85}, {0x80}, {0xc0, 0xaf}, {0xed, 0xa0, 0x80},
        {0xe2, 0x82}, {0xf4, 0x90, 0x80, 0x80}
    };
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        memcpy(p.names[59], invalid[i], sizeof(invalid[i]));
        assert(!pad_profile_valid(&p));
    }
    memset(p.names[59], 0, 24);
    p.reserved[31] = 1;
    assert(!pad_profile_valid(&p));
}

int main(void)
{
    test_touch_triggers();
    test_legacy_profile_upgrade();
    test_stick_settings();
    test_many_to_many();
    test_names();
    pad_profile profile;
    pad_defaults(&profile);
    assert(pad_profile_valid(&profile));
    hand_state hands[2] = {0};
    uint8_t pressed[] = {0x14,8,1,0,0x80,0xff,0xff,0x7f,0,0};
    uint8_t released[] = {0x14,8,0,0,0,0,0,0,0,0};
    uint8_t touch[16] = {0x19,14};
    profile.touch[1][0] = (touch_binding){5, WHOLE_BYTE, 64, 192, UNBOUND};
    touch[7] = 255;
    assert(pad_decode(&hands[1],touch,sizeof(touch)));
    pad_update_touch(&hands[1],profile.touch[1]);
    assert(hands[1].raw[5] == 255 && (hands[1].active & 1));
    profile.touch[1][0].action = PAD_B;
    assert(pad_compose(hands,&profile).bytes[0] == 2);
    touch[7]=128;
    assert(pad_decode(&hands[1],touch,sizeof(touch)));
    pad_update_touch(&hands[1],profile.touch[1]);
    assert(hands[1].active & 1);
    touch[7]=64;
    assert(pad_decode(&hands[1],touch,sizeof(touch)));
    pad_update_touch(&hands[1],profile.touch[1]);
    assert(!(hands[1].active & 1));
    touch[7]=255;
    assert(pad_decode(&hands[1],touch,sizeof(touch)));
    pad_update_touch(&hands[1],profile.touch[1]);
    profile.touch[1][0].action = UNBOUND;
    assert(pad_compose(hands,&profile).bytes[0] == 0);

    assert(pad_decode(&hands[1],pressed,sizeof(pressed)));
    assert(pad_decode(&hands[0],pressed,sizeof(pressed)));
    pad_report r = pad_compose(hands,&profile);
    assert(r.bytes[0] == 1 && r.bytes[2] == 4 && r.bytes[3] == 0);
    uint8_t truncated[] = {0x14,8,0,0,0,0,0,0,0,0,0x19,14,0};
    assert(!pad_decode(&hands[1],truncated,sizeof(truncated)));
    assert(hands[1].buttons == 1 && hands[1].raw[5] == 255);
    assert(!pad_decode(&hands[1],pressed,1));
    uint8_t bad_known_length[] = {0x14,1,0};
    assert(!pad_decode(&hands[1],bad_known_length,3));
    assert(pad_decode(&hands[1],touch,sizeof(touch)));
    assert(hands[1].buttons == 1);
    const uint8_t unknown[] = {0xef,1,42};
    assert(pad_decode(&hands[1],unknown,sizeof(unknown)));
    assert(hands[1].buttons == 1);

    // Opposite raw Y endpoints invert in the full-resolution and Standard layouts.
    const uint8_t opposite[] = {0x14,8,1,0,0,0x40,0,0x80,0,0};
    assert(pad_decode(&hands[1], opposite, sizeof(opposite)));
    profile.stick_flags = 3;
    r = pad_compose(hands, &profile);
    check_sticks(&r, (int16_t[]){-128, INT16_MIN, 16384, INT16_MAX});
    uint8_t standard_axes[11];
    pad_standard_report(&r, standard_axes);
    assert(!memcmp(standard_axes, (uint8_t[]){0, 129, 63, 127}, 4));

    hands[0].y = -1234;
    hands[1].y = 5678;
    r = pad_compose(hands, &profile);
    check_sticks(&r, (int16_t[]){-128, 1234, 16384, -5678});

    hands[0].y = 0;
    profile.stick_flags = 1;
    r = pad_compose(hands, &profile);
    check_sticks(&r, (int16_t[]){-128, 0, 0, 0});
    profile.stick_flags = 2;
    r = pad_compose(hands, &profile);
    check_sticks(&r, (int16_t[]){0, 0, 16384, -5678});
    profile.stick_flags = 3;
    assert(pad_decode(&hands[1],released,sizeof(released)));
    r = pad_compose(hands,&profile);
    assert(r.bytes[0] == 0 && r.bytes[2] == 4 && r.bytes[7] == 0);

    hands[1].buttons = 2;
    bind_outputs(&profile, 16 + 1, bit_for(PAD_RIGHT));
    assert(pad_compose(hands,&profile).bytes[2] == 3);  // down-right
    bind_outputs(&profile, 16 + 1, bit_for(PAD_UP));
    assert(pad_compose(hands,&profile).bytes[2] == 8);  // up and down cancel
    hands[0] = (hand_state){0};                         // a hand disconnected
    assert(pad_compose(hands,&profile).bytes[2] == 0);  // other hand preserved
    bind_outputs(&profile, 16 + 1, bit_for(PAD_HOME));
    assert(pad_compose(hands,&profile).bytes[1] == 4);
    bind_outputs(&profile, 16 + 1, 1U << 22);
    assert(!pad_profile_valid(&profile));
    pad_profile p;
    pad_defaults(&p);
    assert(outputs_at(&p, 9) == bit_for(PAD_SELECT) && outputs_at(&p, 8) == bit_for(PAD_START));
    assert(outputs_at(&p, 16 + 9) == bit_for(PAD_HOME) && outputs_at(&p, 16 + 2) == bit_for(PAD_X));
    assert(outputs_at(&p, 7) == bit_for(PAD_LT) && outputs_at(&p, 16 + 7) == bit_for(PAD_RT));
    assert(outputs_at(&p, 16 + 8) == bit_for(PAD_R_STEAM));
    const uint8_t trigger_press[] = {0x14, 8, 0x80, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t trigger_and_steam[] = {0x14, 8, 0x80, 1, 0, 0, 0, 0, 0, 0};
    assert(pad_decode(&hands[0], trigger_press, sizeof(trigger_press)));
    assert(pad_decode(&hands[1], trigger_and_steam, sizeof(trigger_and_steam)));
    r = pad_compose(hands, &p);
    assert(pad_usb_buttons(&r) == ((1U << 11) | (1U << 12) | (1U << 15)));
    assert(r.trigger_level[0] == 255 && r.trigger_level[1] == 255);
    uint8_t legacy[35] = {1, 3};
    legacy[2 + 5] = PAD_X;
    legacy[34] = PAD_UP;
    assert(pad_profile_load(&p, legacy, sizeof(legacy)) && p.version == PROFILE_VERSION);
    assert(outputs_at(&p, 5) == bit_for(PAD_X) && outputs_at(&p, 0) == 0);
    assert(p.touch[1][0].action == PAD_UP && p.stick_flags == 3);
    assert(p.touch[1][0].channel == NO_CHANNEL);
    legacy[34] = 220;
    assert(!pad_profile_load(&p, legacy, sizeof(legacy)) && p.touch[1][0].action == PAD_UP);
    pad_defaults(&p);
    // Fresh profiles must stay neutral for all touch input before learning.
    memset(hands, 0, sizeof(hands));
    memset(touch + 2, 255, CONTACT_BYTES);
    for (unsigned h = 0; h < HAND_COUNT; ++h) {
        assert(p.trigger_source[h] == 0);
        for (unsigned i = 0; i < TOUCH_SLOTS; ++i) {
            assert(p.touch[h][i].channel == NO_CHANNEL);
            assert(p.touch[h][i].action == UNBOUND);
        }
        assert(pad_decode(&hands[h], touch, sizeof(touch)));
        pad_update_touch(&hands[h], p.touch[h]);
        assert(hands[h].active == 0);
    }
    r = pad_compose(hands, &p);
    assert(!pad_usb_buttons(&r) && !r.trigger_level[0] && !r.trigger_level[1]);

    // Learned D-pad contact and its four physical direction clicks are independent.
    p.touch[0][0] = (touch_binding){4, WHOLE_BYTE, 64, 192, UNBOUND};
    pad_update_touch(&hands[0], p.touch[0]);
    assert(hands[0].active == 1 && pad_compose(hands, &p).bytes[2] == 8);
    const uint8_t hats[]={4,2,6,0};
    for(unsigned b=0;b<4;b++){
        hands[0].buttons=1U<<b;
        assert(pad_compose(hands,&p).bytes[2]==hats[b]);
    }

    // All new USB buttons have distinct bits. Hat and axis positions stay stable.
    for(unsigned a=PAD_LT;a<ACTION_COUNT;a++){
        memset(hands,0,sizeof(hands));
        bind_outputs(&p, 16 + 15, bit_for(a)); hands[1].buttons=0x8000;
        r=pad_compose(hands,&p);
        assert(pad_usb_buttons(&r)==(1U<<(a-PAD_LT+11)) && r.bytes[2]==8);
        assert(r.trigger_level[0] == (a == PAD_LT ? 255 : 0));
        assert(r.trigger_level[1] == (a == PAD_RT ? 255 : 0));
        hands[1].buttons=0;
        r=pad_compose(hands,&p);
        assert(!pad_usb_buttons(&r));
    }
    pad_defaults(&p);
    p.travel[0][0]=(press_binding){1,70,100};
    p.travel[1][1]=(press_binding){2,190,160};
    p.travel[1][2]=(press_binding){2,140,110};
    bind_outputs(&p, 32, bit_for(PAD_LT));
    bind_outputs(&p, 32 + 4, bit_for(PAD_R_GRAB));
    bind_outputs(&p, 32 + 5, bit_for(PAD_R_GRAB_FULL));
    memset(hands,0,sizeof(hands));
    pad_update_travel(&hands[1],p.travel[1]); assert(!hands[1].pressed_travel);
    uint8_t movement[10]={0x14,8};
    movement[8]=100; movement[9]=155;
    for(unsigned h=0;h<2;h++){
        assert(pad_decode(&hands[h],movement,sizeof(movement)));
        pad_update_travel(&hands[h],p.travel[h]);
    }
    r=pad_compose(hands,&p);
    assert(pad_usb_buttons(&r)==((1U<<11)|(1U<<14)));
    movement[9]=100;
    assert(pad_decode(&hands[1],movement,sizeof(movement)));
    pad_update_travel(&hands[1],p.travel[1]);
    r=pad_compose(hands,&p); assert(pad_usb_buttons(&r)&(1U<<17));
    movement[9]=195;
    assert(pad_decode(&hands[1],movement,sizeof(movement)));
    pad_update_travel(&hands[1],p.travel[1]); assert(!hands[1].pressed_travel);

    uint8_t v2[LEGACY_PROFILE_BYTES]={2,1};
    for(unsigned i=34;i<sizeof(v2);i+=5) v2[i]=NO_CHANNEL;
    v2[2+7]=PAD_HOME;
    uint8_t *old=v2+34;
    memcpy(old+9*5,(uint8_t[]){3,0,32,224,PAD_X},5); // retired thumb
    memcpy(old+12*5,(uint8_t[]){4,1,51,204,PAD_Y},5); // surviving ring finger
    memcpy(old,(uint8_t[]){5,0,64,192,PAD_DOWN},5); // retired direction touch
    memcpy(old+(14+0)*5,(uint8_t[]){6,0,192,64,PAD_START},5);
    memcpy(old+(14+7)*5,(uint8_t[]){7,0,32,224,PAD_HOME},5);
    memcpy(old+(14+8)*5,(uint8_t[]){7,0,64,192,PAD_SELECT},5); // conflicting field
    assert(pad_profile_load(&p,v2,sizeof(v2)));
    assert(outputs_at(&p, 7) == bit_for(PAD_HOME) && p.stick_flags == 1);
    assert(p.touch[0][0].channel==NO_CHANNEL && !p.touch[0][0].action);
    assert(!memcmp(&p.touch[0][7],old+12*5,sizeof(touch_binding)));
    assert(p.touch[0][5].channel==NO_CHANNEL && !p.touch[0][5].action);
    assert(!memcmp(&p.touch[1][0],old+14*5,sizeof(touch_binding)));
    assert(!memcmp(&p.touch[1][7],old+(14+7)*5,sizeof(touch_binding)));
    assert(p.touch[1][8].channel==NO_CHANNEL && p.touch[1][8].action==PAD_SELECT);
    pad_profile copy;
    assert(pad_profile_load(&copy,(const uint8_t *)&p,sizeof(p)) && !memcmp(&p,&copy,sizeof(p)));
    p.touch[0][9]=(touch_binding){9,0,20,90,PAD_A};
    assert(!pad_profile_valid(&p));
    // The alternate descriptor uses TinyUSB's axis order, 1..8 hat and contiguous buttons.
    pad_report extended = {.bytes = {0x81, 0x80, 8, 0, 0x80, 0xff, 0x7f, 0, 0, 0, 0x40, 3}};
    uint8_t standard[11];
    assert(pad_standard_report(&extended, standard) == sizeof(standard));
    assert(!memcmp(standard, (uint8_t[]){129,127,0,63,0,0,0,0x81,0x80,3,0}, sizeof(standard)));
    for (unsigned hat = 0; hat < 8; ++hat) {
        extended.bytes[2] = hat;
        pad_standard_report(&extended, standard);
        assert(standard[6] == hat + 1);
    }
    puts("Decoder, profile migration, calibration, analog trigger and USB report tests passed");
}
