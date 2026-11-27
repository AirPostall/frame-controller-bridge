#include "report_queue.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void consume(report_queue *queue, const pad_report *expected)
{
    uint32_t sequence;
    pad_report report;
    assert(report_queue_peek(queue, &report, &sequence));
    assert(memcmp(&report, expected, sizeof(report)) == 0);
    report_queue_sent(queue, sequence);
}

int main(void)
{
    report_queue usb = {0}, bluetooth = {0};
    pad_report neutral = {.bytes = {[2] = 8}};
    pad_report pressed = neutral;
    pressed.bytes[0] = 1;
    const pad_report half_pull = {.bytes = {[2] = 8}, .trigger_level = {123, 0}};
    const pad_report *changes[] = {&pressed, &neutral, &half_pull, &neutral};
    uint32_t pending;
    pad_report report;

    // Bluetooth backpressure retains both edges of a quick tap.
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i) {
        assert(!report_queue_push(&bluetooth, changes[i]));
    }
    assert(!report_queue_peek(&usb, &report, &pending));
    for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); ++i)
        consume(&bluetooth, changes[i]);
    assert(!report_queue_peek(&bluetooth, &report, &pending));

    // A route change discards stale work even if transmission was in progress.
    // The old host gets only a release, the newly selected one the held state.
    assert(!report_queue_push(&bluetooth, &pressed));
    assert(report_queue_peek(&bluetooth, &report, &pending));
    report_queue_sync(&bluetooth, true, &neutral);
    report_queue_sync(&usb, true, &pressed);
    report_queue_sent(&bluetooth, pending);
    consume(&bluetooth, &neutral);
    consume(&usb, &pressed);

    report_queue_sync(&usb, true, &neutral);
    report_queue_sync(&bluetooth, false, &pressed); // Still pairing
    consume(&usb, &neutral);
    assert(!report_queue_peek(&bluetooth, &report, &pending));
    report_queue_sync(&bluetooth, true, &half_pull); // New connection gets the current state
    consume(&bluetooth, &half_pull);

    // Saturation during a stalled link drops old states and keeps the latest
    // release, including when a sequence number crosses its 32-bit wrap point.
    bluetooth.first = bluetooth.end = UINT32_MAX - 2;
    for (size_t i = 0; i < sizeof(bluetooth.reports) / sizeof(bluetooth.reports[0]); ++i)
        assert(!report_queue_push(&bluetooth, &pressed));
    assert(report_queue_peek(&bluetooth, &report, &pending));
    assert(report_queue_push(&bluetooth, &neutral));
    report_queue_sent(&bluetooth, pending);
    consume(&bluetooth, &neutral);
    assert(!report_queue_peek(&bluetooth, &report, &pending));

    // Slow hosts can have a sample in flight while the selected hand sends
    // many more notifications. Reserve at most one follow-up wake, preserving
    // the queued press and release for the sender to overlay with fresh motion.
    report_queue samples = {.first = UINT32_MAX, .end = UINT32_MAX};
    assert(report_queue_request_sample(&samples, &neutral));
    assert(report_queue_peek(&samples, &report, &pending));
    assert(report_queue_request_sample(&samples, &neutral));
    for (unsigned i = 0; i < REPORT_QUEUE_SIZE * 2; ++i)
        assert(!report_queue_request_sample(&samples, &neutral));
    assert(!report_queue_push(&samples, &pressed));
    assert(!report_queue_request_sample(&samples, &pressed));
    assert(!report_queue_push(&samples, &neutral));
    report_queue_sent(&samples, pending);
    consume(&samples, &neutral);
    consume(&samples, &pressed);
    consume(&samples, &neutral);
    assert(!report_queue_peek(&samples, &report, &pending));

    puts("Report queues preserve taps, motion wakeups, route changes and reconnect snapshots.");
    return 0;
}
