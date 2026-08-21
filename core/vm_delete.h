#ifndef HYPE_CORE_VM_DELETE_H
#define HYPE_CORE_VM_DELETE_H

/*
 * #491 (TERM-15): the delete confirmation flow, as a pure state machine -- the same shape as the
 * create wizard (core/vm_create.h) and for the same reason: deletion is destructive and cannot be
 * undone from the terminal, so the flow that guards it must be testable, and an operator must not
 * be able to pass through it by accident.
 *
 * Exactly two steps, in this order, per the ticket:
 *   1. "Are you sure?"                        -- yes / no
 *   2. "This will delete <vm> from hype.cfg.
 *       This cannot be undone."               -- delete / cancel
 *
 * The second answer is the WORD `delete`, not another yes: an operator on autopilot who answers
 * `yes` twice has not confirmed anything. `no` at step 1 or `cancel` at step 2 aborts with nothing
 * changed. Backing disks are never touched -- the step-2 prompt lists the paths it leaves in
 * place, which the caller supplies (only the caller knows the VM's device bindings).
 */

typedef enum {
    HYPE_VMD_CONFIRM1 = 0,
    HYPE_VMD_CONFIRM2,
    HYPE_VMD_CONFIRMED, /* terminal: the caller may now delete */
    HYPE_VMD_ABORTED    /* terminal: nothing may change */
} hype_vmd_step_t;

#define HYPE_VMD_NAME_MAX 32u
#define HYPE_VMD_DISKS_MAX 160u

typedef struct {
    hype_vmd_step_t step;
    char vm_name[HYPE_VMD_NAME_MAX];
    /* the disk paths step 2 promises to leave in place; "" renders as "none" */
    char disks_note[HYPE_VMD_DISKS_MAX];
    char error[96]; /* why the last answer was rejected; empty when it was accepted */
    int have_error;
} hype_vm_delete_t;

/* Begin the flow for `vm_name`, whose backing disks are described by `disks_note`. */
void hype_vm_delete_begin(hype_vm_delete_t *d, const char *vm_name, const char *disks_note);

/*
 * Feed one answer; returns the step the flow is NOW on. An unrecognised answer holds the step and
 * sets `error` -- it never advances and never aborts, because both of those are decisions the
 * operator has not made.
 */
hype_vmd_step_t hype_vm_delete_feed(hype_vm_delete_t *d, const char *line);

/*
 * Render the current state as the exact text the terminal shows -- same contract as
 * hype_vm_wizard_render (#570): a live prompt is "delete> ...", a rejected answer leads with
 * "INVALID -- <why>", terminal states render their outcome alone. Returns `out`.
 */
const char *hype_vm_delete_render(const hype_vm_delete_t *d, char *out, unsigned cap);

#endif /* HYPE_CORE_VM_DELETE_H */
