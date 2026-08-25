#include "input_runner.h"

/* Default per-expect timeout when the script sets none. Generous: a cold guest
 * booting an installer off a streamed ISO can take tens of seconds before its login
 * prompt, and a timeout that fires early would report a failure that isn't one. */
#define HYPE_INPUT_DEFAULT_TIMEOUT_MS 120000u

static void win_reset(hype_input_runner_t *r) {
    r->win_len = 0;
}

static void win_push(hype_input_runner_t *r, uint8_t b) {
    if (r->win_len < HYPE_INPUT_SCRIPT_MAX_ARG) {
        r->win[r->win_len] = b;
        r->win_len++;
        return;
    }
    /* Full: drop the oldest byte. Copied by hand rather than with a library move --
     * this is a freestanding build with no libc. */
    {
        uint32_t i;
        for (i = 1; i < HYPE_INPUT_SCRIPT_MAX_ARG; i++) {
            r->win[i - 1u] = r->win[i];
        }
        r->win[HYPE_INPUT_SCRIPT_MAX_ARG - 1u] = b;
    }
}

/* Does the rolling window end with `pat`? An empty pattern never matches -- the
 * parser rejects those, and treating one as an instant match would silently skip an
 * expect. */
static int win_ends_with(const hype_input_runner_t *r, const uint8_t *pat, uint32_t patlen) {
    uint32_t i;
    if (patlen == 0 || patlen > r->win_len) {
        return 0;
    }
    for (i = 0; i < patlen; i++) {
        if (r->win[r->win_len - patlen + i] != pat[i]) {
            return 0;
        }
    }
    return 1;
}

/* #728: the three states of a screen gate. UNKNOWN means "not yet compared against a
 * screen": the first scan after arming decides, and until one happens the pattern can
 * only be matched off the wire, which needs no gate. */
#define GATE_UNKNOWN 0u
#define GATE_CLOSED 1u /* pattern was on screen when armed -- screen matches suppressed */
#define GATE_OPEN 2u   /* pattern has since been absent -- a new appearance counts */

#define GATE_NO_PC 0xFFFFFFFFu

/*
 * #728: does `pat` occur ANYWHERE in `text`? The gate asks about presence on the whole
 * screen, which is a different question from win_ends_with's "did this just arrive".
 *
 * Plain O(n*m) scan. The screen is a couple of thousand cells and patterns are at most
 * HYPE_INPUT_SCRIPT_MAX_ARG, evaluated at the scan cadence (~10 Hz) and only when the
 * current directive changes -- there is nothing here worth a smarter algorithm.
 */
static int text_contains(const uint8_t *text, uint32_t len, const uint8_t *pat, uint32_t patlen) {
    uint32_t i;
    if (patlen == 0u || patlen > len) {
        return 0;
    }
    for (i = 0; i + patlen <= len; i++) {
        uint32_t j;
        for (j = 0; j < patlen; j++) {
            if (text[i + j] != pat[j]) {
                break;
            }
        }
        if (j == patlen) {
            return 1;
        }
    }
    return 0;
}

/*
 * Consume the directives that need neither a clock nor guest output: `timeout` and
 * `fail-if`. Called from init() and from feed(), not only from poll().
 *
 * Without this the runner is ORDER-DEPENDENT: a caller that fed output before ever
 * polling would leave the program counter parked on a `fail-if`, so the `expect`
 * behind it was not yet current and could never match. A test caught exactly that.
 * A state machine whose correctness depends on the caller's call order is a trap, so
 * the arming happens wherever it can rather than in one privileged entry point.
 */
static void arm_static_directives(hype_input_runner_t *r) {
    while (!r->done && r->pc < r->script->count) {
        const hype_input_directive_t *d = &r->script->d[r->pc];
        if (d->op == HYPE_INPUT_OP_TIMEOUT) {
            r->timeout_ms = d->ms;
            r->pc++;
            continue;
        }
        if (d->op == HYPE_INPUT_OP_FAIL_IF) {
            if (r->failif_count < HYPE_INPUT_SCRIPT_MAX_DIRECTIVES) {
                r->failif[r->failif_count] = r->pc;
                /* #728: undecided until a screen scan compares it. */
                r->failif_gate[r->failif_count] = GATE_UNKNOWN;
                r->failif_count++;
            }
            r->pc++;
            continue;
        }
        break;
    }
}

static void finish(hype_input_runner_t *r, hype_input_verdict_t v, hype_input_reason_t reason,
                   const uint8_t *detail, uint32_t detail_len, uint32_t line) {
    r->verdict = v;
    r->reason = reason;
    r->detail = detail;
    r->detail_len = detail_len;
    r->detail_line = line;
    r->done = 1;
}

