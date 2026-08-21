#include <stdio.h>
#include <string.h>
#include "../kboot.h"
#include "../linux_boot.h"
#include "../../arch/x86_64/cpu/paging.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        if ((long long)(expected) != (long long)(actual)) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected), \
                   (long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* A minimal bzImage head: a valid setup header with setup_sects = 4, so the payload begins at
 * (4+1)*512 = 2560. */
static unsigned char g_head[HYPE_KBOOT_HEAD_BYTES];

static hype_linux_setup_header_t *head_hdr(void) {
    return (hype_linux_setup_header_t *)(g_head + HYPE_LINUX_SETUP_HEADER_OFFSET);
}

static void make_valid_head(void) {
    hype_linux_setup_header_t *h;
    memset(g_head, 0, sizeof(g_head));
    h = head_hdr();
    h->setup_sects = 4;
    h->boot_flag = HYPE_LINUX_BOOT_FLAG;
    h->header = HYPE_LINUX_HDR_MAGIC;
    h->version = 0x020Fu;
    h->xloadflags = HYPE_LINUX_XLF_KERNEL_64;
}

static void test_plan_accepts_a_valid_image(void) {
    hype_kboot_plan_t p;
    hype_kboot_status_t st;
    uint64_t image = 2560ull + 0x40000ull; /* 256 KB of payload */

    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), image, 512ull * 1024ull * 1024ull, 0u, 0, 0ull, &p);

    CHECK_INT("a valid bzImage plans", HYPE_KBOOT_OK, st);
    CHECK_INT("payload starts after the setup region", 2560, p.payload_file_offset);
    CHECK_HEX("payload size is the rest of the file", 0x40000ull, p.payload_bytes);
    CHECK_HEX("payload loads at 16MB", HYPE_KBOOT_LOAD_GPA, p.payload_load_gpa);
    /* The 64-bit entry rule: load address + 0x200. */
    CHECK_HEX("entry is load+0x200", HYPE_KBOOT_LOAD_GPA + 0x200ull, p.entry_gpa);
    CHECK_HEX("cr3 is the guest PML4", HYPE_KBOOT_PML4_GPA, p.cr3_gpa);
    CHECK_HEX("rsp is the stack top", HYPE_KBOOT_STACK_TOP_GPA, p.rsp_gpa);
    CHECK_HEX("rsi will hold the zero page", HYPE_KBOOT_ZERO_PAGE_GPA, p.zero_page_gpa);
    CHECK_INT("4 GB identity-mapped", HYPE_KBOOT_PD_PAGES, p.gb_to_map);
}

/* setup_sects = 0 means 4, per the documented convention -- the payload offset must not be 512. */
static void test_setup_sects_zero_means_four(void) {
    hype_kboot_plan_t p;
    make_valid_head();
    head_hdr()->setup_sects = 0;
    CHECK_INT("plans", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), 4096ull * 64ull, 512ull * 1024ull * 1024ull, 0u, 0, 0ull, &p));
    CHECK_INT("setup_sects 0 is treated as 4", 2560, p.payload_file_offset);
}

static void test_rejections(void) {
    hype_kboot_plan_t p;
    hype_kboot_status_t st;

    make_valid_head();
    head_hdr()->boot_flag = 0;
    st = hype_kboot_plan(g_head, sizeof(g_head), 1ull << 20, 1ull << 30, 0u, 0, 0ull, &p);
    CHECK_INT("no 0xAA55 boot flag is refused", HYPE_KBOOT_ERR_BAD_HEADER, st);

    make_valid_head();
    head_hdr()->xloadflags = 0;
    st = hype_kboot_plan(g_head, sizeof(g_head), 1ull << 20, 1ull << 30, 0u, 0, 0ull, &p);
    CHECK_INT("a 32-bit-only kernel is refused, not degraded", HYPE_KBOOT_ERR_BAD_HEADER, st);

    make_valid_head();
    st = hype_kboot_plan(g_head, HYPE_LINUX_SETUP_HEADER_OFFSET, 1ull << 20, 1ull << 30, 0u, 0, 0ull, &p);
    CHECK_INT("too few head bytes to decide", HYPE_KBOOT_ERR_HEAD_TOO_SMALL, st);

    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 16ull, 1ull << 30, 0u, 0, 0ull, &p);
    CHECK_INT("a file too small to hold a header", HYPE_KBOOT_ERR_SHORT_IMAGE, st);

    /* Header valid, but the file ends exactly where the payload would begin. */
    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 2560ull, 1ull << 30, 0u, 0, 0ull, &p);
    CHECK_INT("no payload after the setup region", HYPE_KBOOT_ERR_NO_PAYLOAD, st);

    /* A 64 MB payload cannot load at 16 MB inside 32 MB of guest RAM. */
    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 2560ull + (64ull << 20), 32ull << 20, 0u, 0, 0ull, &p);
    CHECK_INT("a payload larger than the VM's RAM is refused", HYPE_KBOOT_ERR_RAM_TOO_SMALL, st);

    st = hype_kboot_plan(0, sizeof(g_head), 1ull << 20, 1ull << 30, 0u, 0, 0ull, &p);
    CHECK_INT("a null image head is refused", HYPE_KBOOT_ERR_BAD_HEADER, st);
    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 1ull << 20, 1ull << 30, 0u, 0, 0ull, 0);
    CHECK_INT("a null plan output is refused", HYPE_KBOOT_ERR_BAD_HEADER, st);
}

