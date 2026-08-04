#ifndef HYPE_CORE_INPUT_SCRIPT_H
#define HYPE_CORE_INPUT_SCRIPT_H

#include <stdint.h>

/*
 * INPUT-5 (#278): parser for the per-VM expect-style input scripts described in
 * plan.md §6k. Pure text -> directive list: no I/O, no device knowledge, no clock,
 * so it unit-tests in isolation. #279 turns the list into a state machine, #280
 * injects its bytes into the guest UART, #281 loads the file off the ESP.
 *
 * Line-oriented and deliberately small -- this is a test fixture, not a shell:
 *
 *     timeout 120000                       # ms, applies to each expect
 *     expect  localhost login:
 *     send    root\n
 *     expect  ~#
 *     send    echo vm0-marker > /tmp/m\n
 *     fail-if vm1-marker
 *     pass    isolation-vm0
 *
 * A typo'd directive must NOT parse as a harmless no-op: a script that types
 * nothing and then times out is indistinguishable in a log from a guest that never
 * booted. Unknown directives, malformed escapes, missing arguments and capacity
 * overflow are all hard errors that name the offending line.
 */

/* Enough for the isolation scripts and any plausible sibling; the parser reports
 * overflow rather than silently truncating, so raising this is safe. */
#define HYPE_INPUT_SCRIPT_MAX_DIRECTIVES 64u
/* Longest expect pattern / send payload. The runner's rolling match window is
 * sized from this, so it bounds memory in #279 too. */
#define HYPE_INPUT_SCRIPT_MAX_ARG 128u

typedef enum {
    HYPE_INPUT_OP_EXPECT = 0, /* wait for `text` to appear in the guest's output */
    HYPE_INPUT_OP_SEND,       /* type `text` into the guest over ttyS0 */
    /*
     * INPUT-11 (#284): type `text` into the guest's emulated KEYBOARD instead of its
     * serial port -- for firmware and guests that read a keyboard: OVMF's boot menu,
     * GRUB, graphical installers. Same runner, second transport.
     */
    HYPE_INPUT_OP_SENDKEY,
    HYPE_INPUT_OP_DELAY,      /* pause `ms` */
    HYPE_INPUT_OP_TIMEOUT,    /* set the per-expect timeout to `ms` */
    HYPE_INPUT_OP_FAIL_IF,    /* arm `text`: if it EVER appears, the run fails */
    HYPE_INPUT_OP_PASS,       /* end the script, verdict PASS, label `text` */
    HYPE_INPUT_OP_FAIL        /* end the script, verdict FAIL, label `text` */
} hype_input_op_t;

typedef struct {
    hype_input_op_t op;
    /* Payload for expect/send/fail-if/pass/fail. NOT null-terminated by contract:
     * `send` may legitimately carry a NUL, and `len` is authoritative. */
    uint8_t text[HYPE_INPUT_SCRIPT_MAX_ARG];
    uint32_t len;
    uint32_t ms; /* delay/timeout only */
    uint32_t line; /* 1-based source line, for error and progress reporting */
} hype_input_directive_t;

typedef struct {
    hype_input_directive_t d[HYPE_INPUT_SCRIPT_MAX_DIRECTIVES];
    uint32_t count;
} hype_input_script_t;

typedef enum {
    HYPE_INPUT_PARSE_OK = 0,
    HYPE_INPUT_PARSE_UNKNOWN_DIRECTIVE,
    HYPE_INPUT_PARSE_MISSING_ARG,
    HYPE_INPUT_PARSE_BAD_NUMBER,
    HYPE_INPUT_PARSE_BAD_ESCAPE,
    HYPE_INPUT_PARSE_ARG_TOO_LONG,
    HYPE_INPUT_PARSE_TOO_MANY_DIRECTIVES
} hype_input_parse_status_t;

typedef struct {
    hype_input_parse_status_t status;
    uint32_t line; /* 1-based line the error is on; 0 when status is OK */
} hype_input_parse_result_t;

/*
 * Parse `text` (`len` bytes, need not be null-terminated) into *out.
 *
 * Accepts LF and CRLF line endings -- the script is authored on a FAT ESP, so it
 * may well arrive with CRLF from Windows. `#` begins a comment to end of line;
 * blank and comment-only lines are skipped. Leading and trailing whitespace around
 * the directive and its argument is stripped, EXCEPT that a `send` argument keeps
 * its interior spacing (a shell command needs its spaces).
 *
 * `send` understands exactly four escapes -- \n \r \t \\ -- and rejects any other
 * backslash sequence rather than passing it through, so a typo cannot silently
 * become a literal.
 *
 * On error *out holds the directives parsed so far, which is useful for reporting
 * but must NOT be executed: a half-armed script is the failure mode this whole
 * design avoids. Callers check the status.
 */
hype_input_parse_result_t hype_input_script_parse(const char *text, uint32_t len,
                                                  hype_input_script_t *out);

/* Human-readable reason, for the boot log. */
const char *hype_input_parse_status_str(hype_input_parse_status_t st);

#endif /* HYPE_CORE_INPUT_SCRIPT_H */
