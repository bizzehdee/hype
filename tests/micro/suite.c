/*
 * #554: the suite kernel -- one VM, several microtests, selected by the command line.
 *
 *   kernel  = \EFI\hype\micro\suite.bin
 *   cmdline = tests=ram1,cpumsr,fwcfg
 *
 * WHY THIS EXISTS, and why it is not merely a convenience. Admission grants one WHOLE PHYSICAL CORE
 * per VM with the BSP's core reserved, so a host runs at most (physical cores - 1) VMs however much
 * RAM it has. The bare-metal AMD laptop is four cores: three VMs, ever. A stick carrying two 2-vCPU
 * Alpine guests has exactly ONE VM left, so one-VM-per-test cannot run more than a single test per
 * boot there. This is what makes a bare-metal sweep possible at all.
 *
 * HOW A SUITE MEMBER IS THE SAME CODE AS A STANDALONE ARTIFACT. Each test is compiled twice from
 * one unedited source: once as its own artifact, and once with -DMICRO_SUITE and
 * -Dmicro_main=micro_test_<name>. Every test already ends by calling micro_halt() -- including on
 * every failure path -- and under MICRO_SUITE that longjmps back here instead of halting. So no test
 * was modified to become suite-capable, and neither build can drift from the other.
 *
 * WHAT IS LOST, stated plainly. Tests stop being isolated from each other: they share one guest's
 * RAM, its devices and its descriptor tables, and a test that faults takes the whole VM down with
 * every test after it. So the standalone one-VM-per-test mode remains the AUTHORITATIVE run under
 * QEMU, and this is the sweep. Three things below exist because of that:
 *
 *   - progress is printed BEFORE each test, so a fault names the test that was running;
 *   - devices are put back to a known state between tests;
 *   - a summary line at the end distinguishes a finished suite from a truncated one.
 */
#include "micro.h"
#include "micro_idt.h"

#define NAME "suite"

micro_jmp_buf g_micro_suite_env;

/* Each test's entry, renamed at compile time from micro_main. */
void micro_test_hello(uint64_t zero_page_gpa);
void micro_test_ram1(uint64_t zero_page_gpa);
void micro_test_cpumsr(uint64_t zero_page_gpa);
void micro_test_fwcfg(uint64_t zero_page_gpa);
void micro_test_intdeliver(uint64_t zero_page_gpa);
void micro_test_pausespin(uint64_t zero_page_gpa);
void micro_test_pci(uint64_t zero_page_gpa);
void micro_test_pflash(uint64_t zero_page_gpa);
void micro_test_ps2(uint64_t zero_page_gpa);

typedef void (*micro_entry_t)(uint64_t);

typedef struct {
    const char *name;
    micro_entry_t fn;
    /*
     * #554 item 4: a test that cannot share a guest says so, and is SKIPPED rather than run. A skip
     * is not a pass -- the summary counts it separately and the harness must too. Nothing is marked
     * hostile today: ram1 writes [1 MB,16 MB) and [20 MB,RAM), which misses the stack at 0x80000,
     * the descriptor tables at 0x90000 and this kernel's own image, so even the greediest member is
     * safe. The flag exists so the first test that ISN'T can be excluded without inventing a
     * mechanism under pressure.
     */
    int suite_hostile;
} micro_test_t;

static const micro_test_t TESTS[] = {
    {"hello", micro_test_hello, 0},
    {"ram1", micro_test_ram1, 0},
    {"cpumsr", micro_test_cpumsr, 0},
    {"fwcfg", micro_test_fwcfg, 0},
    {"pci", micro_test_pci, 0},
    {"pflash", micro_test_pflash, 0},
    {"intdeliver", micro_test_intdeliver, 0},
    {"pausespin", micro_test_pausespin, 0},
    {"ps2", micro_test_ps2, 0},
};
#define NTESTS (sizeof(TESTS) / sizeof(TESTS[0]))

/*
 * Put the shared devices back between tests. intdeliver, pausespin and ps2 all leave the 8259
 * remapped, the 8254 running, lines unmasked and interrupts enabled; anything after them would
 * inherit that. Masking rather than trying to restore a "pristine" state, because pristine is not
 * something a guest can reconstruct -- and every test that needs these devices programs them itself.
 */
static void quiesce_devices(void) {
    micro_cli();
    micro_outb(MICRO_PIC_MASTER_DATA, 0xFFu);
    micro_outb(MICRO_PIC_SLAVE_DATA, 0xFFu);
    /* Channel 0 to one-shot with a zero reload: it stops asserting rather than free-running. */
    micro_outb(MICRO_PIT_CMD, 0x30u);
    micro_outb(MICRO_PIT_CH0_DATA, 0x00u);
    micro_outb(MICRO_PIT_CH0_DATA, 0x00u);
}

