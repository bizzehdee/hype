#include <stdint.h>
#include <stdio.h>

#include "../log_drain.h"

static int failures;

#define CHECK(msg, cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_three_second_burst_with_bounded_slices(void) {
    hype_log_drain_t drain;
    const uint64_t hz = 1000000u;

    hype_log_drain_init(&drain, 100u, hz, 3u, 10u);
    CHECK("initial state is idle", !drain.active);
    CHECK("no immediate post-setup drain", !hype_log_drain_due(&drain, 100u, 900u));
    CHECK("still idle before three seconds",
          !hype_log_drain_due(&drain, 3000099u, 1000u));
    CHECK("burst starts at three seconds",
          hype_log_drain_due(&drain, 3000100u, 1000u));
    CHECK("burst captures a fixed target", drain.target == 1000u);

    hype_log_drain_record(&drain, 3000100u, 1, 0);
    CHECK("next slice waits ten milliseconds",
          !hype_log_drain_due(&drain, 3010099u, 5000u));
    CHECK("next slice becomes due at ten milliseconds",
          hype_log_drain_due(&drain, 3010100u, 5000u));
    CHECK("new records do not extend the active target", drain.target == 1000u);

    hype_log_drain_record(&drain, 3010100u, 1, 1);
    CHECK("caught-up burst becomes idle", !drain.active);
    CHECK("next burst waits another three seconds",
          !hype_log_drain_due(&drain, 6010099u, 6000u));
    CHECK("next burst captures the later log length",
          hype_log_drain_due(&drain, 6010100u, 6000u) && drain.target == 6000u);
}

static void test_no_progress_ends_partial_record_burst(void) {
    hype_log_drain_t drain;
    hype_log_drain_init(&drain, 0u, 1000u, 3u, 10u);
    CHECK("partial-record burst starts", hype_log_drain_due(&drain, 3000u, 25u));
    hype_log_drain_record(&drain, 3000u, 0, 0);
    CHECK("stalled partial record ends burst", !drain.active);
    CHECK("stalled burst does not spin at 100 Hz",
          !hype_log_drain_due(&drain, 3010u, 25u));
}

static void test_busy_producer_does_not_force_continuous_metadata_updates(void) {
    hype_log_drain_t drain;
    uint64_t now;
    unsigned int capture_len = 0u;
    unsigned int slices = 0u;

    hype_log_drain_init(&drain, 0u, 1000u, 3u, 10u);
    for (now = 0u; now <= 45000u; now += 10u) {
        capture_len += 40u; /* four KiB/s of continuous guest diagnostics */
        if (hype_log_drain_due(&drain, now, capture_len)) {
            slices++;
            /* This rate fits in one 4 KiB slice per sink at each burst. */
            hype_log_drain_record(&drain, now, 1, 1);
        }
    }
    CHECK("45-second busy producer causes 15 bursts, not 4501 slice attempts", slices == 15u);
}

static void test_invalid_inputs_are_safe(void) {
    hype_log_drain_t drain;
    hype_log_drain_init(0, 0u, 1000u, 3u, 10u);
    hype_log_drain_init(&drain, 0u, 0u, 3u, 10u);
    CHECK("unknown TSC rate disables scheduler", !hype_log_drain_due(&drain, UINT64_MAX, 1u));
    CHECK("null scheduler is never due", !hype_log_drain_due(0, UINT64_MAX, 1u));
    hype_log_drain_init(&drain, 0u, 1000u, 3u, 0u);
    CHECK("zero slice interval remains zero", drain.slice_interval_tsc == 0u);
    hype_log_drain_record(&drain, 0u, 1, 1);
    hype_log_drain_record(0, 0u, 1, 1);
}

int main(void) {
    test_three_second_burst_with_bounded_slices();
    test_no_progress_ends_partial_record_burst();
    test_busy_producer_does_not_force_continuous_metadata_updates();
    test_invalid_inputs_are_safe();
    if (failures != 0) return 1;
    puts("log_drain tests: ok");
    return 0;
}
