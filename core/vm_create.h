#ifndef HYPE_CORE_VM_CREATE_H
#define HYPE_CORE_VM_CREATE_H

#include "cfg.h"

/*
 * TERM-10 (#486): the interactive VM-creation wizard, as a pure state machine.
 *
 * A VM used to exist only if it was in hype.cfg when hype booted, so adding one meant editing that
 * file from another machine and power-cycling the host -- taking the whole hypervisor down to add a
 * guest to it. plan.md section 10 decision 33 rejected runtime VM creation and was REVERSED for
 * operator-driven changes by TERM-9 (#485), on three conditions this wizard depends on: a free
 * per-VM state slot (#393), guest RAM from the pre-reserved pool (#449), and the same admission
 * check that gates startup gating creation (#453).
 *
 * Pure and unit-testable by design: no terminal, no allocation, no I/O. The caller feeds it one
 * line at a time and reads back a prompt; when it reports DONE it holds a filled hype_cfg_vm_t that
 * the caller admits, writes to hype.cfg and starts. Every answer is validated AT THE MOMENT IT IS
 * ENTERED, against the same rules the config parser uses -- an operator who mistypes vcpus should
 * be told on that line, not after answering seven more questions.
 */

typedef enum {
    HYPE_VMW_NAME = 0,
    HYPE_VMW_OS_HINT,
    HYPE_VMW_VCPUS,
    HYPE_VMW_MEM_MB,
    HYPE_VMW_DISK,
    HYPE_VMW_MEDIA,
    HYPE_VMW_BOOT,
    HYPE_VMW_CPU_SET,
    HYPE_VMW_CONFIRM,
    HYPE_VMW_DONE,
    HYPE_VMW_CANCELLED
} hype_vmw_step_t;

typedef struct {
    hype_vmw_step_t step;
    hype_cfg_vm_t vm;      /* filled in as answers arrive */
    char error[128];       /* why the last answer was rejected; empty when it was accepted */
    int have_error;
} hype_vm_wizard_t;

/* Begin. `existing` is the live config, used for duplicate-name rejection; may be null. */
void hype_vm_wizard_begin(hype_vm_wizard_t *w);

/*
 * The prompt for the current step, always non-null and stable, so a caller can redraw it without
 * re-feeding anything.
 */
const char *hype_vm_wizard_prompt(const hype_vm_wizard_t *w);

/*
 * #570: render the wizard's current state as the exact text the terminal shows. This is where a
 * prompt and an error stop looking alike -- the operator report behind #570 read a correct prompt
 * as a rejection, and that was the correct reading of the old text.
 *
 *   - a live prompt renders as   "create> <prompt>  ('cancel' abandons)"
 *   - a rejected answer renders as "INVALID -- <why>" on its own line, THEN the prompt line
 *   - DONE / CANCELLED render as their status text alone (no prompt -- the wizard is over)
 *
 * The `create>` prefix says the wizard owns the command line; the cancel hint names the way out on
 * EVERY step (the other half of the operator report: no stated escape). Pure string building, so a
 * test can assert the two states are distinguishable. Truncates safely at `cap`; returns `out`.
 */
const char *hype_vm_wizard_render(const hype_vm_wizard_t *w, char *out, unsigned cap);

/*
 * Feed one answer. Returns the step the wizard is NOW on -- unchanged if the answer was rejected,
 * in which case w->error says why in terms an operator can act on.
 *
 * An empty line takes the step's default where it has one, and "cancel" abandons the wizard at any
 * point: an operator who realises the machine is wrong must not have to finish inventing it.
 */
hype_vmw_step_t hype_vm_wizard_feed(hype_vm_wizard_t *w, const char *line,
                                    const hype_cfg_t *existing);

/* A one-line summary of what will be created, for the confirmation step. */
void hype_vm_wizard_summary(const hype_vm_wizard_t *w, char *out, unsigned int out_len);

#endif /* HYPE_CORE_VM_CREATE_H */
