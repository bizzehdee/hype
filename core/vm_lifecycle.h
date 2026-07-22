#ifndef HYPE_VM_LIFECYCLE_H
#define HYPE_VM_LIFECYCLE_H

/*
 * M8-4..7: per-VM lifecycle state machine. Pure logic (no I/O) so the
 * transition rules are unit-tested in isolation; boot/main.c holds one
 * `hype_vm_lifecycle_t` per VM, the guest run loop reads it each iteration to
 * decide whether to VMRUN, and the dashboard/command layer posts events.
 *
 * States:
 *   RUNNING    - vCPU executes normally (VMRUN each loop iteration).
 *   PAUSED     - vCPU frozen in place (Stop); VMCB/RAM/device state retained,
 *                Resume returns straight to RUNNING (M8-5).
 *   SHUTTING   - orderly shutdown requested; the guest keeps running so it can
 *                reach ACPI S5 (soft-off). A bounded timeout escalates to OFF
 *                if the guest doesn't power itself off (M8-6).
 *   OFF        - powered off (guest hit S5, was force-killed, or timed out).
 *                RAM retained until the next Start re-zeroes it (M8-4/M8-7).
 *   STARTING   - a Start was requested from OFF; the loop re-initialises the
 *                guest (zero RAM, reload firmware, reset devices/vCPU) then the
 *                machine advances to RUNNING (M8-4).
 *
 * Events: STOP, RESUME, SHUTDOWN, FORCE_OFF, START, S5 (guest reached soft-off),
 * TIMEOUT (shutdown grace elapsed), STARTED (re-init complete).
 */

typedef enum {
    HYPE_VM_RUNNING = 0,
    HYPE_VM_PAUSED,
    HYPE_VM_SHUTTING,
    HYPE_VM_OFF,
    HYPE_VM_STARTING,
} hype_vm_lifecycle_t;

typedef enum {
    HYPE_VM_EV_STOP = 0,
    HYPE_VM_EV_RESUME,
    HYPE_VM_EV_SHUTDOWN,
    HYPE_VM_EV_FORCE_OFF,
    HYPE_VM_EV_START,
    HYPE_VM_EV_S5,       /* guest reached ACPI soft-off */
    HYPE_VM_EV_TIMEOUT,  /* shutdown grace period elapsed */
    HYPE_VM_EV_STARTED,  /* re-initialisation finished */
} hype_vm_event_t;

/* Compute the next state for `state` on `ev`. Events that don't apply to the
 * current state are ignored (return `state` unchanged). Pure. */
hype_vm_lifecycle_t hype_vm_lifecycle_next(hype_vm_lifecycle_t state, hype_vm_event_t ev);

/* True if the vCPU should be VMRUN in this state (RUNNING or SHUTTING -- the
 * guest must keep executing during an orderly shutdown to reach S5). */
int hype_vm_lifecycle_runs(hype_vm_lifecycle_t state);

/* Short human-readable label for the dashboard ("running"/"paused"/...). */
const char *hype_vm_lifecycle_name(hype_vm_lifecycle_t state);

#endif /* HYPE_VM_LIFECYCLE_H */
