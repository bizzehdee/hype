#ifndef HYPE_CORE_HOST_PCI_IRQ_H
#define HYPE_CORE_HOST_PCI_IRQ_H

/*
 * HNET-3 (#400): shared host-PCI-device interrupt/poll model, per plan.md §10
 * decision 34 -- part of the same shared facility as host_pci_dma.h. NICs are
 * the first consumer; storage HBAs may adopt the poll-scheduling half (#426).
 *
 * hype's existing host storage drivers are deliberately POLLED already --
 * host_pci.h's hype_host_pci_disable_interrupts() authoritatively silences a
 * host controller at the PCI level specifically so hype never fields an
 * interrupt from it. That stays true here: this module's poll-scheduling
 * logic (pending flag, budget, re-arm) is a NAPI-style bookkeeping helper
 * over a driver's own poll routine, not a new interrupt-delivery mechanism.
 * A real IRQ vector, where a driver wants one, is optional and layered on
 * top by scheduling a poll -- it never bypasses the poll path. A pure-poll
 * driver that never wires a vector at all works unchanged, exactly like AHCI/
 * NVMe/xHCI do today.
 *
 * Every function here is pure logic over caller-owned state -- no MMIO, no
 * IDT/vector installation (that stays arch-specific and lives with whichever
 * driver's hw shim wires a vector, same as every other hardware-touching
 * concern in this codebase).
 */

typedef struct {
    int pending;          /* work may be waiting: a poll was requested since the last drain */
    unsigned int budget;  /* max descriptors one hype_poll_run() call may reclaim */
} hype_poll_sched_t;

/* Initializes a scheduler with the given per-call budget (0 means "no limit
 * enforced here" -- the caller's own poll loop still bounds itself some other
 * way, e.g. ring-empty). */
void hype_poll_sched_init(hype_poll_sched_t *sched, unsigned int budget);

/* Marks work pending -- called from an IRQ handler (once a vector exists) or
 * whenever a caller knows new work was queued (e.g. right after a submit). */
void hype_poll_sched_mark_pending(hype_poll_sched_t *sched);

/* 1 if a poll should run right now (pending work, or the caller always polls
 * unconditionally and wants this to just say "yes"). */
int hype_poll_sched_should_run(const hype_poll_sched_t *sched);

/*
 * Called after a poll routine reclaims `work_done` descriptors. Clears
 * `pending` only when `work_done` is 0 (nothing left to drain) or `more_work`
 * is 0 (the caller's own ring-empty check says the queue is drained) --
 * otherwise leaves `pending` set so the next hype_poll_sched_should_run()
 * still says to run, which is how a budget-limited poll keeps making
 * progress across several calls instead of stopping with work left queued.
 */
void hype_poll_sched_after_run(hype_poll_sched_t *sched, unsigned int work_done, int more_work);

/*
 * A single service-loop entry point registration: a poll function plus an
 * opaque context, matching the decision-35 registration seam (a driver
 * reaches the rest of hype only through this ABI, never a private
 * cross-reference), so a driver here stays a clean module-extraction
 * candidate. `poll_fn` returns the number of descriptors it reclaimed.
 */
typedef unsigned int (*hype_host_poll_fn)(void *ctx);

#define HYPE_HOST_POLL_MAX_DEVICES 16

/* Registers a poller. Returns 1 on success, 0 if the registry is full or
 * poll_fn is NULL. Never fails silently -- a dropped registration would leave
 * a device that is never drained, which looks like a hung device later. */
int hype_host_poll_register(hype_host_poll_fn poll_fn, void *ctx);

/* Drains every registered poller once (run-to-completion, per HNET-3's "single
 * run-to-completion service call the main loop can invoke"). Returns the sum
 * of every poller's reclaim count. */
unsigned int hype_host_poll_run_all(void);

/* Test/reinit hook: clears every registration. Not used by production code
 * paths (the registry is populated once at driver init and never shrinks),
 * but unit tests need a way back to a known-empty registry between cases. */
void hype_host_poll_reset(void);

#endif /* HYPE_CORE_HOST_PCI_IRQ_H */
