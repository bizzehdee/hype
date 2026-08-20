#include <stdio.h>
#include <string.h>
#include "../run_state.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* The whole point of the module: what one shutdown writes, the next boot must read back to the
 * same answers. Tested as a ROUND TRIP rather than against a golden string, because the contract
 * is the meaning, not the bytes -- a format change that both ends agree on is not a regression. */
static void test_round_trip(void) {
    hype_run_state_t w, r;
    char buf[512];
    unsigned int len = 0;

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_REBOOT);
    CHECK(hype_run_state_add(&w, "alpine", 1) == 0, "add alpine");
    CHECK(hype_run_state_add(&w, "build", 0) == 0, "add build");
    CHECK(hype_run_state_serialize(&w, buf, sizeof(buf), &len) == 0, "serialize");
    CHECK(len > 0 && len < sizeof(buf), "len %u", len);

    CHECK(hype_run_state_parse(buf, len, &r) == 0, "parse round trip");
    CHECK(r.malformed == 0, "round trip must not be malformed");
    CHECK(r.version == HYPE_RUN_STATE_VERSION, "version %u", r.version);
    CHECK(r.reason == HYPE_RUN_STATE_REASON_REBOOT, "reason %d", (int)r.reason);
    CHECK(r.count == 2u, "count %u", r.count);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_RUNNING, "alpine was running");
    CHECK(hype_run_state_lookup(&r, "build") == HYPE_RUN_STATE_STOPPED, "build was stopped");
}

/*
 * THREE ANSWERS, not two. A VM the operator added to hype.cfg since the last shutdown cannot be in
 * the record, and must not be held off by it -- "unknown" is a distinct answer from "stopped" so
 * the caller can keep its own default. Collapsing them would mean a newly-created VM never starts
 * until someone notices.
 */
static void test_unknown_is_not_stopped(void) {
    hype_run_state_t w, r;
    char buf[256];
    unsigned int len = 0;

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_OFF);
    hype_run_state_add(&w, "alpine", 0);
    CHECK(hype_run_state_serialize(&w, buf, sizeof(buf), &len) == 0, "serialize");
    CHECK(hype_run_state_parse(buf, len, &r) == 0, "parse");

    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_STOPPED, "named -> stopped");
    CHECK(hype_run_state_lookup(&r, "brand-new") == HYPE_RUN_STATE_UNKNOWN, "absent -> unknown");
    CHECK(hype_run_state_lookup(&r, "") == HYPE_RUN_STATE_UNKNOWN, "empty name -> unknown");
    CHECK(hype_run_state_lookup(&r, 0) == HYPE_RUN_STATE_UNKNOWN, "NULL name -> unknown");
}

/*
 * NAME-KEYED, and this is the test that says why. The operator reorders their [vm.*] sections
 * between boots; an index-keyed record would now start a different machine than the one that was
 * up. Reading the same record against a reversed name order must give the same per-name answers.
 */
static void test_reordering_does_not_move_the_answers(void) {
    hype_run_state_t w, r;
    char buf[256];
    unsigned int len = 0;
    const char *boot_order[2] = {"build", "alpine"}; /* reversed since the record was written */
    int want[2] = {HYPE_RUN_STATE_STOPPED, HYPE_RUN_STATE_RUNNING};
    unsigned int i;

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_REBOOT);
    hype_run_state_add(&w, "alpine", 1);
    hype_run_state_add(&w, "build", 0);
    hype_run_state_serialize(&w, buf, sizeof(buf), &len);
    hype_run_state_parse(buf, len, &r);

    for (i = 0; i < 2u; i++) {
        CHECK(hype_run_state_lookup(&r, boot_order[i]) == want[i],
              "'%s' answered %d, wanted %d", boot_order[i], hype_run_state_lookup(&r, boot_order[i]),
              want[i]);
    }
}

/*
 * A VERSION THIS BUILD DOES NOT KNOW REFUSES THE WHOLE RECORD. Reading the `vm` lines that still
 * happen to parse would produce a half-restored host, which an operator cannot tell apart from a
 * correctly restored one. So count must be 0 and every lookup must answer unknown -- including for
 * a VM the file plainly names.
 */
static void test_future_version_refuses_everything(void) {
    const char *text = "version = 99\nvm alpine = running\nvm build = running\n";
    hype_run_state_t r;

    CHECK(hype_run_state_parse(text, (unsigned int)strlen(text), &r) == -1, "must refuse");
    CHECK(r.malformed == 1u, "malformed must be set");
    CHECK(r.count == 0u, "count %u -- a refused record describes nothing", r.count);
    CHECK(r.version == 99u, "the version read should still be reported, got %u", r.version);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_UNKNOWN,
          "a refused record must have no opinion even about a VM it names");
}

