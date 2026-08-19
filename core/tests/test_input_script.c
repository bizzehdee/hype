#include <stdio.h>
#include <string.h>
#include "../input_script.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Payloads are length-counted, not null-terminated (a `send` may carry a NUL), so
 * compare explicitly rather than with strcmp. */
static void check_payload(const char *desc, const hype_input_directive_t *d, const char *want,
                          uint32_t want_len) {
    uint32_t i;
    if (d->len != want_len) {
        printf("FAIL: %s: expected len %u, got %u\n", desc, want_len, d->len);
        failures++;
        return;
    }
    for (i = 0; i < want_len; i++) {
        if (d->text[i] != (uint8_t)want[i]) {
            printf("FAIL: %s: byte %u expected 0x%02x, got 0x%02x\n", desc, i,
                   (unsigned)(uint8_t)want[i], (unsigned)d->text[i]);
            failures++;
            return;
        }
    }
}

static hype_input_parse_result_t parse(const char *s, hype_input_script_t *out) {
    return hype_input_script_parse(s, (uint32_t)strlen(s), out);
}

static void test_every_directive(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r = parse("timeout 120000\n"
                                       "expect localhost login:\n"
                                       "send root\\n\n"
                                       "delay 500\n"
                                       "fail-if vm1-marker\n"
                                       "pass isolation-vm0\n"
                                       "fail gave-up\n",
                                       &sc);
    CHECK_INT("all directives parse", HYPE_INPUT_PARSE_OK, r.status);
    CHECK_INT("directive count", 7, sc.count);
    CHECK_INT("timeout op", HYPE_INPUT_OP_TIMEOUT, sc.d[0].op);
    CHECK_INT("timeout ms", 120000, sc.d[0].ms);
    CHECK_INT("expect op", HYPE_INPUT_OP_EXPECT, sc.d[1].op);
    check_payload("expect payload keeps interior spaces", &sc.d[1], "localhost login:", 16);
    CHECK_INT("send op", HYPE_INPUT_OP_SEND, sc.d[2].op);
    check_payload("send resolves \\n", &sc.d[2], "root\n", 5);
    CHECK_INT("delay op", HYPE_INPUT_OP_DELAY, sc.d[3].op);
    CHECK_INT("delay ms", 500, sc.d[3].ms);
    CHECK_INT("fail-if op", HYPE_INPUT_OP_FAIL_IF, sc.d[4].op);
    CHECK_INT("pass op", HYPE_INPUT_OP_PASS, sc.d[5].op);
    CHECK_INT("fail op", HYPE_INPUT_OP_FAIL, sc.d[6].op);
    /* Line numbers are what an error message is worth anything without. */
    CHECK_INT("line numbers tracked", 3, sc.d[2].line);
}

static void test_escapes(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r = parse("send a\\nb\\rc\\td\\\\e\n", &sc);
    CHECK_INT("all four escapes ok", HYPE_INPUT_PARSE_OK, r.status);
    check_payload("escapes resolved", &sc.d[0], "a\nb\rc\td\\e", 9);

    /* An unknown escape must be an ERROR, not passed through: a typo'd \\d
     * silently becoming a literal backslash-d would make a script that types the
     * wrong thing and then times out, which reads as a broken guest. */
    r = parse("send oops\\q\n", &sc);
    CHECK_INT("unknown escape rejected", HYPE_INPUT_PARSE_BAD_ESCAPE, r.status);
    CHECK_INT("bad escape names its line", 1, r.line);

    r = parse("expect a\nsend trailing\\\n", &sc);
    CHECK_INT("trailing backslash rejected", HYPE_INPUT_PARSE_BAD_ESCAPE, r.status);
    CHECK_INT("trailing backslash line", 2, r.line);
}

static void test_comments_and_blanks(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r = parse("# leading comment\n"
                                       "\n"
                                       "   \t \n"
                                       "   # indented comment\n"
                                       "expect x\n",
                                       &sc);
    CHECK_INT("comments/blanks skipped", HYPE_INPUT_PARSE_OK, r.status);
    CHECK_INT("only the real directive counted", 1, sc.count);
    /* The surviving directive must still report ITS line, not a renumbered one --
     * otherwise an error points at the wrong place in the operator's file. */
    CHECK_INT("line number survives skipped lines", 5, sc.d[0].line);

    /* A '#' inside a send payload is a shell comment, not a script comment, and
     * must reach the guest. */
    r = parse("send echo hi # not a comment\n", &sc);
    CHECK_INT("hash inside send kept", HYPE_INPUT_PARSE_OK, r.status);
    check_payload("send keeps its hash", &sc.d[0], "echo hi # not a comment", 23);
}

static void test_crlf(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r = parse("expect login:\r\nsend root\\n\r\n", &sc);
    CHECK_INT("CRLF parses", HYPE_INPUT_PARSE_OK, r.status);
    CHECK_INT("CRLF directive count", 2, sc.count);
    /* The \r must NOT survive into the pattern: the guest never sends it, so the
     * expect would wait forever. This is the whole reason CRLF is handled. */
    check_payload("CR stripped from expect", &sc.d[0], "login:", 6);
    check_payload("CR stripped from send", &sc.d[1], "root\n", 5);
}