/* The RAM floor must be reported as a usable number, not just as "too small". */
static void test_min_ram_is_the_payload_plus_the_layout(void) {
    CHECK_HEX("min RAM covers load address, payload and headroom",
              HYPE_KBOOT_LOAD_GPA + (1ull << 20) + (2ull << 20),
              hype_kboot_min_ram_bytes(1ull << 20, 0ull, 0ull));

    /* A payload that fits exactly at the reported floor must then plan. */
    {
        hype_kboot_plan_t p;
        uint64_t payload = 1ull << 20;
        make_valid_head();
        CHECK_INT("exactly the reported floor is enough", HYPE_KBOOT_OK,
                  hype_kboot_plan(g_head, sizeof(g_head), 2560ull + payload,
                                  hype_kboot_min_ram_bytes(payload, 0ull, 0ull), 0u, 0, 0ull, &p));
        CHECK_INT("one byte under the floor is not", HYPE_KBOOT_ERR_RAM_TOO_SMALL,
                  hype_kboot_plan(g_head, sizeof(g_head), 2560ull + payload,
                                  hype_kboot_min_ram_bytes(payload, 0ull, 0ull) - 1ull, 0u, 0, 0ull, &p));
    }
}

/*
 * #546: the command line. The distinction that matters is "no command line at all" (cmd_line_ptr
 * must be 0, which is what a kernel reads as absent) versus "an empty one" (a valid pointer to a
 * NUL) -- both legitimate, not the same thing.
 */
static void test_cmdline_placement(void) {
    hype_kboot_plan_t p;
    uint64_t image = 2560ull + 0x40000ull;
    uint64_t ram = 512ull * 1024ull * 1024ull;

    make_valid_head();
    CHECK_INT("no cmdline plans", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 0u, 0, 0ull, &p));
    CHECK_HEX("and cmd_line_ptr must be 0, i.e. absent", 0ull, p.cmdline_gpa);

    CHECK_INT("an EMPTY cmdline plans", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 0u, 1, 0ull, &p));
    CHECK_HEX("and gets a real address, so the kernel sees an empty string not none",
              HYPE_KBOOT_CMDLINE_GPA, p.cmdline_gpa);

    CHECK_INT("a normal cmdline plans", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 40u, 1, 0ull, &p));
    CHECK_HEX("placed after the zero page", HYPE_KBOOT_CMDLINE_GPA, p.cmdline_gpa);

    /* Exactly the layout's limit fits; one more does not. */
    CHECK_INT("the longest cmdline the page holds fits", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, HYPE_KBOOT_CMDLINE_MAX, 1, 0ull, &p));
    CHECK_INT("one byte longer is REFUSED, not truncated", HYPE_KBOOT_ERR_CMDLINE_TOO_LONG,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, HYPE_KBOOT_CMDLINE_MAX + 1u, 1, 0ull,
                              &p));

    /* The kernel's OWN stated limit binds too, and is the tighter one here. */
    make_valid_head();
    head_hdr()->cmdline_size = 32u;
    CHECK_INT("within the kernel's stated cmdline_size", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 32u, 1, 0ull, &p));
    CHECK_INT("beyond it is refused even though the page would hold it",
              HYPE_KBOOT_ERR_CMDLINE_TOO_LONG,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 33u, 1, 0ull, &p));

    /* cmdline_size 0 means the image did not state one, so only the layout binds. */
    make_valid_head();
    head_hdr()->cmdline_size = 0u;
    CHECK_INT("cmdline_size 0 means unstated, not zero-length", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 200u, 1, 0ull, &p));

    /* The command-line page must not collide with the zero page below it or the stack above it. */
    CHECK_INT("cmdline starts after the zero page", 1,
              (HYPE_KBOOT_ZERO_PAGE_GPA + 4096ull <= HYPE_KBOOT_CMDLINE_GPA) ? 1 : 0);
    CHECK_INT("and ends below the stack top", 1,
              (HYPE_KBOOT_CMDLINE_GPA + 4096ull <= HYPE_KBOOT_STACK_TOP_GPA) ? 1 : 0);
}

