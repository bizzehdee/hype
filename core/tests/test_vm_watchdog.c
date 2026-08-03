#include <stdio.h>
#include <string.h>
#include "../vm_watchdog.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_shutdown_is_immediate(void) {
    hype_vm_watchdog_t w;
    hype_vm_watchdog_reset(&w);
    /* No benign version of a triple fault -- waiting for a second would only delay
     * the force-off. */
    CHECK_INT("shutdown faults at once", HYPE_VM_HEALTH_FAULTED_SHUTDOWN,
              hype_vm_watchdog_observe(&w, 0x7fu, 0x1000u, 0, 1));
}

static void test_handled_traffic_never_faults(void) {
    hype_vm_watchdog_t w;
    unsigned i;
    hype_vm_watchdog_reset(&w);
    /* A guest polling ONE port at ONE instruction forever is doing exactly what it
     * means to. Far past the storm threshold, and it must stay healthy -- a false
     * positive force-powers-off a working VM. */
    for (i = 0; i < HYPE_VM_WATCHDOG_STORM_THRESHOLD * 3u; i++) {
        if (hype_vm_watchdog_observe(&w, 30u, 0x2000u, 1, 0) != HYPE_VM_HEALTH_OK) {
            printf("FAIL: handled traffic faulted at %u\n", i);
            failures++;
            break;
        }
    }
}

static void test_unhandled_storm_at_one_rip(void) {
    hype_vm_watchdog_t w;
    unsigned i;
    hype_vm_health_t v = HYPE_VM_HEALTH_OK;
    hype_vm_watchdog_reset(&w);
    for (i = 0; i < HYPE_VM_WATCHDOG_STORM_THRESHOLD + 4u; i++) {
        v = hype_vm_watchdog_observe(&w, 0x400u, 0x3000u, 0, 0);
        if (v != HYPE_VM_HEALTH_OK) {
            break;
        }
    }
    CHECK_INT("storm detected", HYPE_VM_HEALTH_FAULTED_STORM, v);
    /* Not before the threshold: the threshold exists so a plausible retry burst does
     * not shoot a healthy guest. */
    CHECK_INT("fired at the threshold, not before", HYPE_VM_WATCHDOG_STORM_THRESHOLD - 1u, i);
}

static void test_unhandled_at_moving_rip_is_not_a_storm(void) {
    hype_vm_watchdog_t w;
    unsigned i;
    hype_vm_watchdog_reset(&w);
    /* Unhandled exits at DIFFERENT RIPs mean the guest is advancing through code hype
     * does not model -- a coverage gap to fix, not a hang to shoot. This is the case a
     * naive "count unhandled exits" watchdog gets wrong. */
    for (i = 0; i < HYPE_VM_WATCHDOG_STORM_THRESHOLD * 2u; i++) {
        if (hype_vm_watchdog_observe(&w, 0x400u, 0x4000u + i, 0, 0) != HYPE_VM_HEALTH_OK) {
            printf("FAIL: moving-RIP unhandled exits faulted at %u\n", i);
            failures++;
            break;
        }
    }
}

static void test_same_rip_different_reason_is_not_a_storm(void) {
    hype_vm_watchdog_t w;
    unsigned i;
    hype_vm_watchdog_reset(&w);
    /* One instruction producing alternating unhandled reasons is still movement of a
     * kind; only a single repeating (reason,rip) pair is a stuck guest. */
    for (i = 0; i < HYPE_VM_WATCHDOG_STORM_THRESHOLD * 2u; i++) {
        uint64_t reason = (i & 1u) ? 0x400u : 0x401u;
        if (hype_vm_watchdog_observe(&w, reason, 0x5000u, 0, 0) != HYPE_VM_HEALTH_OK) {
            printf("FAIL: alternating reasons faulted at %u\n", i);
            failures++;
            break;
        }
    }
}

static void test_progress_clears_the_run(void) {
    hype_vm_watchdog_t w;
    unsigned i;
    hype_vm_watchdog_reset(&w);
    /* Almost a storm, then one serviced exit: the guest moved, so the count must
     * restart. Otherwise a long-running guest accumulates its way to a false fault. */
    for (i = 0; i < HYPE_VM_WATCHDOG_STORM_THRESHOLD - 1u; i++) {
        hype_vm_watchdog_observe(&w, 0x400u, 0x6000u, 0, 0);
    }
    CHECK_INT("still healthy just short", HYPE_VM_HEALTH_OK,
              hype_vm_watchdog_observe(&w, 30u, 0x6004u, 1, 0));
    CHECK_INT("counter cleared by progress", 0, w.repeats);
    CHECK_INT("one more unhandled does not fault", HYPE_VM_HEALTH_OK,
              hype_vm_watchdog_observe(&w, 0x400u, 0x6000u, 0, 0));
}

static void test_verdict_latches(void) {
    hype_vm_watchdog_t w;
    hype_vm_watchdog_reset(&w);
    CHECK_INT("faulted", HYPE_VM_HEALTH_FAULTED_SHUTDOWN,
              hype_vm_watchdog_observe(&w, 0x7fu, 0x7000u, 0, 1));
    /* Latches, so the caller force-powers off exactly once and needs no debounce. */
    CHECK_INT("still faulted after a healthy exit", HYPE_VM_HEALTH_FAULTED_SHUTDOWN,
              hype_vm_watchdog_observe(&w, 30u, 0x7004u, 1, 0));
}

static void test_two_watchdogs_independent(void) {
    hype_vm_watchdog_t a, b;
    unsigned i;
    hype_vm_watchdog_reset(&a);
    hype_vm_watchdog_reset(&b);
    /* #171 says force off THAT VM only -- so one VM's fault must not condemn another. */
    for (i = 0; i < HYPE_VM_WATCHDOG_STORM_THRESHOLD + 1u; i++) {
        hype_vm_watchdog_observe(&a, 0x400u, 0x8000u, 0, 0);
    }
    CHECK_INT("a faulted", HYPE_VM_HEALTH_FAULTED_STORM, a.verdict);
    CHECK_INT("b untouched", HYPE_VM_HEALTH_OK,
              hype_vm_watchdog_observe(&b, 30u, 0x8000u, 1, 0));
}

static void test_health_strings(void) {
    hype_vm_health_t all[] = {HYPE_VM_HEALTH_OK, HYPE_VM_HEALTH_FAULTED_SHUTDOWN,
                              HYPE_VM_HEALTH_FAULTED_STORM};
    unsigned i;
    for (i = 0; i < 3u; i++) {
        const char *s = hype_vm_health_str(all[i]);
        if (s == 0 || s[0] == '\0' || strcmp(s, "unknown") == 0) {
            printf("FAIL: health %u has no message\n", (unsigned)all[i]);
            failures++;
        }
    }
    CHECK_INT("out-of-range still returns a string", 0,
              hype_vm_health_str((hype_vm_health_t)99) == 0);
}

int main(void) {
    test_shutdown_is_immediate();
    test_handled_traffic_never_faults();
    test_unhandled_storm_at_one_rip();
    test_unhandled_at_moving_rip_is_not_a_storm();
    test_same_rip_different_reason_is_not_a_storm();
    test_progress_clears_the_run();
    test_verdict_latches();
    test_two_watchdogs_independent();
    test_health_strings();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
