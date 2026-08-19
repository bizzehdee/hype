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
    st = hype_kboot_plan(g_head, sizeof(g_head), image, 512ull * 1024ull * 1024ull, &p);

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
              hype_kboot_plan(g_head, sizeof(g_head), 4096ull * 64ull, 512ull * 1024ull * 1024ull, &p));
    CHECK_INT("setup_sects 0 is treated as 4", 2560, p.payload_file_offset);
}

static void test_rejections(void) {
    hype_kboot_plan_t p;
    hype_kboot_status_t st;

    make_valid_head();
    head_hdr()->boot_flag = 0;
    st = hype_kboot_plan(g_head, sizeof(g_head), 1ull << 20, 1ull << 30, &p);
    CHECK_INT("no 0xAA55 boot flag is refused", HYPE_KBOOT_ERR_BAD_HEADER, st);

    make_valid_head();
    head_hdr()->xloadflags = 0;
    st = hype_kboot_plan(g_head, sizeof(g_head), 1ull << 20, 1ull << 30, &p);
    CHECK_INT("a 32-bit-only kernel is refused, not degraded", HYPE_KBOOT_ERR_BAD_HEADER, st);

    make_valid_head();
    st = hype_kboot_plan(g_head, HYPE_LINUX_SETUP_HEADER_OFFSET, 1ull << 20, 1ull << 30, &p);
    CHECK_INT("too few head bytes to decide", HYPE_KBOOT_ERR_HEAD_TOO_SMALL, st);

    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 16ull, 1ull << 30, &p);
    CHECK_INT("a file too small to hold a header", HYPE_KBOOT_ERR_SHORT_IMAGE, st);

    /* Header valid, but the file ends exactly where the payload would begin. */
    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 2560ull, 1ull << 30, &p);
    CHECK_INT("no payload after the setup region", HYPE_KBOOT_ERR_NO_PAYLOAD, st);

    /* A 64 MB payload cannot load at 16 MB inside 32 MB of guest RAM. */
    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 2560ull + (64ull << 20), 32ull << 20, &p);
    CHECK_INT("a payload larger than the VM's RAM is refused", HYPE_KBOOT_ERR_RAM_TOO_SMALL, st);

    st = hype_kboot_plan(0, sizeof(g_head), 1ull << 20, 1ull << 30, &p);
    CHECK_INT("a null image head is refused", HYPE_KBOOT_ERR_BAD_HEADER, st);
    make_valid_head();
    st = hype_kboot_plan(g_head, sizeof(g_head), 1ull << 20, 1ull << 30, 0);
    CHECK_INT("a null plan output is refused", HYPE_KBOOT_ERR_BAD_HEADER, st);
}

/* The RAM floor must be reported as a usable number, not just as "too small". */
static void test_min_ram_is_the_payload_plus_the_layout(void) {
    CHECK_HEX("min RAM covers load address, payload and headroom",
              HYPE_KBOOT_LOAD_GPA + (1ull << 20) + (2ull << 20),
              hype_kboot_min_ram_bytes(1ull << 20));

    /* A payload that fits exactly at the reported floor must then plan. */
    {
        hype_kboot_plan_t p;
        uint64_t payload = 1ull << 20;
        make_valid_head();
        CHECK_INT("exactly the reported floor is enough", HYPE_KBOOT_OK,
                  hype_kboot_plan(g_head, sizeof(g_head), 2560ull + payload,
                                  hype_kboot_min_ram_bytes(payload), &p));
        CHECK_INT("one byte under the floor is not", HYPE_KBOOT_ERR_RAM_TOO_SMALL,
                  hype_kboot_plan(g_head, sizeof(g_head), 2560ull + payload,
                                  hype_kboot_min_ram_bytes(payload) - 1ull, &p));
    }
}

static void test_status_strings(void) {
    hype_kboot_status_t all[] = {HYPE_KBOOT_OK,           HYPE_KBOOT_ERR_SHORT_IMAGE,
                                HYPE_KBOOT_ERR_BAD_HEADER, HYPE_KBOOT_ERR_NO_PAYLOAD,
                                HYPE_KBOOT_ERR_RAM_TOO_SMALL, HYPE_KBOOT_ERR_HEAD_TOO_SMALL};
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

int main(void) {
    test_plan_accepts_a_valid_image();
    test_setup_sects_zero_means_four();
    test_rejections();
    test_min_ram_is_the_payload_plus_the_layout();
    test_status_strings();
    test_guest_page_tables_name_guest_addresses();
    if (failures == 0) {
        printf("test_kboot: all checks passed\n");
        return 0;
    }
    printf("test_kboot: %d check(s) failed\n", failures);
    return 1;
}
