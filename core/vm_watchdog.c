#include "vm_watchdog.h"

void hype_vm_watchdog_reset(hype_vm_watchdog_t *w) {
    w->last_reason = 0;
    w->last_rip = 0;
    w->repeats = 0;
    w->verdict = HYPE_VM_HEALTH_OK;
}

hype_vm_health_t hype_vm_watchdog_observe(hype_vm_watchdog_t *w, uint64_t reason, uint64_t rip,
                                          int handled, int is_shutdown) {
    if (w->verdict != HYPE_VM_HEALTH_OK) {
        return w->verdict; /* latched: the caller already acted */
    }

    /* A shutdown/triple-fault exit needs no corroboration -- there is no benign
     * version of it, and waiting for a second one would just delay the force-off. */
    if (is_shutdown) {
        w->verdict = HYPE_VM_HEALTH_FAULTED_SHUTDOWN;
        return w->verdict;
    }

    /*
     * A HANDLED exit is normal traffic no matter how often it repeats -- a guest
     * polling one I/O port in a tight loop is doing exactly what it means to. Treat it
     * as progress and clear the counter, otherwise the watchdog would eventually shoot
     * a healthy guest.
     */
    if (handled) {
        w->repeats = 0;
        w->last_reason = reason;
        w->last_rip = rip;
        return HYPE_VM_HEALTH_OK;
    }

    /*
     * Unhandled. Only count it as a storm if it is the SAME exit at the SAME
     * instruction: unhandled exits at moving RIPs are a guest advancing through code
     * hype does not model (a coverage gap to fix, not a hang to shoot).
     */
    if (reason == w->last_reason && rip == w->last_rip) {
        if (w->repeats < 0xFFFFFFFFu) {
            w->repeats++;
        }
        if (w->repeats >= HYPE_VM_WATCHDOG_STORM_THRESHOLD) {
            w->verdict = HYPE_VM_HEALTH_FAULTED_STORM;
            return w->verdict;
        }
        return HYPE_VM_HEALTH_OK;
    }

    /* A different unhandled exit: the guest moved. Start counting again from this one
     * rather than from zero -- this exit is itself the first of a possible new run. */
    w->last_reason = reason;
    w->last_rip = rip;
    w->repeats = 1;
    return HYPE_VM_HEALTH_OK;
}

const char *hype_vm_health_str(hype_vm_health_t h) {
    switch (h) {
        case HYPE_VM_HEALTH_OK: return "ok";
        case HYPE_VM_HEALTH_FAULTED_SHUTDOWN: return "faulted: shutdown/triple fault";
        case HYPE_VM_HEALTH_FAULTED_STORM: return "faulted: unhandled-exit storm at one RIP";
        default: return "unknown";
    }
}
