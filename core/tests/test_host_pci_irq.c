#include <stdio.h>
#include "../host_pci_irq.h"

static int failures;

#define CHECK(desc, cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", (desc));                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                            \
        if (e_ != a_) {                                                                            \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), e_, a_);                         \
            failures++;                                                                            \
        }                                                                                           \
    } while (0)

static void test_sched(void) {
    hype_poll_sched_t sched;

    hype_poll_sched_init(&sched, 32u);
    CHECK("freshly-initialised scheduler has no pending work", !hype_poll_sched_should_run(&sched));

    hype_poll_sched_mark_pending(&sched);
    CHECK("marking pending makes should_run true", hype_poll_sched_should_run(&sched));

    /* drained everything this call: pending clears */
    hype_poll_sched_after_run(&sched, 5u, 0);
    CHECK("draining with no more work clears pending", !hype_poll_sched_should_run(&sched));

    /* budget-limited: reclaimed some, but more is queued -- must keep running */
    hype_poll_sched_mark_pending(&sched);
    hype_poll_sched_after_run(&sched, 32u, 1);
    CHECK("more work queued keeps pending set", hype_poll_sched_should_run(&sched));

    /* reclaimed nothing at all: nothing to keep chasing, even if the caller
     * (wrongly) still claims more_work -- an empty poll must not spin forever */
    hype_poll_sched_after_run(&sched, 0u, 1);
    CHECK("zero work done clears pending regardless of more_work", !hype_poll_sched_should_run(&sched));
}

static unsigned int g_calls_a, g_calls_b;

static unsigned int poll_a(void *ctx) {
    (void)ctx;
    g_calls_a++;
    return 3u;
}

static unsigned int poll_b(void *ctx) {
    (void)ctx;
    g_calls_b++;
    return 7u;
}

static void test_registry(void) {
    unsigned int total;
    int i, ok;

    hype_host_poll_reset();
    g_calls_a = g_calls_b = 0;

    CHECK("registering a NULL poll_fn is refused", !hype_host_poll_register(0, 0));

    CHECK("first registration succeeds", hype_host_poll_register(poll_a, 0));
    CHECK("second registration succeeds", hype_host_poll_register(poll_b, 0));

    total = hype_host_poll_run_all();
    CHECK_INT("run_all drains every registered poller once", 10, total);
    CHECK_INT("poller a ran once", 1, g_calls_a);
    CHECK_INT("poller b ran once", 1, g_calls_b);

    total = hype_host_poll_run_all();
    CHECK_INT("a second run_all drains again", 10, total);
    CHECK_INT("poller a ran twice total", 2, g_calls_a);

    hype_host_poll_reset();
    CHECK_INT("reset leaves nothing to drain", 0, hype_host_poll_run_all());

    /* fill the registry to its cap, then confirm the next one is refused */
    ok = 1;
    for (i = 0; i < HYPE_HOST_POLL_MAX_DEVICES; i++) {
        ok = ok && hype_host_poll_register(poll_a, 0);
    }
    CHECK("filling the registry to its cap succeeds", ok);
    CHECK("one more registration past the cap is refused", !hype_host_poll_register(poll_a, 0));

    hype_host_poll_reset();
}

static void test_sched_null_guards(void) {
    /* Every scheduler entry point must tolerate a NULL sched rather than dereference it. */
    hype_poll_sched_init(0, 8u);
    hype_poll_sched_mark_pending(0);
    hype_poll_sched_after_run(0, 1u, 1);
    CHECK("should_run on a NULL sched is false", !hype_poll_sched_should_run(0));
}

int main(void) {
    test_sched();
    test_registry();
    test_sched_null_guards();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
