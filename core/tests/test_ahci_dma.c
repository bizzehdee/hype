#include <stdio.h>
#include <string.h>
#include "../../devices/ahci.h"
#include "../fatal.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/*
 * #663: process_ahci_command_slot() (arch/x86_64/svm/svm_vcpu.c, shared verbatim by the VMX
 * MMIO handler in arch/x86_64/vmx/vmcs_hw.c) is the ATAPI-capable sibling of
 * process_ahci_ata_command_slot() (already covered by #672's regression test in
 * core/tests/test_ahci.c). It routes the command-list header, the command table + PRDT, and
 * each PRD's data pointer through the VM's bounds-checked gpa map -- this proves that path,
 * one guest-controlled address at a time, mirroring test_virtio_blk.c's tqm_t rig.
 */

#define RIG_GUEST_BASE 0x7000000000ull

typedef struct {
    uint8_t cmd_list[32];      /* one command-header slot */
    uint8_t cmd_table[0x80 + 16]; /* CFIS/ACMD/reserved block + one PRDT entry */
    uint8_t data[512];         /* the one in-bounds PRD data target */
    uint8_t rx_fis[0x40 + 20]; /* received-FIS area: D2H Register FIS at +0x40 */
    hype_gpa_map_t map;
} rig_t;

static uint64_t rig_gpa(const rig_t *r, const void *host_ptr) {
    return RIG_GUEST_BASE + (uint64_t)((const uint8_t *)host_ptr - (const uint8_t *)r);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Builds a legitimate slot 0: command header -> command table (0 PRD entries, is_atapi=0,
 * H2D Register FIS type 0x27, command IDENTIFY PACKET DEVICE 0xA1) -> one PRD entry pointing
 * at r->data, sized for the full 512-byte IDENTIFY response. `cmd_table_gpa` and `prd_data_gpa`
 * are parameters so each test can perturb exactly one address out of the mapped range. */
static void build_slot0(rig_t *r, uint64_t cmd_table_gpa, uint64_t prd_data_gpa) {
    uint32_t opts;

    memset(r->cmd_list, 0, sizeof(r->cmd_list));
    opts = 5u /* CFL: 5 dwords */ | (1u << 16) /* prdtl = 1 */;
    put32(r->cmd_list + 0, opts);
    put32(r->cmd_list + 8, (uint32_t)cmd_table_gpa);
    put32(r->cmd_list + 12, (uint32_t)(cmd_table_gpa >> 32));

    memset(r->cmd_table, 0, sizeof(r->cmd_table));
    r->cmd_table[0] = 0x27u; /* Register H2D FIS */
    r->cmd_table[1] = HYPE_AHCI_FIS_H2D_FLAG_C; /* "C" bit: this FIS updates Command, not a
                                                  * Control-register write (soft reset) */
    r->cmd_table[2] = HYPE_AHCI_ATA_CMD_IDENTIFY_PACKET_DEVICE;

    /* the one PRDT entry, at offset 0x80 */
    put32(r->cmd_table + 0x80 + 0, (uint32_t)prd_data_gpa);
    put32(r->cmd_table + 0x80 + 4, (uint32_t)(prd_data_gpa >> 32));
    put32(r->cmd_table + 0x80 + 12, 511u); /* DBC = byte_count - 1 -> 512 bytes */
}

static void setup_rig(rig_t *r, hype_ahci_t *ahci, hype_atapi_t *atapi) {
    memset(r, 0, sizeof(*r));
    hype_gpa_map_reset(&r->map);
    hype_gpa_map_add(&r->map, RIG_GUEST_BASE, (uint64_t)(uintptr_t)r, sizeof(*r));

    hype_ahci_reset(ahci); /* bus_master defaults enabled */
    ahci->p_clb = (uint32_t)rig_gpa(r, r->cmd_list);
    ahci->p_clbu = (uint32_t)(rig_gpa(r, r->cmd_list) >> 32);
    ahci->p_fb = (uint32_t)rig_gpa(r, r->rx_fis);
    ahci->p_fbu = (uint32_t)(rig_gpa(r, r->rx_fis) >> 32);
    hype_atapi_reset(atapi, r->data, sizeof(r->data)); /* media content irrelevant to this path */
}

static void test_legitimate_request_succeeds(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));

    hype_debug_set_level(HYPE_LOG_ERROR); /* in case of an unexpected refusal along the way */
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    hype_debug_set_level(HYPE_LOG_DEBUG);
    CHECK_HEX("a fully in-bounds command completes", 0, rc);
}

static void test_out_of_range_command_list_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    /* p_clb/p_clbu now point well outside the mapped rig, overriding setup_rig's own value. */
    ahci.p_clb = 0xFFFF0000u;
    ahci.p_clbu = 0u;

    hype_debug_set_level(HYPE_LOG_ERROR); /* silence the expected refusal's serial log */
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    hype_debug_set_level(HYPE_LOG_DEBUG);
    CHECK_HEX("out-of-range command list is refused", (unsigned)-1, (unsigned)rc);
}

static void test_out_of_range_command_table_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, RIG_GUEST_BASE + 0x10000000ull, rig_gpa(&r, r.data));

    hype_debug_set_level(HYPE_LOG_ERROR);
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    hype_debug_set_level(HYPE_LOG_DEBUG);
    CHECK_HEX("out-of-range command table is refused", (unsigned)-1, (unsigned)rc);
}

static void test_oversized_prdtl_runs_table_out_of_bounds_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    uint32_t opts;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    /* A prdtl this large makes (0x80 + prdtl*16) run the "command table" span past the rig's
     * mapped end, even though cmd_table_phys itself points inside it. */
    opts = 5u | (0xFFFFu << 16);
    put32(r.cmd_list + 0, opts);

    hype_debug_set_level(HYPE_LOG_ERROR);
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    hype_debug_set_level(HYPE_LOG_DEBUG);
    CHECK_HEX("an oversized prdtl running the command table out of bounds is refused",
             (unsigned)-1, (unsigned)rc);
}

static void test_out_of_range_prd_data_pointer_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), RIG_GUEST_BASE + 0x20000000ull);

    hype_debug_set_level(HYPE_LOG_ERROR);
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    hype_debug_set_level(HYPE_LOG_DEBUG);
    CHECK_HEX("out-of-range PRD data pointer is refused", (unsigned)-1, (unsigned)rc);
}

int main(void) {
    test_legitimate_request_succeeds();
    test_out_of_range_command_list_refused();
    test_out_of_range_command_table_refused();
    test_oversized_prdtl_runs_table_out_of_bounds_refused();
    test_out_of_range_prd_data_pointer_refused();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
