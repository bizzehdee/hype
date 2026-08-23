#include <stdio.h>
#include <string.h>
#include "../../devices/ahci.h"
#include "../../arch/x86_64/svm/svm.h" /* hype_svm_set_ahci_trace() */
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

/*
 * #694 coverage follow-up: the tests above only ever build an ATA-style command header
 * (hdr.is_atapi clear) carrying IDENTIFY PACKET DEVICE. process_ahci_command_slot()'s
 * SET FEATURES / unmodelled-command branches of that same "!hdr.is_atapi" arm, its
 * genuine ATAPI-CDB arm (hdr.is_atapi set, a real SCSI command table), its Control-write
 * (soft reset) call site, and its g_ahci_trace-gated debug lines were all unexercised.
 */

static void test_set_features_completes_no_data(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    r.cmd_table[2] = HYPE_AHCI_ATA_CMD_SET_FEATURES;

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("SET FEATURES completes", 0, rc);
    CHECK_HEX("status DRDY|DSC, no error", 0x50u, ahci.p_tfd & 0xFFu);
}

static void test_unmodelled_ata_command_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    r.cmd_table[2] = 0x00u; /* not IDENTIFY PACKET DEVICE or SET FEATURES */

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("an unmodelled ATA command on the ATAPI port is refused", (unsigned)-1, (unsigned)rc);
}

static void test_control_write_triggers_soft_reset(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    r.cmd_table[1] = 0u; /* C bit clear: a Control-register write, not a command */
    r.cmd_table[15] = HYPE_AHCI_ATA_CONTROL_SRST;

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("SRST assert posts no FIS", 0, rc);

    r.cmd_table[15] = 0u; /* release */
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("SRST release completes", 0, rc);
    CHECK_HEX("signature FIS posted", HYPE_AHCI_FIS_TYPE_D2H_REGISTER, r.rx_fis[0x40]);
}

/* Builds a real ATAPI PACKET command: header ATAPI bit SET, H2D FIS command byte
 * ATA_CMD_PACKET (0xA0), and `cdb` copied to the command table's CDB field (offset 0x40). */
static void build_slot0_atapi_cdb(rig_t *r, const uint8_t cdb[16], uint64_t prd_data_gpa,
                                  uint32_t prd_len) {
    uint32_t opts;
    unsigned i;

    memset(r->cmd_list, 0, sizeof(r->cmd_list));
    opts = 5u | (1u << 5) /* ATAPI bit */ | (1u << 16) /* prdtl = 1 */;
    put32(r->cmd_list + 0, opts);
    put32(r->cmd_list + 8, (uint32_t)rig_gpa(r, r->cmd_table));
    put32(r->cmd_list + 12, (uint32_t)(rig_gpa(r, r->cmd_table) >> 32));

    memset(r->cmd_table, 0, sizeof(r->cmd_table));
    r->cmd_table[0] = 0x27u;
    r->cmd_table[1] = HYPE_AHCI_FIS_H2D_FLAG_C;
    r->cmd_table[2] = HYPE_ATA_CMD_PACKET;
    for (i = 0; i < 16u; i++) {
        r->cmd_table[0x40 + i] = cdb[i];
    }

    put32(r->cmd_table + 0x80 + 0, (uint32_t)prd_data_gpa);
    put32(r->cmd_table + 0x80 + 4, (uint32_t)(prd_data_gpa >> 32));
    put32(r->cmd_table + 0x80 + 12, prd_len - 1u);
}

static void test_atapi_inquiry_synth_response(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    uint8_t cdb[16];
    int rc;

    setup_rig(&r, &ahci, &atapi);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = HYPE_ATAPI_CMD_INQUIRY;
    build_slot0_atapi_cdb(&r, cdb, rig_gpa(&r, r.data), 36u);

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("INQUIRY completes", 0, rc);
    CHECK_HEX("status GOOD", 0x50u, ahci.p_tfd & 0xFFu);
}

static void test_atapi_read10_media_response(void) {
    /*
     * READ(10) streams from atapi->media_data, one whole 2048-byte CD sector -- larger
     * than setup_rig()'s own 512-byte r->data, so this test backs the drive with its own,
     * separate 2048-byte media buffer instead. The PRD, still only 512 bytes (r->data),
     * is shorter than the sector: this exercises the non-streamed copy AND the
     * short-transfer accounting in the same call, both otherwise unreachable from this
     * file's other tests.
     */
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    uint8_t media[HYPE_ATAPI_SECTOR_SIZE];
    uint8_t cdb[16];
    int rc;

    setup_rig(&r, &ahci, &atapi);
    memset(media, 0x77u, sizeof(media));
    hype_atapi_reset(&atapi, media, sizeof(media));
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = HYPE_ATAPI_CMD_READ10;
    cdb[8] = 1u; /* transfer length: 1 block */
    build_slot0_atapi_cdb(&r, cdb, rig_gpa(&r, r.data), 512u);

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("READ10 completes (short PRD, real hardware reports a short transfer)", 0, rc);
    CHECK_HEX("status GOOD", 0x50u, ahci.p_tfd & 0xFFu);
}

