#include <stdio.h>
#include <string.h>
#include "../input_runner.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

static hype_input_script_t g_sc;

static void load(const char *text) {
    hype_input_parse_result_t pr = hype_input_script_parse(text, (uint32_t)strlen(text), &g_sc);
    if (pr.status != HYPE_INPUT_PARSE_OK) {
        printf("FAIL: test script does not parse (status=%d line=%u)\n", (int)pr.status, pr.line);
        failures++;
    }
}

static void feed_str(hype_input_runner_t *r, const char *s) {
    while (*s) {
        hype_input_runner_feed(r, (uint8_t)*s);
        s++;
    }
}

/* Drain every action available at `now`, returning the last SEND payload seen (or
 * NULL) so a test can assert on what got typed. */
static const hype_input_action_t *pump(hype_input_runner_t *r, uint64_t now) {
    static hype_input_action_t a;
    static hype_input_action_t last_send;
    int have_send = 0;
    for (;;) {
        hype_input_action_kind_t k = hype_input_runner_poll(r, now, &a);
        if (k == HYPE_INPUT_ACTION_SEND) {
            last_send = a;
            have_send = 1;
            continue; /* keep going: a script may send twice in a row */
        }
        break;
    }
    return have_send ? &last_send : 0;
}

static void check_send(const char *desc, const hype_input_action_t *a, const char *want) {
    uint32_t i, n = (uint32_t)strlen(want);
    if (a == 0) {
        printf("FAIL: %s: expected a SEND, got none\n", desc);
        failures++;
        return;
    }
    if (a->len != n) {
        printf("FAIL: %s: expected %u bytes, got %u\n", desc, n, a->len);
        failures++;
        return;
    }
    for (i = 0; i < n; i++) {
        if (a->data[i] != (uint8_t)want[i]) {
            printf("FAIL: %s: byte %u differs\n", desc, i);
            failures++;
            return;
        }
    }
}

static void test_happy_path(void) {
    hype_input_runner_t r;
    const hype_input_action_t *s;
    hype_input_action_t a;

    load("expect login:\nsend root\\n\nexpect ~#\npass ok-label\n");
    hype_input_runner_init(&r, &g_sc, 0);

    /* Nothing to do until the prompt appears. */
    CHECK_INT("waits for the first expect", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 10, &a));

    feed_str(&r, "some boot noise\nlogin:");
    s = pump(&r, 20);
    check_send("types root after the prompt", s, "root\n");

    feed_str(&r, "\r\nPassword ok\n~#");
    CHECK_INT("finishes once the shell prompt is seen", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 30, &a));
    CHECK_INT("verdict PASS", HYPE_INPUT_VERDICT_PASS, hype_input_runner_verdict(&r));
    CHECK_INT("reason is the pass directive", HYPE_INPUT_REASON_PASS_DIRECTIVE,
              hype_input_runner_reason(&r));
}

