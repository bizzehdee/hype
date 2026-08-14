#ifndef HYPE_CORE_INPUT_RUNNER_H
#define HYPE_CORE_INPUT_RUNNER_H

#include <stdint.h>

#include "input_script.h"

/*
 * INPUT-6 (#279): the decision-making half of §6k's scripted guest input. Still
 * pure -- no hardware, no UART, and NO CLOCK OF ITS OWN.
 *
 * Three inputs: the parsed script (#278), guest console bytes as they arrive, and a
 * monotonic millisecond timestamp the caller supplies. One output: what to do now --
 * send these bytes, wait, or finish with a verdict.
 *
 * Taking the clock as a parameter rather than calling hype_rdtsc() inside is what
 * makes this testable at all (the same trick hype_blk_wstats_set_clock() uses): a
 * 120-second timeout is then exercised in microseconds instead of two minutes.
 *
 * #280 wires the sent bytes into the per-VM guest UART; #282 reports the verdict.
 */

typedef enum {
    HYPE_INPUT_ACTION_WAIT = 0, /* nothing to do yet -- keep feeding output */
    HYPE_INPUT_ACTION_SEND,     /* type data[0..len) into the guest's serial port */
    HYPE_INPUT_ACTION_SENDKEY,  /* #284: type it into the guest's KEYBOARD instead */
    HYPE_INPUT_ACTION_DONE      /* finished; read the verdict */
} hype_input_action_kind_t;

typedef enum {
    HYPE_INPUT_VERDICT_PENDING = 0,
    HYPE_INPUT_VERDICT_PASS,
    HYPE_INPUT_VERDICT_FAIL
} hype_input_verdict_t;

/* Why a run finished. Every terminal state has one -- "no reason" is not a state
 * this can reach, because a validation harness that reports nothing is worse than
 * one that reports a failure. */
typedef enum {
    HYPE_INPUT_REASON_NONE = 0,
    HYPE_INPUT_REASON_PASS_DIRECTIVE,  /* `pass <label>` reached */
    HYPE_INPUT_REASON_FAIL_DIRECTIVE,  /* `fail <label>` reached */
    HYPE_INPUT_REASON_EXPECT_TIMEOUT,  /* an `expect` was not seen in time */
    HYPE_INPUT_REASON_FAIL_IF_MATCHED, /* an armed `fail-if` pattern appeared */
    HYPE_INPUT_REASON_RAN_OFF_END,     /* script ended without pass/fail */
    HYPE_INPUT_REASON_TRANSPORT_STALL  /* the requested input could not reach the guest */
} hype_input_reason_t;

typedef struct {
    hype_input_action_kind_t kind;
    const uint8_t *data; /* SEND only; points into the script, which outlives the run */
    uint32_t len;
} hype_input_action_t;

typedef struct {
    const hype_input_script_t *script;
    uint32_t pc;         /* next directive to consider */
    uint32_t timeout_ms; /* current per-expect timeout */

    int phase_started;      /* has the current expect/delay had its clock started? */
    uint64_t phase_start_ms;

    /*
     * Rolling window of the most recent output bytes, sized to the longest possible
     * pattern. Matching asks "does the window END WITH the pattern", which is what
     * makes a match work when the guest emits it one byte per UART interrupt and
     * when unrelated output surrounds it.
     *
     * Deliberately NOT a line buffer: `localhost login:` has NO trailing newline, so
     * anything that waits for end-of-line hangs forever on the exact case this
     * feature exists to handle.
     */
    uint8_t win[HYPE_INPUT_SCRIPT_MAX_ARG];
    uint32_t win_len;

    /* Indices of `fail-if` directives armed so far. Checked against every byte for
     * the rest of the run, not only while their directive is current. */
    uint32_t failif[HYPE_INPUT_SCRIPT_MAX_DIRECTIVES];
    uint32_t failif_count;

    hype_input_verdict_t verdict;
    hype_input_reason_t reason;
    /* The directive that ended the run: its label (pass/fail) or the pattern that
     * timed out / fired. Points into the script. */
    const uint8_t *detail;
    uint32_t detail_len;
    uint32_t detail_line;
    int done;
} hype_input_runner_t;

/* Begin a run. `script` must outlive the runner. */
void hype_input_runner_init(hype_input_runner_t *r, const hype_input_script_t *script,
                           uint64_t now_ms);

/* Feed one byte of the guest's console output. Safe (and a no-op) once finished. */
void hype_input_runner_feed(hype_input_runner_t *r, uint8_t byte);

/*
 * #302: match against the RECONSTRUCTED SCREEN as well as the byte stream.
 *
 * The byte-stream matcher above cannot see text drawn by a full-screen program. Measured on GRUB's
 * boot menu: the wire carries cursor positioning and paint spaces (`ESC[005;238H`), so the literal
 * `GNU GRUB` never exists as consecutive bytes, even though the log shows it -- that line is
 * hype_vt_filter's reconstruction. Every consumer that reads a KEYBOARD rather than a serial port
 * is such a program (OVMF's boot menu, GRUB, graphical installers), which is precisely the set
 * `sendkey` exists for, so without this they cannot be scripted at all.
 *
 * Pass a snapshot of the terminal grid as text, rows concatenated. Call it on a cadence; each call
 * is an independent look at what is CURRENTLY ON SCREEN.
 *
 * That is a deliberately different meaning from the streaming matcher: `expect` against the screen
 * is satisfied by text that is visible NOW, including text that was already there when the expect
 * became current. For a TUI that is the useful question ("is the menu up"), but it is not the same
 * as "this appeared in the output", so the two are separate entry points rather than one merged
 * matcher.
 *
 * The raw rolling window is saved and restored across the scan, so a pattern arriving one byte at a
 * time on the wire cannot be broken by a screen scan landing in the middle of it.
 */
void hype_input_runner_scan(hype_input_runner_t *r, const uint8_t *text, uint32_t len);

/* The caller could not deliver a previously-issued send/sendkey directive.
 * This is terminal: continuing to a later `pass` would turn lost input into a
 * false validation success. */
void hype_input_runner_transport_stalled(hype_input_runner_t *r);

/*
 * Decide what to do at `now_ms`. Returns the action and fills *out.
 *
 * A SEND is returned exactly once per `send` directive -- calling poll again
 * advances past it, so a caller that drops the bytes loses them. That is deliberate:
 * silently re-sending would double-type a command.
 */
hype_input_action_kind_t hype_input_runner_poll(hype_input_runner_t *r, uint64_t now_ms,
                                                hype_input_action_t *out);

/* Terminal state, once kind == DONE. */
hype_input_verdict_t hype_input_runner_verdict(const hype_input_runner_t *r);
hype_input_reason_t hype_input_runner_reason(const hype_input_runner_t *r);
const char *hype_input_reason_str(hype_input_reason_t reason);

#endif /* HYPE_CORE_INPUT_RUNNER_H */