static void test_whitespace(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r = parse("   expect    spaced   out   \n", &sc);
    CHECK_INT("leading/trailing whitespace ok", HYPE_INPUT_PARSE_OK, r.status);
    check_payload("interior spacing preserved, edges stripped", &sc.d[0], "spaced   out", 12);
}

static void test_errors(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r;

    r = parse("expect a\nexpectt b\n", &sc);
    CHECK_INT("near-miss keyword rejected", HYPE_INPUT_PARSE_UNKNOWN_DIRECTIVE, r.status);
    CHECK_INT("unknown directive line", 2, r.line);

    /* "fail" and "fail-if" are different directives; a prefix match would confuse
     * them and silently end the script instead of arming a pattern. */
    r = parse("fail-if x\n", &sc);
    CHECK_INT("fail-if is not fail", HYPE_INPUT_OP_FAIL_IF, sc.d[0].op);
    CHECK_INT("fail-if parses", HYPE_INPUT_PARSE_OK, r.status);

    r = parse("expect\n", &sc);
    CHECK_INT("missing arg rejected", HYPE_INPUT_PARSE_MISSING_ARG, r.status);
    r = parse("send   \n", &sc);
    CHECK_INT("whitespace-only arg is missing", HYPE_INPUT_PARSE_MISSING_ARG, r.status);

    r = parse("delay abc\n", &sc);
    CHECK_INT("non-numeric delay rejected", HYPE_INPUT_PARSE_BAD_NUMBER, r.status);
    r = parse("timeout 12x\n", &sc);
    CHECK_INT("trailing junk in number rejected", HYPE_INPUT_PARSE_BAD_NUMBER, r.status);
    /* Overflow must not wrap to a small timeout -- that would look like a flaky
     * guest rather than a bad script. */
    r = parse("timeout 99999999999999\n", &sc);
    CHECK_INT("overflowing number rejected", HYPE_INPUT_PARSE_BAD_NUMBER, r.status);
}

static void test_too_long_and_too_many(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r;
    char buf[4096];
    unsigned i, n = 0;

    n += (unsigned)sprintf(buf + n, "expect ");
    for (i = 0; i < HYPE_INPUT_SCRIPT_MAX_ARG + 5u; i++) {
        buf[n++] = 'x';
    }
    buf[n++] = '\n';
    buf[n] = '\0';
    r = hype_input_script_parse(buf, n, &sc);
    CHECK_INT("over-long arg rejected", HYPE_INPUT_PARSE_ARG_TOO_LONG, r.status);

    /* Exactly at the limit must be ACCEPTED -- an off-by-one here would reject a
     * legal script. */
    n = (unsigned)sprintf(buf, "expect ");
    for (i = 0; i < HYPE_INPUT_SCRIPT_MAX_ARG; i++) {
        buf[n++] = 'x';
    }
    buf[n++] = '\n';
    r = hype_input_script_parse(buf, n, &sc);
    CHECK_INT("exactly max-length arg accepted", HYPE_INPUT_PARSE_OK, r.status);
    CHECK_INT("max-length arg length", HYPE_INPUT_SCRIPT_MAX_ARG, sc.d[0].len);

    n = 0;
    for (i = 0; i < HYPE_INPUT_SCRIPT_MAX_DIRECTIVES + 2u; i++) {
        n += (unsigned)sprintf(buf + n, "expect a\n");
    }
    r = hype_input_script_parse(buf, n, &sc);
    CHECK_INT("over-capacity rejected, not truncated",
              HYPE_INPUT_PARSE_TOO_MANY_DIRECTIVES, r.status);
}

static void test_edge_inputs(void) {
    hype_input_script_t sc;
    hype_input_parse_result_t r;

    r = hype_input_script_parse("", 0, &sc);
    CHECK_INT("empty script is not an error", HYPE_INPUT_PARSE_OK, r.status);
    CHECK_INT("empty script has no directives", 0, sc.count);

    /* No trailing newline on the last line must still parse -- an operator's editor
     * may not add one. */
    r = parse("expect x", &sc);
    CHECK_INT("no trailing newline ok", HYPE_INPUT_PARSE_OK, r.status);
    CHECK_INT("last line counted", 1, sc.count);

    r = parse("\n\n\n", &sc);
    CHECK_INT("newlines only", HYPE_INPUT_PARSE_OK, r.status);
    CHECK_INT("newlines only -> no directives", 0, sc.count);
}

/*
 * #542: sendmouse. The parser folds the PS/2 status byte's sign bits so scripts never carry device
 * encoding -- so these tests are about that encoding being right, since a wrong sign bit moves the
 * pointer the opposite way and nothing in a script would show it.
 */
