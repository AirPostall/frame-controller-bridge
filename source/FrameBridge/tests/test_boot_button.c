#include <assert.h>
#include <stdio.h>
#include "boot_button.h"

int main(void)
{
    boot_button b = {0};
    assert(boot_next_wake(&b, INT64_MAX) == INT64_MAX);
    // Bounce shorter than the debounce interval is ignored.
    assert(boot_button_sample(&b, true, 10) == BOOT_NONE);
    assert(boot_next_wake(&b, INT64_MAX) == 10 + BOOT_DEBOUNCE_MS);
    assert(boot_button_sample(&b, false, 15) == BOOT_NONE);
    assert(boot_button_sample(&b, false, 80) == BOOT_NONE);

    assert(boot_button_sample(&b, true, 100) == BOOT_NONE);
    assert(boot_button_sample(&b, true, 100 + BOOT_DEBOUNCE_MS) == BOOT_NONE);
    assert(boot_next_wake(&b, INT64_MAX) == 100 + BOOT_MODE_HOLD_MS);
    assert(boot_button_sample(&b, false, 200) == BOOT_NONE);
    assert(boot_button_sample(&b, true, 202) == BOOT_NONE);
    assert(boot_button_sample(&b, false, 205) == BOOT_NONE);
    assert(boot_button_sample(&b, false, 205 + BOOT_DEBOUNCE_MS) == BOOT_TAP);
    assert(boot_button_sample(&b, false, 300) == BOOT_NONE);

    // An open Wi-Fi session can expire partway through a Bluetooth hold.
    const int64_t wifi_expires = 300 + SETUP_WIFI_MS;
    int64_t start = wifi_expires - 1000;
    assert(boot_button_sample(&b, true, start) == BOOT_NONE);
    assert(boot_button_sample(&b, true, start + BOOT_DEBOUNCE_MS) == BOOT_NONE);
    assert(boot_next_wake(&b, wifi_expires) == wifi_expires);
    assert(boot_button_sample(&b, true, wifi_expires) == BOOT_NONE);
    assert(boot_next_wake(&b, INT64_MAX) == start + BOOT_MODE_HOLD_MS);
    assert(boot_button_sample(&b, true, start + BOOT_MODE_HOLD_MS) == BOOT_HOLD);
    assert(boot_next_wake(&b, INT64_MAX) == INT64_MAX);
    assert(boot_button_sample(&b, true, start + 5000) == BOOT_NONE);
    assert(boot_button_sample(&b, false, start + 6000) == BOOT_NONE);
    assert(boot_button_sample(&b, false, start + 6100) == BOOT_NONE);

    // Classify a release by the edge time when debounce finishes after the cutoff.
    start += 10000;
    assert(boot_button_sample(&b, true, start) == BOOT_NONE);
    assert(boot_button_sample(&b, true, start + BOOT_DEBOUNCE_MS) == BOOT_NONE);
    assert(boot_button_sample(&b, false, start + BOOT_MODE_HOLD_MS - 1) == BOOT_NONE);
    assert(boot_button_sample(&b, false, start + BOOT_MODE_HOLD_MS + 50) == BOOT_TAP);

    b = (boot_button){.raw_down=true, .down=true, .hold_sent=true};
    assert(boot_button_sample(&b, true, 5000) == BOOT_NONE);
    assert(boot_button_sample(&b, false, 5100) == BOOT_NONE);
    assert(boot_button_sample(&b, false, 5200) == BOOT_NONE);

    // Two distinct long presses must each toggle once, even when held far beyond
    // the threshold. Neither release should also request setup Wi-Fi.
    for (int i = 0; i < 2; ++i) {
        start = 6000 + i * 12000;
        assert(boot_button_sample(&b, true, start) == BOOT_NONE);
        assert(boot_button_sample(&b, true, start + BOOT_DEBOUNCE_MS) == BOOT_NONE);
        assert(boot_button_sample(&b, true, start + BOOT_MODE_HOLD_MS) == BOOT_HOLD);
        assert(boot_button_sample(&b, true, start + 9000) == BOOT_NONE);
        assert(boot_button_sample(&b, false, start + 9100) == BOOT_NONE);
        assert(boot_button_sample(&b, false, start + 9100 + BOOT_DEBOUNCE_MS) == BOOT_NONE);
    }
    puts("BOOT tap, repeated mode holds, bounce, Wi-Fi deadline and reset cases passed.");
}
