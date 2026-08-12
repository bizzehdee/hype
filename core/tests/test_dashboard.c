#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dashboard.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

/* Read row `r` of the grid into a NUL-terminated string (trailing blanks kept). */
static void row_text(const hype_vt_screen_t *s, unsigned r, char *out) {
    unsigned c;
    for (c = 0; c < s->cols; c++) out[c] = (char)hype_vt_screen_cell(s, c, r).ch;
    /* trim trailing spaces for easier substring checks */
    while (c > 0 && out[c - 1] == ' ') c--;
    out[c] = '\0';
}

static int row_has(const hype_vt_screen_t *s, unsigned r, const char *needle) {
    char buf[HYPE_VT_MAX_COLS + 1];
    row_text(s, r, buf);
    return strstr(buf, needle) != NULL;
}

/* --- #263: uptime must accumulate RUNNING time, not wall-clock --- */

static void test_uptime_freezes_while_stopped(void) {
    /* The reported bug: the column kept climbing while the VM's state read `off`. */
    hype_vm_uptime_t u;
    hype_vm_uptime_reset(&u);
    hype_vm_uptime_sample(&u, 1000ULL, 1);   /* origin */
    hype_vm_uptime_sample(&u, 3000ULL, 1);   /* +2000 running */
    if (hype_vm_uptime_ms(&u) != 2000ULL) {
        printf("FAIL: running time should be 2000ms, got %llu\n", hype_vm_uptime_ms(&u));
        failures++;
    }
    hype_vm_uptime_sample(&u, 9000ULL, 0);   /* stopped for this interval: no credit */
    if (hype_vm_uptime_ms(&u) != 2000ULL) {
        printf("FAIL: uptime must FREEZE while stopped, got %llu\n", hype_vm_uptime_ms(&u));
        failures++;
    }
    hype_vm_uptime_sample(&u, 90000ULL, 0);  /* still stopped, long gap */
    if (hype_vm_uptime_ms(&u) != 2000ULL) {
        printf("FAIL: a long stopped gap must not be credited, got %llu\n", hype_vm_uptime_ms(&u));
        failures++;
    }
}

static void test_uptime_resumes_after_stop(void) {
    hype_vm_uptime_t u;
    hype_vm_uptime_reset(&u);
    hype_vm_uptime_sample(&u, 0ULL, 1);
    hype_vm_uptime_sample(&u, 500ULL, 1);    /* +500 */
    hype_vm_uptime_sample(&u, 5000ULL, 0);   /* stopped */
    hype_vm_uptime_sample(&u, 6000ULL, 1);   /* running again from here */
    hype_vm_uptime_sample(&u, 6750ULL, 1);   /* +750 */
    if (hype_vm_uptime_ms(&u) != 1250ULL) {
        printf("FAIL: resume should continue the total (expect 1250ms), got %llu\n",
               hype_vm_uptime_ms(&u));
        failures++;
    }
}

static void test_uptime_first_sample_banks_nothing(void) {
    /* The first sample only establishes the origin. Crediting now_ms here would bank
     * everything that happened before the VM existed as its uptime. */
    hype_vm_uptime_t u;
    hype_vm_uptime_reset(&u);
    hype_vm_uptime_sample(&u, 123456ULL, 1);
    if (hype_vm_uptime_ms(&u) != 0ULL) {
        printf("FAIL: the first sample must bank nothing, got %llu\n", hype_vm_uptime_ms(&u));
        failures++;
    }
}

static void test_uptime_ignores_time_going_backwards(void) {
    hype_vm_uptime_t u;
    hype_vm_uptime_reset(&u);
    hype_vm_uptime_sample(&u, 5000ULL, 1);
    hype_vm_uptime_sample(&u, 4000ULL, 1);   /* clock went backwards */
    if (hype_vm_uptime_ms(&u) != 0ULL) {
        printf("FAIL: a backwards clock must not be credited, got %llu\n", hype_vm_uptime_ms(&u));
        failures++;
    }
    hype_vm_uptime_sample(&u, 4500ULL, 1);   /* forward again from the new origin */
    if (hype_vm_uptime_ms(&u) != 500ULL) {
        printf("FAIL: should resume accumulating after a backwards step, got %llu\n",
               hype_vm_uptime_ms(&u));
        failures++;
    }
}

