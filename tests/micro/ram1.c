/*
 * #536: RAM-1/RAM-2, ported out of boot/main.c.
 *
 * What the in-binary version validated: that hype's dynamically-computed NPT and guest-CR3
 * coverage genuinely reached wherever AllocatePages() put the guest's RAM, and reached the guest's
 * own page tables too -- a map that covers a guest's RAM is useless if it does not also cover the
 * tables describing that RAM (#206). Its guest payload was three bytes, `hlt; jmp $-3`, and its
 * whole assertion was host-side: the guest halted cleanly, therefore the first instruction fetch
 * had been translatable.
 *
 * That assertion is weak, and only the host could make it. A guest can do far better, and this is
 * the argument for the whole port: the question "is all of my RAM really there" is one the guest is
 * in the best position to answer, and the host was answering a much easier one instead.
 *
 * So this checks three things the original could not:
 *
 *  1. The e820 the guest is handed matches the mem_mb the operator configured. That is the
 *     config-to-guest link, and nothing in the old test touched it.
 *  2. Every page of that RAM is writable and reads back what was written.
 *  3. No two pages ALIAS. This is why there are two separate passes over the whole region --
 *     write everything first, then verify everything. A write-then-read-per-page loop passes
 *     perfectly if every guest page maps to one single host page, which is precisely the nested-
 *     paging defect worth catching. One pass cannot see it; two can.
 *
 * The pattern is derived from each page's own address, so a page that reads back another page's
 * contents reports both the address it is and the address it got.
 */
#include "micro.h"

#define NAME "ram1"

/* Must match core/kboot.h. A guest cannot include a hype header, so the values are restated --
 * and the checks below are what makes a divergence show up as a FAIL rather than as corruption. */
#define KBOOT_LOAD_GPA 0x1000000ull      /* the payload lives here */
#define KBOOT_STACK_TOP_GPA 0x80000ull   /* and the stack grows down from here */

#define PAGE 4096ull
#define PATTERN_KEY 0x5A5A5A5A5A5A5A5Aull

/*
 * The two ranges this test owns. Everything else in the low 16 MB belongs to something: the guest
 * page tables (0x1000-0x6FFF), the zero page (0x7000), the stack (below 0x80000), and the payload
 * itself (from 0x1000000). Writing to those would break the guest rather than test it.
 */
#define LOW_START 0x100000ull            /* 1 MB -- above the stack, below the payload */
#define LOW_END KBOOT_LOAD_GPA           /* 16 MB */
#define HIGH_START 0x1400000ull          /* 20 MB -- clear of the payload with room to spare */

static inline uint64_t pattern_for(uint64_t gpa) { return gpa ^ PATTERN_KEY; }

/* Two words per page, at both ends, so a partial or misaligned mapping shows up as well as an
 * absent one. */
static inline void page_write(uint64_t gpa) {
    *(volatile uint64_t *)(uintptr_t)gpa = pattern_for(gpa);
    *(volatile uint64_t *)(uintptr_t)(gpa + PAGE - 8ull) = pattern_for(gpa + PAGE - 8ull);
}

static inline int page_verify(uint64_t gpa) {
    uint64_t a = *(volatile uint64_t *)(uintptr_t)gpa;
    uint64_t b = *(volatile uint64_t *)(uintptr_t)(gpa + PAGE - 8ull);
    return (a == pattern_for(gpa) && b == pattern_for(gpa + PAGE - 8ull)) ? 1 : 0;
}

static void report_mismatch(uint64_t gpa) {
    uint64_t got = *(volatile uint64_t *)(uintptr_t)gpa;
    uint64_t want = pattern_for(gpa);

    micro_puts("micro/" NAME ": page ");
    micro_put_hex(gpa);
    micro_puts(" wanted ");
    micro_put_hex(want);
    micro_puts(" got ");
    micro_put_hex(got);
    /* The pattern is address-derived, so a page that returned another page's contents names that
     * page -- which is what tells an alias apart from plain corruption. */
    micro_puts(" (which is the pattern for ");
    micro_put_hex(got ^ PATTERN_KEY);
    micro_puts(")\n");
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const unsigned char *zp = (const unsigned char *)(uintptr_t)zero_page_gpa;
    uint64_t ram_bytes, gpa, pages = 0ull;
    unsigned int e820_entries;

    micro_puts("\n");

    if (zero_page_gpa == 0ull) {
        micro_fail(NAME, "RSI was zero -- no zero page was passed");
        micro_halt();
    }
    e820_entries = zp[0x1E8];
    if (e820_entries == 0u) {
        micro_fail(NAME, "the zero page carries no e820 entries");
        micro_halt();
    }
    /* First e820 entry: {addr @ 0x2D0, size @ 0x2D8, type @ 0x2E0}. */
    if (*(const uint64_t *)(uintptr_t)(zero_page_gpa + 0x2D0) != 0ull) {
        micro_fail(NAME, "the first e820 entry does not start at guest-physical 0");
        micro_halt();
    }
    ram_bytes = *(const uint64_t *)(uintptr_t)(zero_page_gpa + 0x2D8);

    micro_puts("micro/" NAME ": e820 says ");
    micro_put_uint(ram_bytes / (1024ull * 1024ull));
    micro_puts(" MiB of RAM from gpa 0\n");

    if (ram_bytes <= HIGH_START + PAGE) {
        micro_fail(NAME, "too little RAM to test -- give this VM at least 32 mem_mb");
        micro_halt();
    }

    /*
     * Pass 1: write every page in both ranges. Nothing is verified yet, deliberately -- see the
     * aliasing argument in this file's header.
     */
    for (gpa = LOW_START; gpa + PAGE <= LOW_END; gpa += PAGE) {
        page_write(gpa);
        pages++;
    }
    for (gpa = HIGH_START; gpa + PAGE <= ram_bytes; gpa += PAGE) {
        page_write(gpa);
        pages++;
    }

    micro_puts("micro/" NAME ": wrote ");
    micro_put_uint(pages);
    micro_puts(" pages, now verifying every one of them\n");

    /* Pass 2: verify every page, in the same order. A first failure is reported with both
     * addresses and stops -- a wall of mismatches after the first one says nothing extra. */
    for (gpa = LOW_START; gpa + PAGE <= LOW_END; gpa += PAGE) {
        if (!page_verify(gpa)) {
            report_mismatch(gpa);
            micro_fail(NAME, "a page below the payload did not read back what was written");
            micro_halt();
        }
    }
    for (gpa = HIGH_START; gpa + PAGE <= ram_bytes; gpa += PAGE) {
        if (!page_verify(gpa)) {
            report_mismatch(gpa);
            micro_fail(NAME, "a page above the payload did not read back what was written");
            micro_halt();
        }
    }

    /*
     * The last page of reported RAM specifically. It is called out separately from the loop above
     * because an off-by-one in a nested map -- one 2 MB PDE short of the configured RAM -- is
     * exactly the shape of defect the original test could not see, and the tail is where it lives.
     */
    {
        uint64_t last = (ram_bytes - PAGE) & ~(PAGE - 1ull);
        if (last >= HIGH_START && !page_verify(last)) {
            report_mismatch(last);
            micro_fail(NAME, "the LAST page of reported RAM is not usable");
            micro_halt();
        }
    }

    micro_puts("micro/" NAME ": ");
    micro_put_uint(pages);
    micro_puts(" pages written and verified, no aliasing, tail page usable\n");
    micro_pass(NAME);
    micro_halt();
}
