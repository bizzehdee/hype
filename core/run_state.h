/*
 * The run-state record: which VMs were running when the host last went down (#176/#177).
 *
 * plan.md §6h, decision 13: hype restores each VM to its prior run/stopped state across a host
 * power event. This is explicitly a RESTART-TO-THE-SAME-RUN-STATE mechanism, not a snapshot --
 * guest RAM is not preserved (snapshotting is a §1 non-goal). What is persisted is "was this VM
 * supposed to be running", and nothing else.
 *
 * KEYED BY NAME, NOT BY INDEX. An operator edits hype.cfg between boots: adding, removing or
 * reordering a [vm.*] section shifts every later index. A record saying "VM 2 was running" would
 * then start a different machine than the one that was up, which is worse than starting none --
 * it is a wrong action taken confidently. A name survives reordering, and a renamed VM reads as
 * unknown, which has a defined answer (below) rather than an accidental one.
 *
 * THREE ANSWERS, not two. hype_run_state_lookup() distinguishes "was stopped" from "not in the
 * record at all", because a VM added to hype.cfg since the last shutdown must not be held off by
 * a record that could not have mentioned it. Unknown means "no opinion", and the caller keeps its
 * default (start it) -- the same reading §4.3 gives an absent config key.
 *
 * VERSIONED, and a version this build does not know REFUSES the whole record rather than reading
 * the lines it happens to recognise. A half-understood record produces a half-restored host, and
 * the operator has no way to tell that from a correctly restored one.
 *
 * Pure and host-testable on purpose: the format is the contract between a shutdown and the next
 * boot, the two ends run hours apart on either side of a power cycle, and there is no way to
 * observe a disagreement between them except by the host coming up wrong.
 */
#ifndef HYPE_RUN_STATE_H
#define HYPE_RUN_STATE_H

#include "cfg.h"

/* Bounded by the config's own VM ceiling: a record can never describe more machines than hype
 * can be configured to run. */
#define HYPE_RUN_STATE_MAX_VMS HYPE_CFG_MAX_VMS

/* The record's own format version. Bump it when the MEANING of a line changes; a reader that does
 * not know a version refuses the record entirely. */
#define HYPE_RUN_STATE_VERSION 1

/* Where it lives: hype's own boot volume, beside hype.cfg (plan.md §6h -- both the write and the
 * next boot's read go through hype's storage stack, not the firmware's, per decisions 37/38). */
#define HYPE_RUN_STATE_PATH "hype-state.txt"

/* Why the host went down. Recorded for the operator reading the file, not acted on. */
typedef enum {
    HYPE_RUN_STATE_REASON_UNKNOWN = 0,
    HYPE_RUN_STATE_REASON_REBOOT,
    HYPE_RUN_STATE_REASON_OFF
} hype_run_state_reason_t;

/* What lookup() answers. */
#define HYPE_RUN_STATE_UNKNOWN (-1)
#define HYPE_RUN_STATE_STOPPED 0
#define HYPE_RUN_STATE_RUNNING 1

typedef struct {
    char name[HYPE_CFG_NAME_MAX];
    int running;
} hype_run_state_vm_t;

typedef struct {
    unsigned int version;
    hype_run_state_reason_t reason;
    unsigned int count;
    hype_run_state_vm_t vms[HYPE_RUN_STATE_MAX_VMS];
    /*
     * Set when the record could not be trusted as a whole: no version line, a version this build
     * does not know, or a `vm` line that does not parse. It is a REFUSAL, not a warning -- on
     * malformed, count is 0 and every lookup answers UNKNOWN, so a boot falls back to its default
     * instead of acting on a fragment.
     */
    unsigned int malformed;
    /* Lines that parsed as syntax but carried a key this build does not know. Retained as a count
     * only: unlike hype.cfg there is nothing here an operator authored, so there is nothing to
     * preserve on write-back -- but a rising count says a newer hype wrote this file. */
    unsigned int unknown_keys;
    /* VMs the record described beyond HYPE_RUN_STATE_MAX_VMS. Never silently dropped (#341). */
    unsigned int dropped;
} hype_run_state_t;

/*
 * Serialize `st` as text. Returns 0 and sets *len on success, -1 if the text would not fit `cap`
 * (in which case nothing has been written that a caller may use -- a truncated record is a record
 * that lies about which VMs were up).
 */
int hype_run_state_serialize(const hype_run_state_t *st, char *out, unsigned int cap,
                             unsigned int *len);

/* Parse `len` bytes of text into `out`. Always fills `out`. Returns 0 when the record is usable,
 * -1 when it was refused (out->malformed is set and out->count is 0). */
int hype_run_state_parse(const char *text, unsigned int len, hype_run_state_t *out);

/* HYPE_RUN_STATE_RUNNING / _STOPPED / _UNKNOWN for `name`. */
int hype_run_state_lookup(const hype_run_state_t *st, const char *name);

/* Add one VM. Returns 0, or -1 when the record is full (and bumps st->dropped). */
int hype_run_state_add(hype_run_state_t *st, const char *name, int running);

/* Reset to an empty record of this build's version. */
void hype_run_state_init(hype_run_state_t *st, hype_run_state_reason_t reason);

#endif /* HYPE_RUN_STATE_H */
