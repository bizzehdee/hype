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
    /* #542: move the guest's MOUSE. data[0..2] = {status, dx, dy}, already encoded by the parser. */
    HYPE_INPUT_ACTION_SENDMOUSE,
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

    /*
     * #728: the SCREEN gate. A screen scan re-presents the whole display on every
     * call, so matching it with an ends-with window is a substring search over
     * everything currently visible -- including text painted long before the current
     * directive was reached. That let `expect localhost login:` after a `reboot` be
     * satisfied instantly by the PREVIOUS boot's banner still on screen, passing a
     * run whose guest never restarted.
     *
     * The rule these fields implement: a screen scan may only satisfy a pattern that
     * was ABSENT from the screen when its directive was armed. Present-at-arm means
     * gated, and the gate lifts as soon as a scan shows the pattern gone -- so a
     * later, genuinely new appearance still matches.
     *
     * The wire matcher is untouched. It is a true stream: a byte arriving on it has
     * by definition just appeared, so it needs no gate.
     *
     * `gate_state` holds one of GATE_UNKNOWN / GATE_CLOSED / GATE_OPEN per armed
     * `fail-if`, indexed alongside `failif[]`; a false FAIL from stale screen text is
     * the same defect wearing the opposite verdict.
     */
    uint32_t scan_gate_pc; /* pc `scan_gated` was computed for; UINT32_MAX = none */
    uint8_t scan_gated;    /* current expect's pattern was already on screen when armed */
    uint8_t scan_suppress; /* set only inside hype_input_runner_scan, while gated */
    uint8_t failif_gate[HYPE_INPUT_SCRIPT_MAX_DIRECTIVES];
    /*
     * The reference point has to be the screen BEFORE a directive became current, not
     * after. Measuring on the first scan following it cannot tell a stale banner apart
     * from text the script's own send/sendkey just produced -- gating the latter would
     * hang `sendkey Hello World` / `expect Hello World` (tools/302) forever. So each scan
     * pre-measures the NEXT expect the pc has not reached yet, and that answer becomes
     * its gate when it does.
     */
    uint32_t gate_pre_pc;     /* directive `gate_pre_present` was measured for */
    uint8_t gate_pre_present; /* was its pattern on screen before it became current? */

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
 * This is still a different question from the streaming matcher's -- it asks what is ON SCREEN, not
 * what came down the wire -- but since #728 it is no longer "visible NOW" without qualification.
 *
 * #728 amended that: a screen scan may only satisfy a pattern that was ABSENT when its directive
 * was armed. Matching text that was already there turned every repeated `expect` into an instant
 * pass -- `expect localhost login:` after a `reboot` matched the PREVIOUS boot's banner still on
 * screen, and a run whose guest never restarted was reported PASS. A false pass is the worst
 * failure a validation harness has, so the safe reading is the default one.
 *
 * The original wording justified the old behaviour with "is the menu up". Every shipped script's
 * screen-matched expect turns out to WAIT FOR SOMETHING TO APPEAR instead -- `expect GNU GRUB` then
 * `sendkey c` then `expect grub>`, `expect Press any key to boot from CD or DVD.` -- so none of
 * them relied on the reading that was removed, and all of them still work.
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