static void test_status_strings(void) {
    hype_kboot_status_t all[] = {HYPE_KBOOT_OK,           HYPE_KBOOT_ERR_SHORT_IMAGE,
                                HYPE_KBOOT_ERR_BAD_HEADER, HYPE_KBOOT_ERR_NO_PAYLOAD,
                                HYPE_KBOOT_ERR_RAM_TOO_SMALL, HYPE_KBOOT_ERR_HEAD_TOO_SMALL,
                                HYPE_KBOOT_ERR_CMDLINE_TOO_LONG};
    unsigned i;
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        const char *s = hype_kboot_status_str(all[i]);
        if (s == 0 || s[0] == '\0' || strcmp(s, "unknown") == 0) {
            printf("FAIL: status %d has no message\n", (int)all[i]);
            failures++;
        }
    }
    CHECK_INT("an out-of-domain status still returns a string", 0,
              strcmp(hype_kboot_status_str((hype_kboot_status_t)99), "unknown"));
}

/*
 * #535: the guest's page tables live in guest RAM, so the entries must name guest-physical
 * addresses, not the host pointers hype writes them through. That is the whole difference from
 * hype_paging_build_identity(), and getting it wrong hands the guest a CR3 into host RAM.
 */
static unsigned char g_ram[HYPE_KBOOT_LOAD_GPA];

static void test_guest_page_tables_name_guest_addresses(void) {
    hype_pte_t *pml4, *pdpt, *pd;
    unsigned int gb;

    memset(g_ram, 0xAA, sizeof(g_ram));
    hype_paging_build_identity_at(g_ram, HYPE_KBOOT_PML4_GPA, HYPE_KBOOT_PDPT_GPA,
                                  HYPE_KBOOT_PD0_GPA, HYPE_KBOOT_PD_PAGES);

    pml4 = (hype_pte_t *)(g_ram + HYPE_KBOOT_PML4_GPA);
    pdpt = (hype_pte_t *)(g_ram + HYPE_KBOOT_PDPT_GPA);

    CHECK_HEX("pml4[0] names the PDPT's GUEST address", HYPE_KBOOT_PDPT_GPA,
              pml4[0] & 0x000FFFFFFFFFF000ull);
    CHECK_INT("pml4[0] is present and writable", 3, (int)(pml4[0] & 3ull));
    CHECK_HEX("nothing above pml4[0] is present", 0ull, pml4[1]);

    for (gb = 0; gb < HYPE_KBOOT_PD_PAGES; gb++) {
        uint64_t want = HYPE_KBOOT_PD0_GPA + (uint64_t)gb * 4096ull;
        CHECK_HEX("pdpt entry names its PD's GUEST address", want,
                  pdpt[gb] & 0x000FFFFFFFFFF000ull);
        pd = (hype_pte_t *)(g_ram + want);
        CHECK_HEX("first 2MB page of this GB is identity",
                  (uint64_t)gb * HYPE_PAGING_1GB, pd[0] & 0x000FFFFFFFFFF000ull);
        CHECK_HEX("last 2MB page of this GB is identity",
                  (uint64_t)gb * HYPE_PAGING_1GB + 511ull * HYPE_PAGING_2MB,
                  pd[511] & 0x000FFFFFFFFFF000ull);
        CHECK_INT("PS is set (2MB pages)", 1, (pd[0] & HYPE_PAGING_PS) ? 1 : 0);
    }
    CHECK_HEX("the GB beyond the map is not present", 0ull, pdpt[HYPE_KBOOT_PD_PAGES]);

    /* The identity map must cover every guest MMIO address the device model uses. */
    {
        uint64_t mmio[] = {0xFEE00000ull, 0xFEC00000ull, 0xE0000000ull, 0xFFFF0000ull};
        unsigned i;
        for (i = 0; i < sizeof(mmio) / sizeof(mmio[0]); i++) {
            unsigned int g = (unsigned int)(mmio[i] / HYPE_PAGING_1GB);
            unsigned int idx = (unsigned int)((mmio[i] % HYPE_PAGING_1GB) / HYPE_PAGING_2MB);
            pd = (hype_pte_t *)(g_ram + HYPE_KBOOT_PD0_GPA + (uint64_t)g * 4096ull);
            CHECK_INT("guest MMIO is inside the identity map", 1,
                      (g < HYPE_KBOOT_PD_PAGES) ? 1 : 0);
            CHECK_INT("and its page is present", 1, (pd[idx] & HYPE_PAGING_PRESENT) ? 1 : 0);
        }
    }

    /* The tables must not overlap the zero page or the stack. */
    CHECK_INT("tables end below the zero page", 1,
              (HYPE_KBOOT_PD0_GPA + (uint64_t)HYPE_KBOOT_PD_PAGES * 4096ull <=
               HYPE_KBOOT_ZERO_PAGE_GPA)
                  ? 1
                  : 0);
    CHECK_INT("the zero page ends below the stack", 1,
              (HYPE_KBOOT_ZERO_PAGE_GPA + 4096ull <= HYPE_KBOOT_STACK_TOP_GPA) ? 1 : 0);
    CHECK_INT("the stack ends below the payload", 1,
              (HYPE_KBOOT_STACK_TOP_GPA <= HYPE_KBOOT_LOAD_GPA) ? 1 : 0);
}


