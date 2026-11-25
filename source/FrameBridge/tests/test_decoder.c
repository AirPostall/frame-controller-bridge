#include "../main/decoder.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_sticks(const pad_report *report, const int16_t expected[4])
{
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *axis = &report->bytes[3 + i * 2];
        assert((axis[0] | (axis[1] << 8)) == (uint16_t)expected[i]);
    }
}

static void test_touch_triggers(void)
{
    pad_profile p = pad_defaults();
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
    p.binding[0][0] = PAD_LT;
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

static void test_v3_profile_upgrade(void)
{
    pad_profile p = pad_defaults();
    p.stick_flags = 3;
    p.binding[1][14] = PAD_R_GRAB_FULL;
    p.travel[1][1] = (press_binding){2, 170, 42};
    p.touch[1][11] = (touch_binding){4, HIGH_NIBBLE, 51, 204, PAD_X};
    uint8_t v3[V3_PROFILE_BYTES];
    memcpy(v3, &p, sizeof(v3));
    v3[0] = 3;
    pad_profile destination = {.trigger_source = {1, 13}};
    assert(pad_profile_load(&destination, v3, sizeof(v3)));
    assert(!memcmp(&destination, &p, sizeof(p)));
    destination.trigger_source[1] = 24;
    assert(pad_profile_valid(&destination));
    pad_profile saved = destination;
    assert(pad_profile_load(&destination, (const uint8_t *)&saved, sizeof(saved)));
    assert(!memcmp(&destination, &saved, sizeof(saved)));
    v3[2] = ACTION_COUNT;
    assert(!pad_profile_load(&destination, v3, sizeof(v3)));
    assert(!memcmp(&destination, &saved, sizeof(saved)));
    assert(!pad_profile_load(&destination, v3, 0));
    assert(!pad_profile_load(&destination, (const uint8_t *)&saved, sizeof(saved) - 1));
}

int main(void)
{
    test_touch_triggers();
    test_v3_profile_upgrade();
    pad_profile profile = pad_defaults();
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
    profile.binding[1][1] = PAD_RIGHT;
    assert(pad_compose(hands,&profile).bytes[2] == 3);  // down-right
    profile.binding[1][1] = PAD_UP;
    assert(pad_compose(hands,&profile).bytes[2] == 8);  // up and down cancel
    hands[0] = (hand_state){0};                         // a hand disconnected
    assert(pad_compose(hands,&profile).bytes[2] == 0);  // other hand preserved
    profile.binding[1][1] = PAD_HOME;
    assert(pad_compose(hands,&profile).bytes[1] == 4);
    profile.binding[1][1] = 99;
    assert(!pad_profile_valid(&profile));
    pad_profile p = pad_defaults();
    assert(p.binding[LEFT_HAND][9] == PAD_SELECT && p.binding[LEFT_HAND][8] == PAD_START);
    assert(p.binding[RIGHT_HAND][9] == PAD_HOME && p.binding[RIGHT_HAND][2] == PAD_X);
    uint8_t legacy[35] = {1, 3};
    legacy[2 + 5] = PAD_X;
    legacy[34] = PAD_UP;
    assert(pad_profile_load(&p, legacy, sizeof(legacy)) && p.version == PROFILE_VERSION);
    assert(p.binding[0][5] == PAD_X && p.binding[0][0] == UNBOUND);
    assert(p.touch[1][0].action == PAD_UP && p.stick_flags == 3);
    assert(p.touch[1][0].channel == NO_CHANNEL);
    legacy[34] = 220;
    assert(!pad_profile_load(&p, legacy, sizeof(legacy)) && p.touch[1][0].action == PAD_UP);
    p = pad_defaults();
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
        p.binding[1][15]=a; hands[1].buttons=0x8000;
        r=pad_compose(hands,&p);
        assert(pad_usb_buttons(&r)==(1U<<(a-PAD_LT+11)) && r.bytes[2]==8);
        assert(r.trigger_level[0] == (a == PAD_LT ? 255 : 0));
        assert(r.trigger_level[1] == (a == PAD_RT ? 255 : 0));
        hands[1].buttons=0;
        r=pad_compose(hands,&p);
        assert(!pad_usb_buttons(&r));
    }
    p = pad_defaults();
    p.travel[0][0]=(press_binding){1,70,100};
    p.travel[1][1]=(press_binding){2,190,160};
    p.travel[1][2]=(press_binding){2,140,110};
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
    assert(p.binding[0][7]==PAD_HOME && p.stick_flags==1);
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