/* No version line is the same refusal: the file is either not ours, or was written by something
 * that did not finish. Both readings say do not act on it. */
static void test_missing_version_refuses(void) {
    const char *text = "reason = off\nvm alpine = running\n";
    hype_run_state_t r;

    CHECK(hype_run_state_parse(text, (unsigned int)strlen(text), &r) == -1, "must refuse");
    CHECK(r.malformed == 1u, "malformed");
    CHECK(r.count == 0u, "count %u", r.count);
}

/* An empty or absent file is a refusal, not an empty record. "Nothing was running" and "there is
 * no record" are different facts and only one of them means "stop every VM". */
static void test_empty_input_refuses(void) {
    hype_run_state_t r;

    CHECK(hype_run_state_parse("", 0u, &r) == -1, "empty text");
    CHECK(r.malformed == 1u, "malformed on empty");
    CHECK(hype_run_state_parse(0, 10u, &r) == -1, "NULL text");
    CHECK(r.malformed == 1u, "malformed on NULL");
}

/* A state word that is neither running nor stopped refuses: the two answers lead to opposite
 * actions on the next boot, so guessing one is worse than doing nothing. */
static void test_unknown_state_word_refuses(void) {
    const char *text = "version = 1\nvm alpine = paused\n";
    hype_run_state_t r;

    CHECK(hype_run_state_parse(text, (unsigned int)strlen(text), &r) == -1, "must refuse");
    CHECK(r.count == 0u, "count %u", r.count);
}

/* A `vm` line with no name cannot be looked up, so it is damage rather than a key from a newer
 * build. Same for a line with no `=` at all. */
static void test_damaged_lines_refuse(void) {
    hype_run_state_t r;
    const char *noname = "version = 1\nvm = running\n";
    const char *noeq = "version = 1\nthis line has no equals sign\n";
    const char *blankname = "version = 1\nvm    = running\n";

    CHECK(hype_run_state_parse(noname, (unsigned int)strlen(noname), &r) == -1, "vm with no name");
    CHECK(hype_run_state_parse(noeq, (unsigned int)strlen(noeq), &r) == -1, "line with no =");
    CHECK(hype_run_state_parse(blankname, (unsigned int)strlen(blankname), &r) == -1,
          "vm with blank name");
}

/*
 * A key from a NEWER hype is tolerated, unlike damage: an unknown key means the file was written
 * by a build that knows something extra, and the `vm` lines it also wrote are still exactly what
 * they say. Counted, so a rising number is visible, but not a refusal. This is the same
 * distinction hype.cfg draws between an unknown key and a syntax error.
 */
static void test_unknown_key_is_tolerated(void) {
    const char *text = "version = 1\nfuture_thing = 42\nvm alpine = running\n";
    hype_run_state_t r;

    CHECK(hype_run_state_parse(text, (unsigned int)strlen(text), &r) == 0, "must be accepted");
    CHECK(r.malformed == 0u, "not malformed");
    CHECK(r.unknown_keys == 1u, "unknown_keys %u", r.unknown_keys);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_RUNNING, "alpine still running");
}

/* Comments, blank lines, CRLF and surrounding whitespace are all ordinary. The file is meant to be
 * read and edited by an operator on a host that is not working, so it must survive being opened in
 * whatever editor that host has. */
static void test_whitespace_comments_and_crlf(void) {
    const char *text = "# a comment\r\n"
                       "\r\n"
                       "   version = 1   \r\n"
                       "  reason = off\r\n"
                       "\tvm   alpine   =   running   \r\n";
    hype_run_state_t r;

    CHECK(hype_run_state_parse(text, (unsigned int)strlen(text), &r) == 0, "accepted");
    CHECK(r.reason == HYPE_RUN_STATE_REASON_OFF, "reason %d", (int)r.reason);
    CHECK(r.count == 1u, "count %u", r.count);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_RUNNING, "alpine running");
}

/* An unrecognised reason word is not damage -- the reason is diagnostic and acted on by nobody, so
 * it degrades to unknown while the `vm` lines stay authoritative. */
static void test_unknown_reason_degrades(void) {
    const char *text = "version = 1\nreason = brownout\nvm alpine = stopped\n";
    hype_run_state_t r;

    CHECK(hype_run_state_parse(text, (unsigned int)strlen(text), &r) == 0, "accepted");
    CHECK(r.reason == HYPE_RUN_STATE_REASON_UNKNOWN, "reason %d", (int)r.reason);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_STOPPED, "still stopped");
}

/*
 * A TRUNCATED RECORD IS A RECORD THAT LIES. If the buffer cannot hold every line, serialize must
 * fail rather than hand back a prefix -- a prefix ending mid-`vm` claims a machine was stopped
 * when it was running, and the next boot acts on it.
 */