static void test_uptime_null_safe(void) {
    hype_vm_uptime_reset(0);
    hype_vm_uptime_sample(0, 1ULL, 1);
    if (hype_vm_uptime_ms(0) != 0ULL) {
        printf("FAIL: NULL accumulator should read 0\n");
        failures++;
    }
}

/* --- #264: CPU% must be measured, and must be able to read something other than 0/100 --- */

static void test_cpu_reports_intermediate_values(void) {
    /* The whole point: the old metric could only ever be 0 or 100. A guest busy for a
     * quarter of the window must read 25. */
    hype_vm_cpu_t c;
    hype_vm_cpu_reset(&c);
    hype_vm_cpu_sample(&c, 1000ULL, 10000ULL); /* baseline */
    hype_vm_cpu_sample(&c, 1250ULL, 11000ULL); /* busy 250 of 1000 */
    if (hype_vm_cpu_pct(&c) != 25u) {
        printf("FAIL: 250/1000 busy should read 25%%, got %u\n", hype_vm_cpu_pct(&c));
        failures++;
    }
    hype_vm_cpu_sample(&c, 1300ULL, 12000ULL); /* busy 50 of 1000 -- it must MOVE */
    if (hype_vm_cpu_pct(&c) != 5u) {
        printf("FAIL: the window should track load and read 5%%, got %u\n",
               hype_vm_cpu_pct(&c));
        failures++;
    }
}

static void test_cpu_first_sample_is_baseline_only(void) {
    /* Treating the cumulative totals as a window would report a lifetime mean, which
     * is the behaviour this replaces. */
    hype_vm_cpu_t c;
    hype_vm_cpu_reset(&c);
    hype_vm_cpu_sample(&c, 900ULL, 1000ULL);
    if (hype_vm_cpu_pct(&c) != 0u) {
        printf("FAIL: the first sample must not report a percentage, got %u\n",
               hype_vm_cpu_pct(&c));
        failures++;
    }
}

static void test_cpu_clamps_and_handles_degenerate_windows(void) {
    hype_vm_cpu_t c;
    hype_vm_cpu_reset(&c);
    hype_vm_cpu_sample(&c, 0ULL, 0ULL);
    hype_vm_cpu_sample(&c, 5000ULL, 1000ULL); /* busy > wall: jitter, must clamp */
    if (hype_vm_cpu_pct(&c) != 100u) {
        printf("FAIL: busy exceeding wall must clamp to 100, got %u\n", hype_vm_cpu_pct(&c));
        failures++;
    }
    hype_vm_cpu_sample(&c, 5500ULL, 1000ULL); /* no wall elapsed: keep last reading */
    if (hype_vm_cpu_pct(&c) != 100u) {
        printf("FAIL: a zero-length window must keep the previous reading, got %u\n",
               hype_vm_cpu_pct(&c));
        failures++;
    }
}

static void test_cpu_rebases_on_counter_going_backwards(void) {
    hype_vm_cpu_t c;
    hype_vm_cpu_reset(&c);
    hype_vm_cpu_sample(&c, 1000ULL, 10000ULL);
    hype_vm_cpu_sample(&c, 1500ULL, 11000ULL); /* 50% */
    if (hype_vm_cpu_pct(&c) != 50u) {
        printf("FAIL: expected 50%%, got %u\n", hype_vm_cpu_pct(&c));
        failures++;
    }
    hype_vm_cpu_sample(&c, 10ULL, 20ULL);      /* counters reset: rebase, do not spike */
    if (hype_vm_cpu_pct(&c) != 50u) {
        printf("FAIL: a backwards counter must keep the last reading, got %u\n",
               hype_vm_cpu_pct(&c));
        failures++;
    }
    hype_vm_cpu_sample(&c, 110ULL, 220ULL);    /* 100 busy of 200 wall */
    if (hype_vm_cpu_pct(&c) != 50u) {
        printf("FAIL: should resume measuring after a rebase, got %u\n",
               hype_vm_cpu_pct(&c));
        failures++;
    }
}

