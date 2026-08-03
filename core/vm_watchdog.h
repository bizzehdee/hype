#ifndef HYPE_CORE_VM_WATCHDOG_H
#define HYPE_CORE_VM_WATCHDOG_H

#include <stdint.h>

/*
 * M8-8 (#171): per-vCPU liveness watchdog -- detect a GENUINELY FAULTED guest and
 * force it off, without touching the other VMs.
 *
 * The distinction that makes this hard, and that the existing M4-6b2 idle detector
 * deliberately does not draw: **idle is not faulted**. A guest sitting at a login
 * prompt does no device work for minutes and is perfectly healthy. So this watchdog
 * keys on FAULT SIGNATURES, never on absence of progress:
 *
 *   1. A shutdown/triple-fault exit -- the CPU telling us the guest is unrecoverable.
 *      One is enough; there is no benign version of it.
 *   2. An UNHANDLED exit repeating at the SAME guest RIP. Both halves matter: a
 *      handled exit is normal traffic however frequent, and thousands of unhandled
 *      exits at DIFFERENT RIPs is a guest making progress through code hype does not
 *      model, which is a portability gap rather than a hang. The same unmodelled exit
 *      at one instruction, forever, is a guest that cannot advance.
 *
 * As #171 says: this is a liveness detector, NOT a substitute for VALID-1..4 input
 * validation. It catches a guest that has stopped being able to run; it catches
 * nothing about a guest that is running and lying.
 *
 * Pure state machine -- no clock, no VM knowledge, no I/O -- so it unit-tests.
 */

/*
 * How many consecutive unhandled exits at one RIP before declaring a storm.
 *
 * Deliberately large. A false positive force-powers-off a healthy VM, which is
 * strictly worse than taking a few extra milliseconds to notice a real hang: the
 * guest is already stuck and nobody is waiting on the watchdog's latency. Sized so
 * that a plausible burst of retries at a single instruction cannot reach it, but a
 * genuine loop reaches it near-instantly (hype sees these at exit rate).
 */
#define HYPE_VM_WATCHDOG_STORM_THRESHOLD 4096u

typedef enum {
    HYPE_VM_HEALTH_OK = 0,
    HYPE_VM_HEALTH_FAULTED_SHUTDOWN, /* triple fault / shutdown exit */
    HYPE_VM_HEALTH_FAULTED_STORM     /* unhandled exit storm at one RIP */
} hype_vm_health_t;

typedef struct {
    uint64_t last_reason;
    uint64_t last_rip;
    uint32_t repeats;         /* consecutive unhandled (reason,rip) pairs */
    hype_vm_health_t verdict; /* latches once faulted */
} hype_vm_watchdog_t;

void hype_vm_watchdog_reset(hype_vm_watchdog_t *w);

/*
 * Record one VM exit.
 *
 * `handled` is whether hype's dispatch actually serviced this exit. `is_shutdown` is
 * whether the exit is the architectural shutdown/triple-fault reason for this backend
 * (the caller knows its own encoding; this module stays vendor-neutral).
 *
 * Returns the verdict. Once faulted it LATCHES -- the caller force-powers the VM off
 * once and must not need to debounce.
 */
hype_vm_health_t hype_vm_watchdog_observe(hype_vm_watchdog_t *w, uint64_t reason, uint64_t rip,
                                          int handled, int is_shutdown);

/* Human-readable verdict, for the log line that accompanies a force-off. */
const char *hype_vm_health_str(hype_vm_health_t h);

#endif /* HYPE_CORE_VM_WATCHDOG_H */