static void test_serialize_refuses_to_truncate(void) {
    hype_run_state_t w;
    char small[40];
    unsigned int len = 12345u;

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_REBOOT);
    hype_run_state_add(&w, "alpine", 1);
    CHECK(hype_run_state_serialize(&w, small, sizeof(small), &len) == -1, "must refuse to fit");
    CHECK(len == 12345u, "len must be left alone on failure, got %u", len);
    CHECK(hype_run_state_serialize(&w, small, 0u, &len) == -1, "zero cap");
    CHECK(hype_run_state_serialize(&w, 0, sizeof(small), &len) == -1, "NULL out");
}

/* The record is bounded by the config's own VM ceiling, and an overflow is COUNTED rather than
 * silently dropped (#341) -- a VM that vanishes without being named is the failure mode this
 * project has paid for repeatedly. */
static void test_full_record_reports_drops(void) {
    hype_run_state_t w;
    unsigned int i;
    char nm[HYPE_CFG_NAME_MAX];

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_REBOOT);
    for (i = 0; i < HYPE_RUN_STATE_MAX_VMS; i++) {
        nm[0] = 'a';
        nm[1] = (char)('0' + (int)(i % 10u));
        nm[2] = (char)('0' + (int)(i / 10u));
        nm[3] = '\0';
        CHECK(hype_run_state_add(&w, nm, 1) == 0, "add %u", i);
    }
    CHECK(w.count == HYPE_RUN_STATE_MAX_VMS, "count %u", w.count);
    CHECK(hype_run_state_add(&w, "one-too-many", 1) == -1, "the extra must be refused");
    CHECK(w.dropped == 1u, "dropped %u", w.dropped);
    CHECK(w.count == HYPE_RUN_STATE_MAX_VMS, "count must not grow past the ceiling");
}

/* A record naming more VMs than this build can hold keeps the ones it can and counts the rest,
 * for the same reason. */
static void test_parse_over_ceiling_counts_drops(void) {
    char text[64 + (HYPE_RUN_STATE_MAX_VMS + 4u) * 24u];
    unsigned int n = 0, i;
    hype_run_state_t r;

    n += (unsigned int)sprintf(text + n, "version = 1\n");
    for (i = 0; i < HYPE_RUN_STATE_MAX_VMS + 3u; i++) {
        n += (unsigned int)sprintf(text + n, "vm v%u = running\n", i);
    }
    CHECK(hype_run_state_parse(text, n, &r) == 0, "accepted");
    CHECK(r.count == HYPE_RUN_STATE_MAX_VMS, "count %u", r.count);
    CHECK(r.dropped == 3u, "dropped %u", r.dropped);
}

/*
 * A line longer than the parser's own buffer is treated as an unknown line, NOT as the prefix that
 * happened to fit. `vm someverylongname` truncated to `vm someverylong` names a DIFFERENT machine,
 * and acting on it would start the wrong guest -- the exact failure name-keying exists to avoid.
 */
static void test_overlong_line_is_not_a_prefix(void) {
    char text[4096];
    unsigned int n = 0, i;
    hype_run_state_t r;

    n += (unsigned int)sprintf(text + n, "version = 1\nvm alpine = running\nvm ");
    for (i = 0; i < 3000u; i++) {
        text[n++] = 'x';
    }
    n += (unsigned int)sprintf(text + n, " = stopped\n");
    CHECK(hype_run_state_parse(text, n, &r) == 0, "accepted");
    CHECK(r.count == 1u, "only the short line counts, got %u", r.count);
    CHECK(r.unknown_keys >= 1u, "the overlong line must be counted, got %u", r.unknown_keys);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_RUNNING, "alpine survives");
}

/* An empty name is never written: it could not be looked up again, so the line could only ever be
 * ignored. Skipping it is not a failure of the record. */
static void test_unnamed_vm_is_not_written(void) {
    hype_run_state_t w, r;
    char buf[256];
    unsigned int len = 0;

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_REBOOT);
    hype_run_state_add(&w, "", 1);
    hype_run_state_add(&w, "alpine", 1);
    CHECK(w.count == 2u, "both were added");
    CHECK(hype_run_state_serialize(&w, buf, sizeof(buf), &len) == 0, "serialize");
    CHECK(hype_run_state_parse(buf, len, &r) == 0, "parse");
    CHECK(r.count == 1u, "only the named VM round-trips, got %u", r.count);
}

/* A record with no VMs at all is legitimate: a host where every machine was stopped. It must read
 * back as usable-and-empty, which answers "stopped" for nothing and "unknown" for everything. */
