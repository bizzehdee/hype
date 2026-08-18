#ifndef HYPE_CORE_LOG_DRAIN_H
#define HYPE_CORE_LOG_DRAIN_H

#include <stdint.h>

/*
 * #377: schedule bounded USB-log drain bursts.
 *
 * The three-second interval limits how often FAT directory metadata changes.
 * Once a burst starts, short slices keep dashboard and input latency bounded.
 * target is a snapshot. New capture bytes wait for the next burst, so a busy
 * producer cannot keep the filesystem writer active continuously.
 */
typedef struct {
    uint64_t burst_interval_tsc;
    uint64_t slice_interval_tsc;
    uint64_t next_burst_tsc;
    uint64_t next_slice_tsc;
    unsigned int target;
    int active;
} hype_log_drain_t;

void hype_log_drain_init(hype_log_drain_t *drain, uint64_t now_tsc, uint64_t tsc_hz,
                         unsigned int burst_interval_secs, unsigned int slice_interval_ms);

/* Returns 1 when one bounded slice should run now. */
int hype_log_drain_due(hype_log_drain_t *drain, uint64_t now_tsc,
                       unsigned int capture_len);

/* #522: 1 while a burst is in progress. Lets a caller tell a burst's FIRST slice from
 * its later ones, and so count why each burst ended -- caught up, or stalled. */
static inline int hype_log_drain_active(const hype_log_drain_t *drain) {
    return drain != (const hype_log_drain_t *)0 && drain->active;
}

/* Finish a due slice. A caught-up or stalled burst returns to the long cadence. */
void hype_log_drain_record(hype_log_drain_t *drain, uint64_t now_tsc,
                           int made_progress, int reached_target);

#endif /* HYPE_CORE_LOG_DRAIN_H */