/* --- #545: init_size-aware admission, initrd placement, truthful e820 --- */

static void test_init_size_governs_admission(void) {
    hype_kboot_plan_t p;
    uint64_t image = 2560ull + (11ull << 20); /* ~11 MB compressed, the Alpine shape */

    make_valid_head();
    head_hdr()->init_size = 37u << 20; /* what alpine-virt 6.12 actually declares */
    head_hdr()->initrd_addr_max = 0x7FFFFFFFu;
    /* RAM that fits payload+headroom but NOT init_size: the old check admitted this and the
     * kernel died inside its decompressor. */
    CHECK_INT("payload-sized RAM is refused for a real kernel", HYPE_KBOOT_ERR_RAM_TOO_SMALL,
              hype_kboot_plan(g_head, sizeof(g_head), image, (30ull << 20), 0u, 0, 0ull, &p));
    CHECK_INT("init_size-sized RAM is admitted", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, (64ull << 20), 0u, 0, 0ull, &p));
    CHECK_HEX("min_ram uses max(payload, init_size)",
              HYPE_KBOOT_LOAD_GPA + (37ull << 20) + (2ull << 20),
              hype_kboot_min_ram_bytes(11ull << 20, 37ull << 20, 0ull));
    /* A microtest (init_size == 0) keeps the old arithmetic exactly. */
    CHECK_HEX("microtest requirement unchanged", HYPE_KBOOT_LOAD_GPA + 4096ull + (2ull << 20),
              hype_kboot_min_ram_bytes(4096ull, 0ull, 0ull));
}

static void test_initrd_is_placed_high_and_page_aligned(void) {
    hype_kboot_plan_t p;
    uint64_t image = 2560ull + 0x40000ull;
    uint64_t ram = 512ull << 20;
    uint64_t initrd = (9ull << 20) + 123ull; /* deliberately unaligned size */

    make_valid_head();
    head_hdr()->initrd_addr_max = 0x7FFFFFFFu;
    CHECK_INT("plan with initrd ok", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 0u, 0, initrd, &p));
    CHECK_HEX("initrd bytes recorded", initrd, p.initrd_bytes);
    CHECK_HEX("page aligned", 0ull, p.initrd_gpa & 0xFFFull);
    CHECK_INT("as high as RAM allows", 1, p.initrd_gpa + initrd <= ram);
    CHECK_INT("within a page of the top", 1, ram - (p.initrd_gpa + initrd) < 0x1000ull);
    CHECK_INT("above the kernel scratch", 1, p.initrd_gpa >= HYPE_KBOOT_LOAD_GPA);
}