static void test_match_split_across_feeds(void) {
    hype_input_runner_t r;
    const char *p = "localhost login:";
    hype_input_action_t a;

    load("expect localhost login:\nsend x\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);

    /* One byte at a time, which is how a UART actually delivers it. A line-buffered
     * matcher would never fire here: this prompt has NO trailing newline. */
    while (*p) {
        hype_input_runner_feed(&r, (uint8_t)*p);
        p++;
    }
    CHECK_INT("byte-at-a-time match works", HYPE_INPUT_ACTION_SEND,
              hype_input_runner_poll(&r, 1, &a));
}

static void test_match_with_surrounding_noise(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("expect ~#\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    feed_str(&r, "[ 1.234] random kernel chatter\nmore stuff ~# trailing");
    CHECK_INT("matches mid-stream with noise either side", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("noise match still PASS", HYPE_INPUT_VERDICT_PASS, hype_input_runner_verdict(&r));
}

static void test_repeated_pattern_not_double_matched(void) {
    hype_input_runner_t r;
    const hype_input_action_t *s;
    hype_input_action_t a;

    /* Two expects for the SAME pattern around a command. The second must wait for a
     * FRESH occurrence, not be satisfied by what the first already consumed. */
    load("expect ~#\nsend cmd\\n\nexpect ~#\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);

    feed_str(&r, "~#");
    s = pump(&r, 1);
    check_send("first prompt triggers the command", s, "cmd\n");

    CHECK_INT("second expect is NOT already satisfied", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 2, &a));

    feed_str(&r, "output here\n~#");
    CHECK_INT("second expect satisfied by a fresh prompt", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 3, &a));
    CHECK_INT("repeated pattern PASS", HYPE_INPUT_VERDICT_PASS, hype_input_runner_verdict(&r));
}

static void test_expect_timeout(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("timeout 5000\nexpect never-appears\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 1000);

    CHECK_INT("waiting before the deadline", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 5000, &a));
    /* Clock started at the first poll that reached the expect, so the deadline is
     * relative to that, not to init. */
    CHECK_INT("times out at the deadline", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 10000, &a));
    CHECK_INT("timeout is a FAIL, not a hang", HYPE_INPUT_VERDICT_FAIL,
              hype_input_runner_verdict(&r));
    CHECK_INT("reason names the timeout", HYPE_INPUT_REASON_EXPECT_TIMEOUT,
              hype_input_runner_reason(&r));
    /* The unseen pattern must be reported, or the reader is left guessing. */
    CHECK_INT("timed-out pattern recorded", 13, r.detail_len);
}

static void test_fail_if_during_unrelated_expect(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    /* This is the case #283 depends on: the other guest's marker can appear at ANY
     * time, including while an unrelated expect is current. */
    load("fail-if vm1-marker\nexpect ~#\npass isolated\n");
    hype_input_runner_init(&r, &g_sc, 0);
    CHECK_INT("armed, then waiting on the expect", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 1, &a));

    feed_str(&r, "cat /tmp/marker\nvm1-marker\n");
    CHECK_INT("fail-if fires mid-expect", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 2, &a));
    CHECK_INT("fail-if is a FAIL", HYPE_INPUT_VERDICT_FAIL, hype_input_runner_verdict(&r));
    CHECK_INT("reason is the fail-if", HYPE_INPUT_REASON_FAIL_IF_MATCHED,
              hype_input_runner_reason(&r));
}

static void test_fail_if_does_not_fire_on_absence(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("fail-if vm1-marker\nexpect vm0-marker\npass isolated\n");
    hype_input_runner_init(&r, &g_sc, 0);
    feed_str(&r, "cat /tmp/marker\nvm0-marker\n");
    CHECK_INT("own marker passes", HYPE_INPUT_ACTION_DONE, hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("no false fail-if", HYPE_INPUT_VERDICT_PASS, hype_input_runner_verdict(&r));
}

static void test_delay(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("delay 1000\nsend go\\n\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 500);

    /* The delay clock starts when the directive becomes CURRENT (the first poll that
     * reaches it), not at init -- otherwise time spent on earlier expects would be
     * silently deducted from it. So this poll at 1000 starts it and the deadline is
     * 2000, not 1500. My first version of this test asserted 1500 and was wrong. */
    CHECK_INT("delay starts at the first poll that reaches it", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 1000, &a));
    CHECK_INT("delay still holding just short", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 1999, &a));
    CHECK_INT("delay releases at the deadline", HYPE_INPUT_ACTION_SEND,
              hype_input_runner_poll(&r, 2000, &a));
}

