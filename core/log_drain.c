#include "log_drain.h"

void hype_log_drain_init(hype_log_drain_t *drain, uint64_t now_tsc, uint64_t tsc_hz,
                         unsigned int burst_interval_secs, unsigned int slice_interval_ms) {
    if (drain == (hype_log_drain_t *)0) return;
    drain->burst_interval_tsc = tsc_hz * burst_interval_secs;
    drain->slice_interval_tsc = (tsc_hz / 1000u) * slice_interval_ms;
    if (slice_interval_ms != 0u && drain->slice_interval_tsc == 0u) {
        drain->slice_interval_tsc = 1u;
    }
    drain->next_burst_tsc = now_tsc + drain->burst_interval_tsc;
    drain->next_slice_tsc = 0u;
    drain->target = 0u;
    drain->active = 0;
}

int hype_log_drain_due(hype_log_drain_t *drain, uint64_t now_tsc,
                       unsigned int capture_len) {
    if (drain == (hype_log_drain_t *)0 || drain->burst_interval_tsc == 0u) return 0;
    if (!drain->active) {
        if (now_tsc < drain->next_burst_tsc) return 0;
        drain->active = 1;
        drain->target = capture_len;
        drain->next_slice_tsc = now_tsc;
    }
    return now_tsc >= drain->next_slice_tsc;
}

void hype_log_drain_record(hype_log_drain_t *drain, uint64_t now_tsc,
                           int made_progress, int reached_target) {
    if (drain == (hype_log_drain_t *)0 || !drain->active) return;
    if (reached_target || !made_progress) {
        drain->active = 0;
        drain->next_burst_tsc = now_tsc + drain->burst_interval_tsc;
        drain->next_slice_tsc = 0u;
        return;
    }
    drain->next_slice_tsc = now_tsc + drain->slice_interval_tsc;
}
