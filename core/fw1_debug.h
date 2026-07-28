#ifndef HYPE_CORE_FW1_DEBUG_H
#define HYPE_CORE_FW1_DEBUG_H

/*
 * Compile-time gate for the FW-1 live-guest investigation diagnostics.
 *
 * The live-guest path accumulated a large amount of instrumentation while the
 * hard bugs were being chased -- PM-timer liveness traces, IOAPIC
 * redirection-entry timelines, interrupt-wedge dumps, RIP/IO histograms,
 * per-access PS/2 and MSR tracing. Each earned its keep at the time, and most
 * describe a *class* of bug that can recur, so they are worth keeping. But
 * several sit directly in hot paths (one fires on every ACPI PM-timer read, one
 * on every IOAPIC RTE write), and a guest that is already slow on real hardware
 * (#234, PERF-2) cannot afford them in a production boot.
 *
 * So: off by default, and enabled per-build with -DHYPE_FW1_DEBUG=1.
 *
 * Gate the WHOLE diagnostic block, not just its print. Several of these compute
 * something to report it -- a guest page-table walk to fetch instruction bytes,
 * a one-shot 1ms busy-wait to measure the PM timer's rate -- and leaving that
 * work in place while suppressing the output would keep the cost and lose the
 * benefit. HYPE_FW1_DBG() exists for the cases that really are just a print.
 *
 * Note also that a diagnostic reading vendor-specific state must still go
 * through the vmm_* shims and respect their "is this available on this backend"
 * return, even inside a debug block: enabling this on Intel must not start
 * reinterpreting a VMCS-backed context as though it were a VMCB.
 */
#ifndef HYPE_FW1_DEBUG
#define HYPE_FW1_DEBUG 0
#endif

#if HYPE_FW1_DEBUG
#define HYPE_FW1_DBG(...) hype_debug_print(__VA_ARGS__)
#else
/* Consumes the arguments without evaluating them, and compiles to nothing. */
#define HYPE_FW1_DBG(...)                                                                          \
    do {                                                                                           \
    } while (0)
#endif

#endif /* HYPE_CORE_FW1_DEBUG_H */