static void test_cpu_null_safe(void) {
    hype_vm_cpu_reset(0);
    hype_vm_cpu_sample(0, 1ULL, 1ULL);
    if (hype_vm_cpu_pct(0) != 0u) {
        printf("FAIL: NULL accumulator should read 0\n");
        failures++;
    }
}

static void test_focus_skips_undispatched_vms(void) {
    const unsigned char avail[2] = {1u, 0u}; /* vm0 ready; vm1 never dispatched */
    CHECK("next from dashboard reaches vm0",
          hype_term_focus_apply(-1, HYPE_TERM_FOCUS_NEXT, 0u, avail, 2u) == 0);
    CHECK("next from vm0 returns to dashboard, skipping vm1",
          hype_term_focus_apply(0, HYPE_TERM_FOCUS_NEXT, 0u, avail, 2u) == -1);
    CHECK("previous from dashboard reaches vm0, skipping vm1",
          hype_term_focus_apply(-1, HYPE_TERM_FOCUS_PREV, 0u, avail, 2u) == 0);
    CHECK("direct jump to unavailable vm1 is refused",
          hype_term_focus_apply(0, HYPE_TERM_FOCUS_JUMP, 1u, avail, 2u) == 0);
    CHECK("invalid current view is forced to dashboard",
          hype_term_focus_validate(1, avail, 2u) == -1);
}

static void test_focus_handles_no_available_vms(void) {
    const unsigned char none[2] = {0u, 0u};
    CHECK("toggle with no VM stays on dashboard",
          hype_term_focus_apply(-1, HYPE_TERM_FOCUS_TOGGLE_DASHBOARD, 0u, none, 2u) == -1);
    CHECK("jump with no VM stays on dashboard",
          hype_term_focus_apply(-1, HYPE_TERM_FOCUS_JUMP, 0u, none, 2u) == -1);
    CHECK("negative view validates as dashboard",
          hype_term_focus_validate(-5, none, 2u) == -1);
}

static void test_focus_preserves_normal_two_vm_cycle(void) {
    const unsigned char avail[2] = {1u, 1u};
    CHECK("dashboard next vm0",
          hype_term_focus_apply(-1, HYPE_TERM_FOCUS_NEXT, 0u, avail, 2u) == 0);
    CHECK("vm0 next vm1",
          hype_term_focus_apply(0, HYPE_TERM_FOCUS_NEXT, 0u, avail, 2u) == 1);
    CHECK("vm1 next dashboard",
          hype_term_focus_apply(1, HYPE_TERM_FOCUS_NEXT, 0u, avail, 2u) == -1);
    CHECK("dashboard previous vm1",
          hype_term_focus_apply(-1, HYPE_TERM_FOCUS_PREV, 0u, avail, 2u) == 1);
    CHECK("vm1 previous vm0",
          hype_term_focus_apply(1, HYPE_TERM_FOCUS_PREV, 0u, avail, 2u) == 0);
    CHECK("vm0 previous dashboard",
          hype_term_focus_apply(0, HYPE_TERM_FOCUS_PREV, 0u, avail, 2u) == -1);
    CHECK("toggle dashboard enters first VM",
          hype_term_focus_apply(-1, HYPE_TERM_FOCUS_TOGGLE_DASHBOARD, 0u, avail, 2u) == 0);
    CHECK("toggle VM returns dashboard",
          hype_term_focus_apply(1, HYPE_TERM_FOCUS_TOGGLE_DASHBOARD, 0u, avail, 2u) == -1);
    CHECK("none keeps current",
          hype_term_focus_apply(1, HYPE_TERM_FOCUS_NONE, 0u, avail, 2u) == 1);
}