static void test_atapi_check_condition_reports_error_status(void) {
    /* TEST UNIT READY with no media attached: a real CHECK_CONDITION completion,
     * exercising the CDB arm's error_reg/status_reg branch (0x51, sense-key nibble). */
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    uint8_t cdb[16];
    int rc;

    setup_rig(&r, &ahci, &atapi);
    hype_atapi_reset(&atapi, 0, 0); /* no media */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = HYPE_ATAPI_CMD_TEST_UNIT_READY;
    build_slot0_atapi_cdb(&r, cdb, rig_gpa(&r, r.data), 1u);

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("CHECK CONDITION still completes the slot", 0, rc);
    CHECK_HEX("status CHECK_CONDITION|ERR", 0x51u, ahci.p_tfd & 0xFFu);
}

static void test_ahci_trace_enabled_sweeps_debug_lines(void) {
    /* Not a behavioral test: g_ahci_trace only gates hype_debug_print() calls, and
     * enabling it for one pass over the already-proven-correct paths above is the
     * cheapest way to cover those trace lines without duplicating every test. */
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    uint8_t cdb[16];
    int rc;

    hype_svm_set_ahci_trace(1);

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("IDENTIFY PACKET DEVICE still completes with tracing on", 0, rc);

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    r.cmd_table[2] = HYPE_AHCI_ATA_CMD_SET_FEATURES;
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("SET FEATURES still completes with tracing on", 0, rc);

    setup_rig(&r, &ahci, &atapi);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = HYPE_ATAPI_CMD_INQUIRY;
    build_slot0_atapi_cdb(&r, cdb, rig_gpa(&r, r.data), 36u);
    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("INQUIRY still completes with tracing on", 0, rc);

    hype_svm_set_ahci_trace(0);
}

static void test_out_of_range_rx_fis_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    ahci.p_fb = 0xFFFF0000u; /* received-FIS area now points outside the mapped rig */
    ahci.p_fbu = 0u;

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("out-of-range received-FIS area is refused", (unsigned)-1, (unsigned)rc);
}

static void test_bus_master_refused_returns_zero(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    hype_ahci_set_bus_master(&ahci, 0);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("no bus master: ignored, not an error", 0, rc);
}

static void test_malformed_ata_style_fis_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));
    r.cmd_table[0] = 0x00u; /* not a Register H2D FIS at all */

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("a malformed ATA-style FIS is refused", (unsigned)-1, (unsigned)rc);
}

static void test_malformed_atapi_fis_refused(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    uint8_t cdb[16];
    int rc;

    setup_rig(&r, &ahci, &atapi);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = HYPE_ATAPI_CMD_INQUIRY;
    build_slot0_atapi_cdb(&r, cdb, rig_gpa(&r, r.data), 36u);
    r.cmd_table[2] = 0x00u; /* not ATA_CMD_PACKET (0xA0) */

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("an ATAPI header not carrying PACKET is refused", (unsigned)-1, (unsigned)rc);
}

static void test_identify_packet_raises_port0_interrupt(void) {
    rig_t r;
    hype_ahci_t ahci;
    hype_atapi_t atapi;
    int rc;

    setup_rig(&r, &ahci, &atapi);
    ahci.p_ie = HYPE_AHCI_PIS_PSS | HYPE_AHCI_PIS_DHRS;
    build_slot0(&r, rig_gpa(&r, r.cmd_table), rig_gpa(&r, r.data));

    rc = process_ahci_command_slot(&ahci, &atapi, &r.map, 0u);
    CHECK_HEX("IDENTIFY PACKET completes", 0, rc);
    CHECK_HEX("global IS.PORT0 latched", HYPE_AHCI_IS_PORT0, ahci.is & HYPE_AHCI_IS_PORT0);
}

int main(void) {
    test_legitimate_request_succeeds();
    test_out_of_range_command_list_refused();
    test_out_of_range_command_table_refused();
    test_oversized_prdtl_runs_table_out_of_bounds_refused();
    test_out_of_range_prd_data_pointer_refused();

    /* process_ahci_command_slot() unconditionally traces its first 24 completions
     * (the #344 "ahci-cpl" line) regardless of outcome -- every call below needs the
     * host-unsafe DEBUG sink suppressed, not just the ones expected to refuse. */
    hype_debug_set_level(HYPE_LOG_ERROR);
    test_set_features_completes_no_data();
    test_unmodelled_ata_command_refused();
    test_control_write_triggers_soft_reset();
    test_atapi_inquiry_synth_response();
    test_atapi_read10_media_response();
    test_atapi_check_condition_reports_error_status();
    test_ahci_trace_enabled_sweeps_debug_lines();
    test_out_of_range_rx_fis_refused();
    test_bus_master_refused_returns_zero();
    test_malformed_ata_style_fis_refused();
    test_malformed_atapi_fis_refused();
    test_identify_packet_raises_port0_interrupt();
    hype_debug_set_level(HYPE_LOG_DEBUG);

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
