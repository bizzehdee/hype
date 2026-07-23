#ifndef HYPE_CORE_PHYS_CONFIRM_H
#define HYPE_CORE_PHYS_CONFIRM_H

#include <stdint.h>

/*
 * M10-5 (#125): the INTERACTIVE-confirmation half of the physical-write safety
 * story. hype_phys_guard (#124) is the pure policy -- identity match + the
 * non-empty-partition-table guard + an `operator_confirmed` bit. This module is
 * the pure model of how that bit gets set: a `physical:` target that already
 * passed identity + non-empty MUST still not be armed for writing until the
 * operator, shown the REAL drive's model / serial / size on the local
 * dashboard, deliberately re-types that drive's serial.
 *
 * Re-typing the serial (rather than a bare "y") is the whole point: it forces
 * the operator to have actually read the identity of the drive they are about
 * to destroy, and makes an accidental confirmation essentially impossible.
 *
 * Pure + UEFI-free so it unit-tests in isolation. boot/main.c owns the dashboard
 * render + keystroke routing: it arms a request when a VM has a writable
 * `physical:` target, renders hype_phys_confirm_prompt() as the dashboard footer,
 * feeds the operator's typed token to hype_phys_confirm_submit(), and only once
 * hype_phys_confirm_is_accepted() is true passes operator_confirmed=1 to
 * hype_phys_guard_arm() -- so the guard, not this module, remains the single
 * arming gate.
 */

#define HYPE_PHYS_CONFIRM_NAME_MAX   32
#define HYPE_PHYS_CONFIRM_MODEL_MAX  48
#define HYPE_PHYS_CONFIRM_SERIAL_MAX 32

typedef enum {
    HYPE_PHYS_CONFIRM_IDLE = 0,   /* nothing pending */
    HYPE_PHYS_CONFIRM_PENDING,    /* awaiting the operator's typed serial */
    HYPE_PHYS_CONFIRM_ACCEPTED    /* operator typed the matching serial */
} hype_phys_confirm_state_t;

typedef struct {
    hype_phys_confirm_state_t state;
    char vm_name[HYPE_PHYS_CONFIRM_NAME_MAX];
    char model[HYPE_PHYS_CONFIRM_MODEL_MAX];
    char serial[HYPE_PHYS_CONFIRM_SERIAL_MAX];
    uint64_t size_bytes;
    unsigned int attempts;        /* count of mismatched submissions so far */
} hype_phys_confirm_t;

typedef enum {
    HYPE_PHYS_CONFIRM_SUBMIT_NONE_PENDING = 0, /* no request armed -> ignored */
    HYPE_PHYS_CONFIRM_SUBMIT_ACCEPTED,         /* typed serial matched -> ACCEPTED */
    HYPE_PHYS_CONFIRM_SUBMIT_MISMATCH          /* wrong serial -> still PENDING */
} hype_phys_confirm_submit_t;

/*
 * Arms a pending confirmation for `vm_name`'s write to the drive identified by
 * (model, serial, size_bytes). Overwrites any prior request and clears the
 * accepted/attempt state -- a fresh request always starts unconfirmed. NULL
 * strings are treated as empty. State becomes PENDING.
 */
void hype_phys_confirm_request(hype_phys_confirm_t *c, const char *vm_name,
                               const char *model, const char *serial,
                               uint64_t size_bytes);

/*
 * Feeds an operator-typed token. Only meaningful while PENDING:
 *   - exact match of the pending drive's serial  -> state ACCEPTED, returns ACCEPTED
 *   - any other non-empty token                  -> attempts++, stays PENDING, MISMATCH
 * When not PENDING (IDLE or already ACCEPTED) returns NONE_PENDING and changes
 * nothing. The match is exact and case-sensitive against the enumerated serial
 * (leading/trailing whitespace on the typed token is ignored). Pure.
 */
hype_phys_confirm_submit_t hype_phys_confirm_submit(hype_phys_confirm_t *c,
                                                    const char *typed_serial);

/* 1 once the operator has confirmed THIS request (state ACCEPTED). */
int hype_phys_confirm_is_accepted(const hype_phys_confirm_t *c);

/* Clears back to IDLE (e.g. VM stopped, or the write completed). */
void hype_phys_confirm_reset(hype_phys_confirm_t *c);

/*
 * Formats the operator-facing prompt for the current state into buf (NUL-
 * terminated, truncated to n). PENDING renders the destructive-write warning
 * with the drive model / serial / human size and the exact command to type;
 * ACCEPTED renders a short confirmed line; IDLE renders "". Returns buf. Pure --
 * no allocation, no I/O.
 */
const char *hype_phys_confirm_prompt(const hype_phys_confirm_t *c, char *buf, unsigned int n);

#endif /* HYPE_CORE_PHYS_CONFIRM_H */
