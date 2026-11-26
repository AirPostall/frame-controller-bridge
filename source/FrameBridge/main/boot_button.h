#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { BOOT_DEBOUNCE_MS = 35, BOOT_MODE_HOLD_MS = 3000, SETUP_WIFI_MS = 30 * 60 * 1000 };
typedef enum { BOOT_NONE, BOOT_TAP, BOOT_HOLD } boot_action;

typedef struct {
    bool raw_down, down, hold_sent;
    int64_t edge_ms, press_ms;
} boot_button;

static inline boot_action boot_button_sample(boot_button *b, bool down, int64_t now)
{
    if (down != b->raw_down) {
        b->raw_down = down;
        b->edge_ms = now;
    }
    if (down != b->down && now - b->edge_ms >= BOOT_DEBOUNCE_MS) {
        b->down = down;
        if (down) {
            b->press_ms = b->edge_ms;
            b->hold_sent = false;
        } else if (!b->hold_sent) {
            return b->edge_ms - b->press_ms >= BOOT_MODE_HOLD_MS ? BOOT_HOLD : BOOT_TAP;
        }
    }
    if (b->down && down && !b->hold_sent && now - b->press_ms >= BOOT_MODE_HOLD_MS) {
        b->hold_sent = true;
        return BOOT_HOLD;
    }
    return BOOT_NONE;
}

// GPIO edges wake the task too. INT64_MAX means there is no timed wakeup.
static inline int64_t boot_next_wake(const boot_button *b, int64_t wifi_deadline)
{
    int64_t deadline = INT64_MAX;
    if (b->raw_down != b->down) deadline = b->edge_ms + BOOT_DEBOUNCE_MS;
    else if (b->down && !b->hold_sent) deadline = b->press_ms + BOOT_MODE_HOLD_MS;
    return wifi_deadline < deadline ? wifi_deadline : deadline;
}
