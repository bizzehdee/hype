#include "host_pci_irq.h"

void hype_poll_sched_init(hype_poll_sched_t *sched, unsigned int budget) {
    if (sched == 0) {
        return;
    }
    sched->pending = 0;
    sched->budget = budget;
}

void hype_poll_sched_mark_pending(hype_poll_sched_t *sched) {
    if (sched == 0) {
        return;
    }
    sched->pending = 1;
}

int hype_poll_sched_should_run(const hype_poll_sched_t *sched) {
    if (sched == 0) {
        return 0;
    }
    return sched->pending != 0;
}

void hype_poll_sched_after_run(hype_poll_sched_t *sched, unsigned int work_done, int more_work) {
    if (sched == 0) {
        return;
    }
    if (work_done == 0u || more_work == 0) {
        sched->pending = 0;
    }
}

typedef struct {
    hype_host_poll_fn poll_fn;
    void *ctx;
} hype_host_poll_entry_t;

static hype_host_poll_entry_t g_pollers[HYPE_HOST_POLL_MAX_DEVICES];
static unsigned int g_poller_count;

int hype_host_poll_register(hype_host_poll_fn poll_fn, void *ctx) {
    if (poll_fn == 0 || g_poller_count >= HYPE_HOST_POLL_MAX_DEVICES) {
        return 0;
    }
    g_pollers[g_poller_count].poll_fn = poll_fn;
    g_pollers[g_poller_count].ctx = ctx;
    g_poller_count++;
    return 1;
}

unsigned int hype_host_poll_run_all(void) {
    unsigned int i, total = 0;
    for (i = 0; i < g_poller_count; i++) {
        total += g_pollers[i].poll_fn(g_pollers[i].ctx);
    }
    return total;
}

void hype_host_poll_reset(void) {
    g_poller_count = 0;
}