static void test_initrd_respects_addr_max(void) {
    hype_kboot_plan_t p;
    uint64_t image = 2560ull + 0x40000ull;
    uint64_t ram = 2048ull << 20; /* RAM ABOVE the ceiling: the ceiling must win */

    make_valid_head();
    head_hdr()->initrd_addr_max = (128u << 20) - 1u; /* kernel says: below 128 MB */
    CHECK_INT("plan ok", HYPE_KBOOT_OK,
              hype_kboot_plan(g_head, sizeof(g_head), image, ram, 0u, 0, 4096ull, &p));
    CHECK_INT("ceiling respected", 1, p.initrd_gpa + p.initrd_bytes <= (128ull << 20));
}

static void test_initrd_unreachable_is_refused(void) {
    hype_kboot_plan_t p;
    uint64_t image = 2560ull + 0x40000ull;

    make_valid_head();
    /* Ceiling below the kernel's own top: no legal address exists. */
    head_hdr()->initrd_addr_max = (16u << 20) - 1u;
    head_hdr()->init_size = 32u << 20;
    CHECK_INT("initrd below the scratch refused", HYPE_KBOOT_ERR_INITRD_UNREACHABLE,
              hype_kboot_plan(g_head, sizeof(g_head), image, 512ull << 20, 0u, 0, 4096ull, &p));
    /* And an initrd larger than the whole window. */
    head_hdr()->init_size = 0;
    head_hdr()->initrd_addr_max = (1u << 20) - 1u;
    CHECK_INT("initrd larger than the window refused", HYPE_KBOOT_ERR_INITRD_UNREACHABLE,
              hype_kboot_plan(g_head, sizeof(g_head), image, 512ull << 20, 0u, 0, 2ull << 20, &p));
    CHECK_INT("status has words", 1,
              hype_kboot_status_str(HYPE_KBOOT_ERR_INITRD_UNREACHABLE)[0] != 0);
}

static void test_e820_is_truthful(void) {
    hype_linux_e820_entry_t e[HYPE_KBOOT_E820_MAX];
    unsigned int n = hype_kboot_build_e820(512ull << 20, e);
    unsigned int i;
    uint64_t covered = 0;

    CHECK_INT("five entries", 5, (int)n);
    CHECK_INT("page 0 reserved", HYPE_LINUX_E820_TYPE_RESERVED, (int)e[0].type);
    CHECK_HEX("tables+zero page+cmdline reserved from 0x1000", 0x1000ull, e[1].addr);
    CHECK_INT("that block is reserved", HYPE_LINUX_E820_TYPE_RESERVED, (int)e[1].type);
    CHECK_HEX("it covers through the cmdline page", HYPE_KBOOT_CMDLINE_GPA + 0x1000ull,
              e[1].addr + e[1].size);
    CHECK_INT("low RAM usable", HYPE_LINUX_E820_TYPE_RAM, (int)e[2].type);
    CHECK_HEX("PC hole starts at 0xA0000", 0xA0000ull, e[3].addr);
    CHECK_INT("PC hole reserved", HYPE_LINUX_E820_TYPE_RESERVED, (int)e[3].type);
    CHECK_HEX("high RAM from 1 MB", 0x100000ull, e[4].addr);
    CHECK_HEX("high RAM to the top", 512ull << 20, e[4].addr + e[4].size);
    /* Entries tile [0, ram) exactly: no gaps, no overlaps. */
    for (i = 0; i < n; i++) {
        CHECK_HEX("entry starts where the last ended", covered, e[i].addr);
        covered = e[i].addr + e[i].size;
    }
    CHECK_HEX("map covers all RAM", 512ull << 20, covered);
}

int main(void) {
    test_plan_accepts_a_valid_image();
    test_setup_sects_zero_means_four();
    test_rejections();
    test_min_ram_is_the_payload_plus_the_layout();
    test_cmdline_placement();
    test_status_strings();
    test_guest_page_tables_name_guest_addresses();

    test_init_size_governs_admission();
    test_initrd_is_placed_high_and_page_aligned();
    test_initrd_respects_addr_max();
    test_initrd_unreachable_is_refused();
    test_e820_is_truthful();
    if (failures == 0) {
        printf("test_kboot: all checks passed\n");
        return 0;
    }
    printf("test_kboot: %d check(s) failed\n", failures);
    return 1;
}