static int name_eq(const char *a, const char *b, unsigned blen) {
    unsigned i = 0;
    while (i < blen && a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return (i == blen && a[i] == '\0') ? 1 : 0;
}

static const micro_test_t *find_test(const char *n, unsigned len) {
    unsigned i;
    for (i = 0; i < NTESTS; i++) {
        if (name_eq(TESTS[i].name, n, len)) {
            return &TESTS[i];
        }
    }
    return 0;
}

static unsigned g_ran, g_passed, g_failed, g_noverdict, g_skipped, g_unknown;

static void run_one(const micro_test_t *t, uint64_t zero_page_gpa) {
    /* BEFORE, not after. A test that triple-faults prints no verdict and takes the VM with it, so
     * this line is the only thing that will say which one was running. */
    micro_puts("\nMICRO RUN: ");
    micro_puts(t->name);
    micro_puts("\n");

    if (t->suite_hostile) {
        micro_puts("MICRO SKIP: ");
        micro_puts(t->name);
        micro_puts(" -- runs isolated only; a suite cannot give it a clean guest\n");
        g_skipped++;
        return;
    }

    g_micro_verdict = 0;
    g_ran++;
    if (micro_setjmp(g_micro_suite_env) == 0) {
        t->fn(zero_page_gpa);
        /* A test that RETURNS rather than calling micro_halt() is not an error, just unusual. */
    }
    quiesce_devices();

    if (g_micro_verdict == 1) {
        g_passed++;
    } else if (g_micro_verdict == 2) {
        g_failed++;
    } else {
        /* Absence of a verdict is a failure in its own right, and is counted separately so it can be
         * told apart from a test that decided it had failed. */
        micro_puts("MICRO FAIL: ");
        micro_puts(t->name);
        micro_puts(": returned without printing a verdict\n");
        g_noverdict++;
    }
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cl = micro_cmdline(zero_page_gpa);
    const char *list = (cl != 0) ? micro_cmdline_value(cl, "tests") : 0;

    micro_puts("\nmicro/" NAME ": suite kernel, ");
    micro_put_uint(NTESTS);
    micro_puts(" test(s) available\n");

    if (list == 0) {
        micro_puts("micro/" NAME ": no tests= on the command line, so running ALL of them. Name a "
                   "comma-separated subset to narrow it.\n");
    } else {
        micro_puts("micro/" NAME ": requested '");
        micro_puts(list);
        micro_puts("'\n");
    }

    if (list == 0) {
        unsigned i;
        for (i = 0; i < NTESTS; i++) {
            run_one(&TESTS[i], zero_page_gpa);
        }
    } else {
        const char *p = list;
        while (*p != '\0' && *p != ' ') {
            const char *start = p;
            unsigned len = 0;
            const micro_test_t *t;

            while (p[len] != '\0' && p[len] != ',' && p[len] != ' ') {
                len++;
            }
            p = start + len;
            if (*p == ',') {
                p++;
            }
            if (len == 0) {
                continue;
            }
            t = find_test(start, len);
            if (t == 0) {
                /* Naming a test that does not exist is a FAILURE of the run, not a silent no-op: a
                 * typo in a config would otherwise look like a suite that simply ran less. */
                micro_puts("MICRO FAIL: suite: unknown test name '");
                {
                    unsigned k;
                    for (k = 0; k < len; k++) {
                        micro_putc(start[k]);
                    }
                }
                micro_puts("' -- check the tests= list against the table in suite.c\n");
                g_unknown++;
                continue;
            }
            run_one(t, zero_page_gpa);
        }
    }

    /*
     * The summary. Its absence is how a truncated run is recognised -- a suite that died halfway
     * otherwise looks exactly like a suite that had fewer tests in it.
     */
    micro_puts("\nMICRO SUITE: ran=");
    micro_put_uint(g_ran);
    micro_puts(" passed=");
    micro_put_uint(g_passed);
    micro_puts(" failed=");
    micro_put_uint(g_failed);
    micro_puts(" noverdict=");
    micro_put_uint(g_noverdict);
    micro_puts(" skipped=");
    micro_put_uint(g_skipped);
    micro_puts(" unknown=");
    micro_put_uint(g_unknown);
    micro_puts("\n");

    if (g_failed == 0u && g_noverdict == 0u && g_unknown == 0u) {
        micro_pass(NAME);
    } else {
        micro_fail(NAME, "one or more members failed, printed no verdict, or was not recognised");
    }

    /* Not micro_halt(): that longjmps under MICRO_SUITE and there is nowhere left to jump to. */
    for (;;) {
        __asm__ volatile("hlt");
    }
}