void hype_input_runner_init(hype_input_runner_t *r, const hype_input_script_t *script,
                           uint64_t now_ms) {
    r->script = script;
    r->pc = 0;
    r->timeout_ms = HYPE_INPUT_DEFAULT_TIMEOUT_MS;
    r->phase_started = 0;
    r->phase_start_ms = now_ms;
    r->win_len = 0;
    r->failif_count = 0;
    r->scan_gate_pc = GATE_NO_PC;
    r->scan_gated = 0;
    r->scan_suppress = 0;
    r->gate_pre_pc = GATE_NO_PC;
    r->gate_pre_present = 0;
    r->verdict = HYPE_INPUT_VERDICT_PENDING;
    r->reason = HYPE_INPUT_REASON_NONE;
    r->detail = 0;
    r->detail_len = 0;
    r->detail_line = 0;
    r->done = 0;
    arm_static_directives(r);
}

void hype_input_runner_feed(hype_input_runner_t *r, uint8_t byte) {
    uint32_t i;

    if (r->done) {
        return;
    }
    /* Keep the pc off the clock-free directives regardless of call order. */
    arm_static_directives(r);
    win_push(r, byte);

    /*
     * fail-if first, and against every byte for the whole remaining run. Seeing the
     * other guest's marker is the bug #283 is looking for, and it can appear at any
     * point -- including while an unrelated `expect` is current. Checking it only
     * while its own directive is at the program counter would miss exactly the case
     * it exists for.
     */
    for (i = 0; i < r->failif_count; i++) {
        const hype_input_directive_t *fd = &r->script->d[r->failif[i]];
        /* #728: while this scan is presenting the screen, a fail-if whose pattern was
         * already visible when it was armed must not fire. Same defect as the expect
         * false-pass, wearing the opposite verdict. */
        if (r->scan_suppress && r->failif_gate[i] == GATE_CLOSED) {
            continue;
        }
        if (win_ends_with(r, fd->text, fd->len)) {
            finish(r, HYPE_INPUT_VERDICT_FAIL, HYPE_INPUT_REASON_FAIL_IF_MATCHED, fd->text, fd->len,
                   fd->line);
            return;
        }
    }

    if (r->pc < r->script->count) {
        const hype_input_directive_t *d = &r->script->d[r->pc];
        /* #728: likewise for the current expect -- a gated pattern is one that was
         * already on screen when this directive was armed, so matching it here would
         * pass on history rather than on anything the guest just did. */
        if (r->scan_suppress && r->scan_gated && r->scan_gate_pc == r->pc) {
            return;
        }
        if (d->op == HYPE_INPUT_OP_EXPECT && win_ends_with(r, d->text, d->len)) {
            /* Consume the matched input: a following `expect` for the same pattern
             * (e.g. two `expect ~#` around a command) must NOT be satisfied
             * instantly by what the previous one already matched. */
            win_reset(r);
            r->pc++;
            r->phase_started = 0;
            arm_static_directives(r);
        }
    }
}

/* #302: see the header. Reuses feed() so fail-if, expect consumption and pc advance behave
 * identically -- the only difference is which bytes are presented and that the wire's own
 * rolling window is preserved around the call. */
