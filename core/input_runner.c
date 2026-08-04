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
        if (win_ends_with(r, fd->text, fd->len)) {
            finish(r, HYPE_INPUT_VERDICT_FAIL, HYPE_INPUT_REASON_FAIL_IF_MATCHED, fd->text, fd->len,
                   fd->line);
            return;
        }
    }

    if (r->pc < r->script->count) {
        const hype_input_directive_t *d = &r->script->d[r->pc];
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
                /* Identical control flow; only the transport differs, which is the
                 * caller's business. Sharing the case is deliberate -- duplicating it
                 * would be two places for the "returned exactly once per directive"
                 * rule to drift apart. */
                r->pc++;
                r->phase_started = 0;
                out->kind = (d->op == HYPE_INPUT_OP_SENDKEY) ? HYPE_INPUT_ACTION_SENDKEY
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
        default: return "unknown";
    }
}