static void test_no_vms_is_valid(void) {
    hype_run_state_t w, r;
    char buf[256];
    unsigned int len = 0;

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_OFF);
    CHECK(hype_run_state_serialize(&w, buf, sizeof(buf), &len) == 0, "serialize");
    CHECK(hype_run_state_parse(buf, len, &r) == 0, "accepted");
    CHECK(r.malformed == 0u, "an all-stopped host is not a damaged record");
    CHECK(r.count == 0u, "count %u", r.count);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_UNKNOWN, "unknown");
}

/* A name at the config's maximum length round-trips whole. A silently shortened name looks up as a
 * different VM. */
static void test_max_length_name_round_trips(void) {
    hype_run_state_t w, r;
    char buf[512];
    char nm[HYPE_CFG_NAME_MAX];
    unsigned int len = 0, i;

    for (i = 0; i + 1u < sizeof(nm); i++) {
        nm[i] = 'n';
    }
    nm[sizeof(nm) - 1u] = '\0';
    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_REBOOT);
    hype_run_state_add(&w, nm, 1);
    CHECK(hype_run_state_serialize(&w, buf, sizeof(buf), &len) == 0, "serialize");
    CHECK(hype_run_state_parse(buf, len, &r) == 0, "parse");
    CHECK(r.count == 1u, "count %u", r.count);
    CHECK(hype_run_state_lookup(&r, nm) == HYPE_RUN_STATE_RUNNING, "full-length name looks up");
}

/*
 * TRUNCATION MUST FAIL AT EVERY POINT, not only past the last line. Sweeping the cap from 1 to
 * one-past-fitting exercises the cut landing inside the comment, the version line, the reason line
 * and each `vm` line -- and each of those, if it silently succeeded, would produce a record that
 * says something different from what was true. The one cap that does fit must round-trip.
 */
static void test_every_truncation_point_refuses(void) {
    hype_run_state_t w, r;
    char buf[512];
    unsigned int cap, full = 0;

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_OFF);
    hype_run_state_add(&w, "alpine", 1);
    hype_run_state_add(&w, "build", 0);
    CHECK(hype_run_state_serialize(&w, buf, sizeof(buf), &full) == 0, "full serialize");

    for (cap = 1u; cap <= full + 1u; cap++) {
        char small[512];
        unsigned int len = 0;
        int rc = hype_run_state_serialize(&w, small, cap, &len);
        if (cap <= full) {
            CHECK(rc == -1, "cap %u must refuse (the record needs %u+1)", cap, full);
        } else {
            CHECK(rc == 0, "cap %u must fit", cap);
            CHECK(hype_run_state_parse(small, len, &r) == 0, "cap %u must parse", cap);
            CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_RUNNING, "alpine");
            CHECK(hype_run_state_lookup(&r, "build") == HYPE_RUN_STATE_STOPPED, "build");
        }
    }
}

/* A caller that does not want the length may pass NULL for it. */
static void test_serialize_len_may_be_null(void) {
    hype_run_state_t w;
    char buf[256];

    hype_run_state_init(&w, HYPE_RUN_STATE_REASON_REBOOT);
    hype_run_state_add(&w, "alpine", 1);
    CHECK(hype_run_state_serialize(&w, buf, sizeof(buf), 0) == 0, "NULL len is fine");
}

/*
 * A FILE WITH NO TRAILING NEWLINE. Whatever wrote it may have been interrupted, or an operator may
 * have edited it -- either way the last line is still a whole line and must be read. Dropping it
 * would silently forget one machine, which is exactly the class of quiet loss this record exists
 * to prevent.
 */
static void test_last_line_without_newline(void) {
    const char *text = "version = 1\nvm alpine = running";
    hype_run_state_t r;

    CHECK(hype_run_state_parse(text, (unsigned int)strlen(text), &r) == 0, "accepted");
    CHECK(r.count == 1u, "the unterminated last line must still count, got %u", r.count);
    CHECK(hype_run_state_lookup(&r, "alpine") == HYPE_RUN_STATE_RUNNING, "alpine running");
}

int main(void) {
    test_round_trip();
    test_unknown_is_not_stopped();
    test_reordering_does_not_move_the_answers();
    test_future_version_refuses_everything();
    test_missing_version_refuses();
    test_empty_input_refuses();
    test_unknown_state_word_refuses();
    test_damaged_lines_refuse();
    test_unknown_key_is_tolerated();
    test_whitespace_comments_and_crlf();
    test_unknown_reason_degrades();
    test_serialize_refuses_to_truncate();
    test_full_record_reports_drops();
    test_parse_over_ceiling_counts_drops();
    test_overlong_line_is_not_a_prefix();
    test_unnamed_vm_is_not_written();
    test_no_vms_is_valid();
    test_max_length_name_round_trips();
    test_every_truncation_point_refuses();
    test_serialize_len_may_be_null();
    test_last_line_without_newline();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