void hype_input_runner_scan(hype_input_runner_t *r, const uint8_t *text, uint32_t len) {
    uint8_t saved[HYPE_INPUT_SCRIPT_MAX_ARG];
    uint32_t saved_len;
    uint32_t i;

    if (r == 0 || text == 0 || len == 0u || r->done) {
        return;
    }
    /* `saved` is the same size as r->win and win_len can never exceed it, so the copy needs no
     * bound of its own -- a guard that cannot fail is dead code pretending to be caution. */
    saved_len = r->win_len;
    for (i = 0; i < saved_len; i++) {
        saved[i] = r->win[i];
    }
    /* Start clean: a partial match left by the wire must not combine with the screen's first
     * characters into a match that never appeared anywhere. */
    r->win_len = 0;

    /*
     * #728: settle every fail-if's gate against THIS screen before feeding it. A pattern
     * visible at arm time is history; once a scan shows it gone, any later appearance is
     * genuinely new and the gate opens for good.
     */
    for (i = 0; i < r->failif_count; i++) {
        const hype_input_directive_t *fd = &r->script->d[r->failif[i]];
        int present = text_contains(text, len, fd->text, fd->len);
        if (r->failif_gate[i] == GATE_UNKNOWN) {
            r->failif_gate[i] = present ? (uint8_t)GATE_CLOSED : (uint8_t)GATE_OPEN;
        } else if (r->failif_gate[i] == GATE_CLOSED && !present) {
            r->failif_gate[i] = GATE_OPEN;
        }
    }

    /*
     * #728: settle the CURRENT expect's gate. The reference point that matters is the
     * screen as it was BEFORE this directive became current -- not as it is now.
     *
     * Measuring "is it present right now, on the first scan after arming" conflates two
     * opposite cases: a stale banner left over from before (must gate) and text the
     * script's own `send`/`sendkey` just put there (must NOT gate, or
     * `sendkey Hello World` followed by `expect Hello World` in tools/302 would time out
     * forever). They are indistinguishable at that moment.
     *
     * So each scan also pre-measures the NEXT expect in the script -- the one the pc has
     * not reached yet -- and that recorded answer becomes its gate when it does. By
     * construction that measurement predates the directive, which is exactly the question
     * being asked. `gate_pre_pc` says which directive the recorded `gate_pre_present`
     * belongs to; anything else falls back to measuring against this screen.
     */
    if (r->pc == r->scan_gate_pc) {
        /* Same directive as the previous scan. Once the pattern leaves the screen, any
         * later appearance is genuinely new, so the gate opens for good. */
        if (r->scan_gated && r->pc < r->script->count) {
            const hype_input_directive_t *d = &r->script->d[r->pc];
            if (!text_contains(text, len, d->text, d->len)) {
                r->scan_gated = 0;
            }
        }
    } else {
        r->scan_gate_pc = r->pc;
        r->scan_gated = 0;
        if (r->pc < r->script->count) {
            const hype_input_directive_t *d = &r->script->d[r->pc];
            if (d->op == HYPE_INPUT_OP_EXPECT) {
                /* No pre-measurement for this directive means no scan ever saw the screen
                 * while it was pending, so there is no "before" to call the text stale
                 * against -- ungated. Gating on the current screen instead would break
                 * the first scan of a run, which is exactly when a TUI menu is already
                 * painted (`expect GNU GRUB`). */
                r->scan_gated = (r->gate_pre_pc == r->pc) ? r->gate_pre_present : 0u;
            }
        }
    }

    /* Pre-measure the next expect the pc has not reached, for the branch above. */
    {
        uint32_t k;
        r->gate_pre_pc = GATE_NO_PC;
        r->gate_pre_present = 0;
        for (k = r->pc + 1u; k < r->script->count; k++) {
            if (r->script->d[k].op == HYPE_INPUT_OP_EXPECT) {
                r->gate_pre_pc = k;
                r->gate_pre_present =
                    (uint8_t)text_contains(text, len, r->script->d[k].text, r->script->d[k].len);
                break;
            }
        }
    }

    r->scan_suppress = 1;
    for (i = 0; i < len; i++) {
        /*
         * The pc can move mid-scan, when an earlier expect matches off this same screen.
         * The directive that just became current would otherwise be ungated by default
         * and could be satisfied by text further along in the very same snapshot.
         */
        if (r->pc != r->scan_gate_pc) {
            r->scan_gate_pc = r->pc;
            r->scan_gated = 0;
            if (r->pc < r->script->count) {
                const hype_input_directive_t *d = &r->script->d[r->pc];
                if (d->op == HYPE_INPUT_OP_EXPECT) {
                    /* No pre-measurement for this directive means no scan ever saw the screen
                     * while it was pending, so there is no "before" to call the text stale
                     * against -- ungated. Gating on the current screen instead would break
                     * the first scan of a run, which is exactly when a TUI menu is already
                     * painted (`expect GNU GRUB`). */
                    r->scan_gated = (r->gate_pre_pc == r->pc) ? r->gate_pre_present : 0u;
                }
            }
        }
        hype_input_runner_feed(r, text[i]);
    }
    r->scan_suppress = 0;

    for (i = 0; i < saved_len; i++) {
        r->win[i] = saved[i];
    }
    r->win_len = saved_len;
}

void hype_input_runner_transport_stalled(hype_input_runner_t *r) {
    static const uint8_t detail[] = "guest input was not drained";
    if (r == 0 || r->done) {
        return;
    }
    finish(r, HYPE_INPUT_VERDICT_FAIL, HYPE_INPUT_REASON_TRANSPORT_STALL,
           detail, (uint32_t)(sizeof(detail) - 1u), 0);
}