int main(void) {
    /* --- uptime formatting --- */
    char up[16];
    hype_dashboard_fmt_uptime(up, 0);       CHECK("uptime 0", strcmp(up, "00:00:00") == 0);
    hype_dashboard_fmt_uptime(up, 754);     CHECK("uptime 12:34", strcmp(up, "00:12:34") == 0);
    hype_dashboard_fmt_uptime(up, 3661);    CHECK("uptime 1:01:01", strcmp(up, "01:01:01") == 0);
    hype_dashboard_fmt_uptime(up, 45296);   CHECK("uptime 12:34:56", strcmp(up, "12:34:56") == 0);

    /* --- full render --- */
    hype_vt_screen_t *s = malloc(sizeof(*s));
    hype_vt_screen_init(s, 80, 25);

    hype_vm_dash_info_t vms[2] = {
        { "alpine", "linux", "running", 3, 6144, 754, "test.iso", 1 },
        { "fedora", "linux", "off",     0, 6144, 0,   0,          0 },
    };
    hype_dashboard_render(s, vms, 2, 45296, "sto", "vm0: paused");

    CHECK("header line names product", row_has(s, 0, "hype - VM dashboard"));
    CHECK("header shows host uptime", row_has(s, 0, "12:34:56"));
    CHECK("column header present", row_has(s, 2, "NAME") && row_has(s, 2, "STATE") && row_has(s, 2, "MEDIA"));

    /* row 3 = vm1 (alpine, focused) */
    CHECK("vm1 focused marker", row_has(s, 3, ">1"));
    CHECK("vm1 name", row_has(s, 3, "alpine"));
    CHECK("vm1 os", row_has(s, 3, "linux"));
    CHECK("vm1 state", row_has(s, 3, "running"));
    CHECK("vm1 cpu", row_has(s, 3, "3%"));
    CHECK("vm1 mem", row_has(s, 3, "6144M"));
    CHECK("vm1 uptime", row_has(s, 3, "00:12:34"));
    CHECK("vm1 media", row_has(s, 3, "test.iso"));

    /* row 4 = vm2 (fedora, off, no media) */
    CHECK("vm2 name", row_has(s, 4, "fedora"));
    CHECK("vm2 state off", row_has(s, 4, "off"));
    CHECK("vm2 no focus marker (starts with plain '2')", !row_has(s, 4, ">2"));
    CHECK("vm2 media dash", row_has(s, 4, "-"));

    /* --- long name is truncated to the column, no overflow crash --- */
    hype_vt_screen_init(s, 80, 25);
    hype_vm_dash_info_t big[1] = {
        { "a-really-long-vm-name-exceeding-the-column", "linux", "running", 100, 65536, 99999, "some-long-media-name.iso", 0 },
    };
    hype_dashboard_render(s, big, 1, 0, NULL, NULL);
    CHECK("long name truncated (col header intact on row2)", row_has(s, 2, "NAME"));
    CHECK("long-name row rendered something", row_has(s, 3, "a-really-long"));

    /* --- NULL identity fields fall back to placeholders (no crash) --- */
    hype_vt_screen_init(s, 80, 25);
    {
        hype_vm_dash_info_t nullish[1] = { { NULL, NULL, NULL, 0, 512, 0, NULL, 0 } };
        hype_dashboard_render(s, nullish, 1, 0, NULL, NULL);
        CHECK("null name -> '?'", row_has(s, 3, "?"));
        CHECK("null media -> '-'", row_has(s, 3, "-"));
    }

    /* --- zero VMs: header still renders, no crash --- */
    hype_vt_screen_init(s, 80, 25);
    hype_dashboard_render(s, NULL, 0, 10, "", "");
    CHECK("empty dashboard header", row_has(s, 0, "hype - VM dashboard"));

    free(s);
    test_uptime_freezes_while_stopped();
    test_uptime_resumes_after_stop();
    test_uptime_first_sample_banks_nothing();
    test_uptime_ignores_time_going_backwards();
    test_uptime_null_safe();

    test_cpu_reports_intermediate_values();
    test_cpu_first_sample_is_baseline_only();
    test_cpu_clamps_and_handles_degenerate_windows();
    test_cpu_rebases_on_counter_going_backwards();
    test_cpu_null_safe();
    test_focus_skips_undispatched_vms();
    test_focus_handles_no_available_vms();
    test_focus_preserves_normal_two_vm_cycle();

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