static void test_transport_stall_is_terminal_failure(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("send hello\npass misleading-success\n");
    hype_input_runner_init(&r, &g_sc, 0);
    CHECK_INT("send is issued", HYPE_INPUT_ACTION_SEND, hype_input_runner_poll(&r, 0, &a));
    hype_input_runner_transport_stalled(&r);
    CHECK_INT("stalled transport finishes", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("stalled transport fails", HYPE_INPUT_VERDICT_FAIL,
              hype_input_runner_verdict(&r));
    CHECK_INT("stall reason", HYPE_INPUT_REASON_TRANSPORT_STALL,
              hype_input_runner_reason(&r));
}

static void test_send_delivered_once(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("send one\\n\nsend two\\n\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);

    CHECK_INT("first send", HYPE_INPUT_ACTION_SEND, hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("first send payload len", 4, a.len);
    CHECK_INT("second send is a DIFFERENT directive", HYPE_INPUT_ACTION_SEND,
              hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("second send payload len", 4, a.len);
    /* Not re-offered: re-sending would double-type a command. */
    CHECK_INT("no third send", HYPE_INPUT_ACTION_DONE, hype_input_runner_poll(&r, 1, &a));
}

static void test_ran_off_end_is_fail(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    /* A script that asserts nothing must NOT be counted as success -- that is how a
     * test comes to "pass" while checking nothing. */
    load("send hello\\n\n");
    hype_input_runner_init(&r, &g_sc, 0);
    CHECK_INT("the send happens", HYPE_INPUT_ACTION_SEND, hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("then it ends", HYPE_INPUT_ACTION_DONE, hype_input_runner_poll(&r, 2, &a));
    CHECK_INT("running off the end is FAIL", HYPE_INPUT_VERDICT_FAIL,
              hype_input_runner_verdict(&r));
    CHECK_INT("with its own distinguishable reason", HYPE_INPUT_REASON_RAN_OFF_END,
              hype_input_runner_reason(&r));
}

static void test_empty_script_still_yields_a_verdict(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("# nothing but a comment\n");
    CHECK_INT("empty script parsed to nothing", 0, g_sc.count);
    hype_input_runner_init(&r, &g_sc, 0);
    CHECK_INT("empty script terminates immediately", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 0, &a));
    /* "No verdict" must be unreachable, not merely unlikely. */
    CHECK_INT("empty script has a verdict", HYPE_INPUT_VERDICT_FAIL,
              hype_input_runner_verdict(&r));
    CHECK_INT("empty script has a reason", HYPE_INPUT_REASON_RAN_OFF_END,
              hype_input_runner_reason(&r));
}

static void test_fail_directive(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("fail gave-up\n");
    hype_input_runner_init(&r, &g_sc, 0);
    CHECK_INT("fail directive ends the run", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 0, &a));
    CHECK_INT("explicit fail", HYPE_INPUT_VERDICT_FAIL, hype_input_runner_verdict(&r));
    CHECK_INT("explicit fail reason", HYPE_INPUT_REASON_FAIL_DIRECTIVE,
              hype_input_runner_reason(&r));
}

static void test_feed_after_done_is_inert(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    load("expect a\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    feed_str(&r, "a");
    CHECK_INT("done", HYPE_INPUT_ACTION_DONE, hype_input_runner_poll(&r, 1, &a));
    /* Late output must not disturb a settled verdict -- the guest keeps talking long
     * after a script finishes. */
    feed_str(&r, "aaa more output");
    CHECK_INT("verdict unchanged by later output", HYPE_INPUT_VERDICT_PASS,
              hype_input_runner_verdict(&r));
    CHECK_INT("still DONE", HYPE_INPUT_ACTION_DONE, hype_input_runner_poll(&r, 2, &a));
}

static void test_window_overflow_still_matches(void) {
    hype_input_runner_t r;
    hype_input_action_t a;
    unsigned i;

    load("expect TAIL\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    /* Far more output than the window holds, then the pattern. The rolling window
     * must keep the most RECENT bytes, not the first ones. */
    for (i = 0; i < HYPE_INPUT_SCRIPT_MAX_ARG * 4u; i++) {
        hype_input_runner_feed(&r, (uint8_t)('a' + (i % 26u)));
    }
    feed_str(&r, "TAIL");
    CHECK_INT("match after window overflow", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("overflow match PASS", HYPE_INPUT_VERDICT_PASS, hype_input_runner_verdict(&r));
}

static void test_two_runners_independent(void) {
    hype_input_runner_t a_run, b_run;
    hype_input_script_t sa, sb;
    hype_input_parse_result_t pr;
    hype_input_action_t act;

    /* #280 gives every VM its own runner; if they shared state, guest 0's password
     * would be typed into guest 1. */
    pr = hype_input_script_parse("expect A\npass a-done\n", 20, &sa);
    CHECK_INT("script A parses", HYPE_INPUT_PARSE_OK, pr.status);
    pr = hype_input_script_parse("expect B\npass b-done\n", 20, &sb);
    CHECK_INT("script B parses", HYPE_INPUT_PARSE_OK, pr.status);

    hype_input_runner_init(&a_run, &sa, 0);
    hype_input_runner_init(&b_run, &sb, 0);

    feed_str(&a_run, "xxA");
    CHECK_INT("A finished", HYPE_INPUT_ACTION_DONE, hype_input_runner_poll(&a_run, 1, &act));
    CHECK_INT("B untouched by A's input", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&b_run, 1, &act));
    CHECK_INT("B still pending", HYPE_INPUT_VERDICT_PENDING, hype_input_runner_verdict(&b_run));

    feed_str(&b_run, "B");
    CHECK_INT("B finishes on its own input", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&b_run, 2, &act));
}

static void test_directives_reached_mid_script(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    /*
     * `timeout` and `fail-if` are armed at init and after each satisfied expect, but
     * they can also be reached mid-script -- e.g. straight after a `send`, which
     * returns immediately without draining what follows. Both paths must work, so
     * both are exercised.
     */
    load("send go\\n\ntimeout 50\nexpect nope\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    CHECK_INT("send first", HYPE_INPUT_ACTION_SEND, hype_input_runner_poll(&r, 0, &a));
    /* The timeout after the send must take effect: at t=60 the 50ms deadline has
     * passed. If poll ignored a mid-script `timeout` we would still be waiting on the
     * 120s default. */
    CHECK_INT("mid-script timeout applies", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 10, &a));
    CHECK_INT("mid-script timeout fires", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 61, &a));
    CHECK_INT("mid-script timeout -> FAIL", HYPE_INPUT_VERDICT_FAIL,
              hype_input_runner_verdict(&r));
    CHECK_INT("mid-script timeout reason", HYPE_INPUT_REASON_EXPECT_TIMEOUT,
              hype_input_runner_reason(&r));

    /* Same for a fail-if armed only after a send. */
    load("send go\\n\nfail-if boom\nexpect never\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    CHECK_INT("send", HYPE_INPUT_ACTION_SEND, hype_input_runner_poll(&r, 0, &a));
    CHECK_INT("now parked on the expect", HYPE_INPUT_ACTION_WAIT,
              hype_input_runner_poll(&r, 1, &a));
    feed_str(&r, "...boom...");
    CHECK_INT("mid-script fail-if fires", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 2, &a));
    CHECK_INT("mid-script fail-if reason", HYPE_INPUT_REASON_FAIL_IF_MATCHED,
              hype_input_runner_reason(&r));
}

static void test_feed_before_any_poll(void) {
    hype_input_runner_t r;
    hype_input_action_t a;

    /* Order independence: a caller may feed output before ever polling. Before this
     * was fixed the pc sat on the fail-if, so the expect behind it was not current
     * and could never match -- the runner silently waited forever. */
    load("timeout 9000\nfail-if bad\nexpect ready\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    feed_str(&r, "system ready");
    CHECK_INT("expect matched without a prior poll", HYPE_INPUT_ACTION_DONE,
              hype_input_runner_poll(&r, 1, &a));
    CHECK_INT("feed-before-poll PASS", HYPE_INPUT_VERDICT_PASS, hype_input_runner_verdict(&r));
}

static void test_reason_strings(void) {
    hype_input_reason_t all[] = {
        HYPE_INPUT_REASON_NONE,           HYPE_INPUT_REASON_PASS_DIRECTIVE,
        HYPE_INPUT_REASON_FAIL_DIRECTIVE, HYPE_INPUT_REASON_EXPECT_TIMEOUT,
        HYPE_INPUT_REASON_FAIL_IF_MATCHED, HYPE_INPUT_REASON_RAN_OFF_END
    };
    unsigned i;
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *s = hype_input_reason_str(all[i]);
        if (s == 0 || s[0] == '\0' || strcmp(s, "unknown") == 0) {
            printf("FAIL: reason %u has no message\n", (unsigned)all[i]);
            failures++;
        }
    }
    CHECK_INT("out-of-range reason still returns a string", 0,
              hype_input_reason_str((hype_input_reason_t)999) == 0);
}


/*
 * #302: matching against the reconstructed SCREEN.
 *
 * A full-screen program never puts its text on the wire contiguously -- GRUB's menu arrives as
 * cursor positioning and paint spaces -- so the streaming matcher cannot see it, and every
 * keyboard-reading consumer `sendkey` exists for is such a program.
 */
static void test_scan_matches_text_the_wire_never_carried(void) {
    hype_input_runner_t r;
    load("timeout 5000\nexpect GNU GRUB\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);

    /* What GRUB actually sends: positioning between the characters. The pattern is NOT present as
     * consecutive bytes, so the streaming matcher must NOT match it. */
    feed_str(&r, "\x1b[5;10HG\x1b[5;11HN\x1b[5;12HU\x1b[5;13H \x1b[5;14HG\x1b[5;15HR"
                 "\x1b[5;16HU\x1b[5;17HB");
    pump(&r, 1);
    CHECK_INT("wire bytes alone do not satisfy the expect", (int)HYPE_INPUT_VERDICT_PENDING,
              (int)hype_input_runner_verdict(&r));

    /* The reconstructed screen, which is what the operator sees and what vt_screen builds. */
    hype_input_runner_scan(&r, (const uint8_t *)"    GNU GRUB  version 2.12    ", 30u);
    pump(&r, 2);
    CHECK_INT("the screen snapshot satisfies it", (int)HYPE_INPUT_VERDICT_PASS,
              (int)hype_input_runner_verdict(&r));
}

/* The two matchers share one rolling window, so a scan landing mid-pattern must not destroy a
 * wire match in flight. This is the regression that would silently break every existing script. */
static void test_scan_does_not_disturb_a_wire_match_in_progress(void) {
    hype_input_runner_t r;
    load("timeout 5000\nexpect localhost login:\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);

    feed_str(&r, "localhost log");                       /* half the pattern on the wire */
    hype_input_runner_scan(&r, (const uint8_t *)"an unrelated screen full of text", 32u);
    feed_str(&r, "in:");                                 /* the rest */
    pump(&r, 1);
    CHECK_INT("the split wire match still completes", (int)HYPE_INPUT_VERDICT_PASS,
              (int)hype_input_runner_verdict(&r));
}

/* ...and the converse: the wire must not lend its tail to the screen. */
static void test_scan_does_not_splice_onto_wire_bytes(void) {
    hype_input_runner_t r;
    load("timeout 5000\nexpect ABCD\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);

    feed_str(&r, "AB");                                  /* wire ends mid-pattern */
    hype_input_runner_scan(&r, (const uint8_t *)"CD is on screen", 15u);
    pump(&r, 1);
    /* "AB" + "CD" appeared in NEITHER place as a whole. Matching would be inventing an event. */
    CHECK_INT("no match spliced across wire and screen", (int)HYPE_INPUT_VERDICT_PENDING,
              (int)hype_input_runner_verdict(&r));
}

static void test_scan_edge_inputs(void) {
    hype_input_runner_t r;
    load("timeout 5000\nexpect X\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);
    hype_input_runner_scan(&r, 0, 5u);                              /* null */
    hype_input_runner_scan(&r, (const uint8_t *)"X", 0u);           /* zero length */
    CHECK_INT("neither advances the run", (int)HYPE_INPUT_VERDICT_PENDING,
              (int)hype_input_runner_verdict(&r));
    hype_input_runner_scan(&r, (const uint8_t *)"X", 1u);
    pump(&r, 1);
    CHECK_INT("a real snapshot does", (int)HYPE_INPUT_VERDICT_PASS,
              (int)hype_input_runner_verdict(&r));

    /* Scanning after the verdict must be inert. The caller is on a timer and will keep handing
     * over snapshots after the run ends; a late screen must not rewrite the result. */
    hype_input_runner_scan(&r, (const uint8_t *)"anything at all", 15u);
    CHECK_INT("a scan after the run is a no-op", (int)HYPE_INPUT_VERDICT_PASS,
              (int)hype_input_runner_verdict(&r));
}

/*
 * #728: the false-pass regression. A screen scan re-presents the whole display every
 * time, so an `expect` for a pattern ALREADY on screen used to be satisfied instantly
 * by history. On hw-val Boot 2b that reported PASS 124ms after `reboot` was sent, on a
 * guest that never restarted.
 */
static void test_scan_does_not_match_text_that_was_already_on_screen(void) {
    hype_input_runner_t r;
    static const char *screen = "localhost login: root\nlocalhost:~# reboot\n";
    load("timeout 5000\nexpect reboot\nexpect localhost login:\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);

    /* The caller scans on a ~10Hz cadence throughout, so by the time the reboot is
     * issued the screen has already been offered while the FIRST expect was current.
     * That scan is what gives the gate a genuine "before" to measure against. */
    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));

    /* First expect matches off the wire; the pc moves to `expect localhost login:`
     * while that pattern is ALREADY on the screen from the previous boot. */
    feed_str(&r, "reboot");
    pump(&r, 1);

    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    pump(&r, 2);
    CHECK_INT("stale screen text does not satisfy the expect", (int)HYPE_INPUT_VERDICT_PENDING,
              (int)hype_input_runner_verdict(&r));

    /* Repeating the same screen must stay pending however many times it is offered. */
    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    pump(&r, 3);
    CHECK_INT("and still does not on a later scan", (int)HYPE_INPUT_VERDICT_PENDING,
              (int)hype_input_runner_verdict(&r));
}

/* ...and the gate must LIFT: once the pattern leaves the screen, its next appearance is
 * a genuine event. This is the other half -- a gate that never opened would turn the
 * false pass into a permanent hang. */
static void test_scan_gate_lifts_once_the_pattern_leaves_the_screen(void) {
    hype_input_runner_t r;
    static const char *before = "localhost login: root\nlocalhost:~# reboot\n";
    static const char *during = "Restarting system.\n";
    static const char *after = "Welcome to Alpine Linux\nlocalhost login:";
    load("timeout 5000\nexpect reboot\nexpect localhost login:\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);
    hype_input_runner_scan(&r, (const uint8_t *)before, (uint32_t)strlen(before)); /* the "before" */
    feed_str(&r, "reboot");
    pump(&r, 1);

    hype_input_runner_scan(&r, (const uint8_t *)before, (uint32_t)strlen(before));
    pump(&r, 2);
    CHECK_INT("gated while the stale prompt is up", (int)HYPE_INPUT_VERDICT_PENDING,
              (int)hype_input_runner_verdict(&r));

    hype_input_runner_scan(&r, (const uint8_t *)during, (uint32_t)strlen(during)); /* screen cleared */
    hype_input_runner_scan(&r, (const uint8_t *)after, (uint32_t)strlen(after));   /* it comes back */
    pump(&r, 3);
    CHECK_INT("the real second prompt does satisfy it", (int)HYPE_INPUT_VERDICT_PASS,
              (int)hype_input_runner_verdict(&r));
}

/*
 * The gate must not swallow text the script itself just caused. `sendkey Hello World`
 * followed by `expect Hello World` (tools/302's shape) can have the echo on screen
 * before the first scan after the expect is armed -- indistinguishable from a stale
 * banner at that instant, which is why the gate is measured from the screen BEFORE the
 * directive became current rather than after.
 */
static void test_scan_gate_does_not_block_output_the_script_just_caused(void) {
    hype_input_runner_t r;
    load("timeout 5000\nexpect grub>\nsendkey Hello World\nexpect Hello World\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);

    /* A screen with the prompt but NOT yet the typed text: this is the pre-measurement
     * that decides the next expect's gate. */
    hype_input_runner_scan(&r, (const uint8_t *)"grub> ", 6u);
    pump(&r, 1); /* matches grub>, then hands over the sendkey */

    /* By the next scan the keystrokes have echoed. That is new output, not history. */
    hype_input_runner_scan(&r, (const uint8_t *)"grub> Hello World", 17u);
    pump(&r, 2);
    CHECK_INT("echoed keystrokes still satisfy the expect", (int)HYPE_INPUT_VERDICT_PASS,
              (int)hype_input_runner_verdict(&r));
}

/* A fail-if is the same defect wearing the opposite verdict: stale screen text must not
 * fire it, but a genuine later appearance must. */
static void test_fail_if_ignores_stale_screen_text(void) {
    hype_input_runner_t r;
    load("timeout 5000\nfail-if soft lockup\nexpect DONE\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);

    hype_input_runner_scan(&r, (const uint8_t *)"old log: soft lockup on cpu 3", 29u);
    pump(&r, 1);
    CHECK_INT("pre-existing screen text does not trip fail-if",
              (int)HYPE_INPUT_VERDICT_PENDING, (int)hype_input_runner_verdict(&r));

    hype_input_runner_scan(&r, (const uint8_t *)"a clean screen", 14u);
    hype_input_runner_scan(&r, (const uint8_t *)"BUG: soft lockup - CPU#0 stuck", 30u);
    pump(&r, 2);
    CHECK_INT("but a fresh occurrence does", (int)HYPE_INPUT_VERDICT_FAIL,
              (int)hype_input_runner_verdict(&r));
}

/* The wire is a true stream -- a byte arriving on it has by definition just appeared --
 * so the gate must never apply there, even for a pattern sitting on the screen. */
static void test_wire_match_is_never_gated_by_the_screen(void) {
    hype_input_runner_t r;
    static const char *screen = "READY is already on screen";
    load("timeout 5000\nexpect GO\nexpect READY\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);
    pump(&r, 0);

    /* Establishes the "before": READY is on screen while the FIRST expect is current. */
    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    feed_str(&r, "GO");
    pump(&r, 1);

    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    pump(&r, 2);
    CHECK_INT("the stale screen is gated", (int)HYPE_INPUT_VERDICT_PENDING,
              (int)hype_input_runner_verdict(&r));

    /* The wire is a true stream: a byte arriving on it has by definition just
     * appeared, so the gate must never apply there. */
    feed_str(&r, "READY");
    pump(&r, 3);
    CHECK_INT("the wire still matches while the screen is gated",
              (int)HYPE_INPUT_VERDICT_PASS, (int)hype_input_runner_verdict(&r));
}

/*
 * #728 / #731: the shell-prompt case, which is the most COMMON repeated expect in
 * this repo (`expect ~#` appears around nearly every command in every script).
 *
 * The prompt is essentially always on screen, so the gate stays closed for its whole
 * life -- and that is fine, because the prompt is also re-emitted on the WIRE after
 * every command, and the wire is never gated. This test pins that down: the pattern
 * being permanently visible must not stop the script progressing, and each command's
 * prompt must be matched from its own output rather than from the previous one still
 * being displayed.
 */
static void test_shell_prompt_progresses_from_the_wire_while_permanently_on_screen(void) {
    hype_input_runner_t r;
    static const char *screen = "localhost:~# first\nlocalhost:~# ";
    load("timeout 5000\nexpect ~#\nsend one\\n\nexpect ~#\nsend two\\n\nexpect ~#\npass ok\n");
    hype_input_runner_init(&r, &g_sc, 0);

    /* The prompt is on screen from the very start and never leaves. */
    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    pump(&r, 0);

    /* Each command's own prompt arrives on the wire; the screen never changes. */
    feed_str(&r, "localhost:~# ");
    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    pump(&r, 1);
    feed_str(&r, "one\nlocalhost:~# ");
    hype_input_runner_scan(&r, (const uint8_t *)screen, (uint32_t)strlen(screen));
    pump(&r, 2);
    feed_str(&r, "two\nlocalhost:~# ");
    pump(&r, 3);

    CHECK_INT("a permanently-visible prompt still lets the script finish",
              (int)HYPE_INPUT_VERDICT_PASS, (int)hype_input_runner_verdict(&r));
}

int main(void) {
    test_happy_path();
    test_match_split_across_feeds();
    test_match_with_surrounding_noise();
    test_repeated_pattern_not_double_matched();
    test_expect_timeout();
    test_fail_if_during_unrelated_expect();
    test_fail_if_does_not_fire_on_absence();
    test_delay();
    test_transport_stall_is_terminal_failure();
    test_send_delivered_once();
    test_ran_off_end_is_fail();
    test_empty_script_still_yields_a_verdict();
    test_fail_directive();
    test_feed_after_done_is_inert();
    test_window_overflow_still_matches();
    test_two_runners_independent();
    test_directives_reached_mid_script();
    test_feed_before_any_poll();
    test_reason_strings();

    test_scan_matches_text_the_wire_never_carried();
    test_scan_does_not_disturb_a_wire_match_in_progress();
    test_scan_does_not_splice_onto_wire_bytes();
    test_scan_edge_inputs();
    test_scan_does_not_match_text_that_was_already_on_screen();
    test_scan_gate_lifts_once_the_pattern_leaves_the_screen();
    test_scan_gate_does_not_block_output_the_script_just_caused();
    test_fail_if_ignores_stale_screen_text();
    test_wire_match_is_never_gated_by_the_screen();
    test_shell_prompt_progresses_from_the_wire_while_permanently_on_screen();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