static void test_sendmouse(void) {
    hype_input_script_t sc;

    CHECK_INT("positive deltas parse", HYPE_INPUT_PARSE_OK, parse("sendmouse 5 3\n", &sc).status);
    CHECK_INT("one directive", 1, (int)sc.count);
    CHECK_INT("op is sendmouse", (int)HYPE_INPUT_OP_SENDMOUSE, (int)sc.d[0].op);
    CHECK_INT("three bytes of payload", 3, (int)sc.d[0].len);
    /* 0x08 is the always-1 bit; no buttons, no sign bits. This is the same status the in-binary
     * INPUT-2 test used as its expected value, which is why it is spelled out. */
    CHECK_INT("status has the always-1 bit only", 0x08, (int)sc.d[0].text[0]);
    CHECK_INT("dx", 5, (int)sc.d[0].text[1]);
    CHECK_INT("dy", 3, (int)sc.d[0].text[2]);

    CHECK_INT("negative dx parses", HYPE_INPUT_PARSE_OK, parse("sendmouse -5 3\n", &sc).status);
    CHECK_INT("dx sign bit (0x10) set", 0x18, (int)sc.d[0].text[0]);
    CHECK_INT("dx is the two's-complement byte", 0xFB, (int)sc.d[0].text[1]);

    CHECK_INT("negative dy parses", HYPE_INPUT_PARSE_OK, parse("sendmouse 5 -3\n", &sc).status);
    CHECK_INT("dy sign bit (0x20) set", 0x28, (int)sc.d[0].text[0]);
    CHECK_INT("dy is the two's-complement byte", 0xFD, (int)sc.d[0].text[2]);

    CHECK_INT("both negative", HYPE_INPUT_PARSE_OK, parse("sendmouse -1 -1\n", &sc).status);
    CHECK_INT("both sign bits set", 0x38, (int)sc.d[0].text[0]);

    CHECK_INT("buttons parse", HYPE_INPUT_PARSE_OK, parse("sendmouse 0 0 5\n", &sc).status);
    CHECK_INT("left+middle in the low bits", 0x0D, (int)sc.d[0].text[0]);

    /* Extra whitespace between fields, and a leading +. */
    CHECK_INT("tolerant of spacing", HYPE_INPUT_PARSE_OK,
              parse("sendmouse   +7    -8   2\n", &sc).status);
    CHECK_INT("status", 0x2A, (int)sc.d[0].text[0]);
    CHECK_INT("dx", 7, (int)sc.d[0].text[1]);
    CHECK_INT("dy", 0xF8, (int)sc.d[0].text[2]);

    /* Out of range is a FAILURE, not a clamp: `sendmouse 500 0` asks for a move the device cannot
     * express, and clamping would move the pointer a different distance than the script said. */
    CHECK_INT("dx above 127 refused", HYPE_INPUT_PARSE_BAD_NUMBER,
              parse("sendmouse 500 0\n", &sc).status);
    CHECK_INT("dx below -128 refused", HYPE_INPUT_PARSE_BAD_NUMBER,
              parse("sendmouse -129 0\n", &sc).status);
    CHECK_INT("dy out of range refused", HYPE_INPUT_PARSE_BAD_NUMBER,
              parse("sendmouse 0 200\n", &sc).status);
    CHECK_INT("buttons above 7 refused", HYPE_INPUT_PARSE_BAD_NUMBER,
              parse("sendmouse 0 0 9\n", &sc).status);
    CHECK_INT("one field is not enough", HYPE_INPUT_PARSE_BAD_NUMBER,
              parse("sendmouse 5\n", &sc).status);
    CHECK_INT("non-numeric refused", HYPE_INPUT_PARSE_BAD_NUMBER,
              parse("sendmouse left 3\n", &sc).status);
    CHECK_INT("no argument at all refused", HYPE_INPUT_PARSE_MISSING_ARG,
              parse("sendmouse\n", &sc).status);
}

static void test_status_strings(void) {
    /* Every status needs a message: an error reported as "unknown error" wastes the
     * line number it came with. */
    hype_input_parse_status_t all[] = {
        HYPE_INPUT_PARSE_OK,          HYPE_INPUT_PARSE_UNKNOWN_DIRECTIVE,
        HYPE_INPUT_PARSE_MISSING_ARG, HYPE_INPUT_PARSE_BAD_NUMBER,
        HYPE_INPUT_PARSE_BAD_ESCAPE,  HYPE_INPUT_PARSE_ARG_TOO_LONG,
        HYPE_INPUT_PARSE_TOO_MANY_DIRECTIVES
    };
    unsigned i;
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *s = hype_input_parse_status_str(all[i]);
        if (s == 0 || s[0] == '\0' || strcmp(s, "unknown error") == 0) {
            printf("FAIL: status %u has no message\n", (unsigned)all[i]);
            failures++;
        }
    }
    CHECK_INT("out-of-range status still returns a string", 0,
              hype_input_parse_status_str((hype_input_parse_status_t)999) == 0);
}

int main(void) {
    test_every_directive();
    test_escapes();
    test_comments_and_blanks();
    test_crlf();
    test_whitespace();
    test_errors();
    test_too_long_and_too_many();
    test_edge_inputs();
    test_sendmouse();
    test_status_strings();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
