#pragma once
#include "decoder.h"

enum { REPORT_QUEUE_SIZE = 32 };

typedef struct {
    pad_report reports[REPORT_QUEUE_SIZE];
    uint32_t first, end;
} report_queue;

// Keep indices monotonic across detach and overflow. A late completion can only
// acknowledge the entry it actually sent.
static inline void report_queue_reset(report_queue *q)
{
    q->first = q->end;
}

static inline bool report_queue_push(report_queue *q, const pad_report *report)
{
    bool full = q->end - q->first == REPORT_QUEUE_SIZE;
    if (full) report_queue_reset(q);
    q->reports[q->end++ % REPORT_QUEUE_SIZE] = *report;
    return full;
}

static inline void report_queue_sync(report_queue *q, bool online, const pad_report *snapshot)
{
    report_queue_reset(q);
    if (online) report_queue_push(q, snapshot);
}

static inline bool report_queue_peek(const report_queue *q, pad_report *report, uint32_t *seq)
{
    if (q->first == q->end) return false;
    *seq = q->first;
    *report = q->reports[q->first % REPORT_QUEUE_SIZE];
    return true;
}

static inline void report_queue_sent(report_queue *q, uint32_t seq)
{
    if (q->first == seq && q->first != q->end) ++q->first;
}