hype_input_action_kind_t hype_input_runner_poll(hype_input_runner_t *r, uint64_t now_ms,
                                                hype_input_action_t *out) {
    out->kind = HYPE_INPUT_ACTION_WAIT;
    out->data = 0;
    out->len = 0;

    if (r->done) {
        out->kind = HYPE_INPUT_ACTION_DONE;
        return out->kind;
    }

    for (;;) {
        const hype_input_directive_t *d;

        if (r->pc >= r->script->count) {
            /*
             * The script did everything it was told and never said pass or fail.
             * That is FAIL, not PASS: this is a validation harness, and a script
             * which does not explicitly assert success must not be counted as
             * success -- that is how a test comes to "pass" while checking nothing.
             * Reported with its own reason so the operator can tell it apart from a
             * genuine failure and go fix the script.
             */
            finish(r, HYPE_INPUT_VERDICT_FAIL, HYPE_INPUT_REASON_RAN_OFF_END, 0, 0, 0);
            out->kind = HYPE_INPUT_ACTION_DONE;
            return out->kind;
        }

        d = &r->script->d[r->pc];
        switch (d->op) {
            case HYPE_INPUT_OP_TIMEOUT:
                r->timeout_ms = d->ms;
                r->pc++;
                continue;

            case HYPE_INPUT_OP_FAIL_IF:
                /* Arm it once. The array is sized to the directive limit, so it
                 * cannot overflow, but guard anyway rather than trust the caller
                 * handed us a script this parser produced. */
                if (r->failif_count < HYPE_INPUT_SCRIPT_MAX_DIRECTIVES) {
                    r->failif[r->failif_count] = r->pc;
                    r->failif_count++;
                }
                r->pc++;
                continue;

            case HYPE_INPUT_OP_SEND:
            case HYPE_INPUT_OP_SENDKEY:
            case HYPE_INPUT_OP_SENDMOUSE:
                /* Identical control flow; only the transport differs, which is the
                 * caller's business. Sharing the case is deliberate -- duplicating it
                 * would be three places for the "returned exactly once per directive"
                 * rule to drift apart. */
                r->pc++;
                r->phase_started = 0;
                out->kind = (d->op == HYPE_INPUT_OP_SENDKEY)     ? HYPE_INPUT_ACTION_SENDKEY
                            : (d->op == HYPE_INPUT_OP_SENDMOUSE) ? HYPE_INPUT_ACTION_SENDMOUSE
                                                                 : HYPE_INPUT_ACTION_SEND;
                out->data = d->text;
                out->len = d->len;
                return out->kind;

            case HYPE_INPUT_OP_DELAY:
                if (!r->phase_started) {
                    r->phase_started = 1;
                    r->phase_start_ms = now_ms;
                }
                if (now_ms - r->phase_start_ms >= (uint64_t)d->ms) {
                    r->pc++;
                    r->phase_started = 0;
                    continue;
                }
                return out->kind; /* WAIT */

            case HYPE_INPUT_OP_EXPECT:
                if (!r->phase_started) {
                    r->phase_started = 1;
                    r->phase_start_ms = now_ms;
                }
                /*
                 * A timeout is a RESULT, not a hang. The pattern that was not seen
                 * goes into the verdict, because "timed out" without naming what it
                 * waited for sends the reader back into an interleaved log to guess.
                 */
                if (now_ms - r->phase_start_ms >= (uint64_t)r->timeout_ms) {
                    finish(r, HYPE_INPUT_VERDICT_FAIL, HYPE_INPUT_REASON_EXPECT_TIMEOUT, d->text,
                           d->len, d->line);
                    out->kind = HYPE_INPUT_ACTION_DONE;
                    return out->kind;
                }
                return out->kind; /* WAIT -- feed() is what satisfies an expect */

            case HYPE_INPUT_OP_PASS:
                finish(r, HYPE_INPUT_VERDICT_PASS, HYPE_INPUT_REASON_PASS_DIRECTIVE, d->text,
                       d->len, d->line);
                out->kind = HYPE_INPUT_ACTION_DONE;
                return out->kind;

            case HYPE_INPUT_OP_FAIL:
            default:
                finish(r, HYPE_INPUT_VERDICT_FAIL, HYPE_INPUT_REASON_FAIL_DIRECTIVE, d->text,
                       d->len, d->line);
                out->kind = HYPE_INPUT_ACTION_DONE;
                return out->kind;
        }
    }
}

hype_input_verdict_t hype_input_runner_verdict(const hype_input_runner_t *r) {
    return r->verdict;
}

hype_input_reason_t hype_input_runner_reason(const hype_input_runner_t *r) {
    return r->reason;
}

const char *hype_input_reason_str(hype_input_reason_t reason) {
    switch (reason) {
        case HYPE_INPUT_REASON_NONE: return "still running";
        case HYPE_INPUT_REASON_PASS_DIRECTIVE: return "pass";
        case HYPE_INPUT_REASON_FAIL_DIRECTIVE: return "fail";
        case HYPE_INPUT_REASON_EXPECT_TIMEOUT: return "expect timed out";
        case HYPE_INPUT_REASON_FAIL_IF_MATCHED: return "fail-if pattern appeared";
        case HYPE_INPUT_REASON_RAN_OFF_END: return "script ended without pass/fail";
        case HYPE_INPUT_REASON_TRANSPORT_STALL: return "input transport stalled";
        default: return "unknown";
    }
}
