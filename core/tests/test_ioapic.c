#include <stdio.h>
#include "../../devices/ioapic.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* Select register `index` via IOREGSEL, then write `value` to IOWIN. */
static void reg_write(hype_ioapic_t *io, uint32_t index, uint32_t value) {
    hype_ioapic_mmio_write(io, HYPE_IOAPIC_REG_IOREGSEL, index);
    hype_ioapic_mmio_write(io, HYPE_IOAPIC_REG_IOWIN, value);
}

static uint32_t reg_read(hype_ioapic_t *io, uint32_t index) {
    uint32_t v = 0;
    hype_ioapic_mmio_write(io, HYPE_IOAPIC_REG_IOREGSEL, index);
    hype_ioapic_mmio_read(io, HYPE_IOAPIC_REG_IOWIN, &v);
    return v;
}

static void test_reset_all_masked(void) {
    hype_ioapic_t io;
    uint32_t i;
    uint8_t vec = 0;
    hype_ioapic_reset(&io);
    CHECK_HEX("ioregsel 0 at reset", 0, io.ioregsel);
    CHECK_HEX("id 0 at reset", 0, io.id);
    for (i = 0; i < HYPE_IOAPIC_NUM_RTES; i++) {
        CHECK_HEX("rte masked at reset", HYPE_IOAPIC_RTE_MASK, (uint32_t)io.rte[i]);
        /* a masked line never delivers */
        CHECK_HEX("masked line does not raise", 0, hype_ioapic_raise(&io, i, &vec));
    }
}

static void test_version_register(void) {
    hype_ioapic_t io;
    uint32_t ver;
    hype_ioapic_reset(&io);
    ver = reg_read(&io, HYPE_IOAPIC_INDEX_VER);
    CHECK_HEX("version byte", HYPE_IOAPIC_VERSION, ver & 0xFFu);
    CHECK_HEX("max redirection entry = count-1", HYPE_IOAPIC_NUM_RTES - 1u, (ver >> 16) & 0xFFu);
}

static void test_id_read_write(void) {
    hype_ioapic_t io;
    hype_ioapic_reset(&io);
    reg_write(&io, HYPE_IOAPIC_INDEX_ID, 0x0Fu << 24);
    CHECK_HEX("id stored in bits 27:24", 0x0Fu << 24, reg_read(&io, HYPE_IOAPIC_INDEX_ID));
    CHECK_HEX("id field", 0x0Fu, io.id);
}

static void test_ioregsel_latches(void) {
    hype_ioapic_t io;
    uint32_t v = 0;
    hype_ioapic_reset(&io);
    hype_ioapic_mmio_write(&io, HYPE_IOAPIC_REG_IOREGSEL, 0x1234u);
    hype_ioapic_mmio_read(&io, HYPE_IOAPIC_REG_IOREGSEL, &v);
    CHECK_HEX("IOREGSEL latches only low 8 bits", 0x34u, v);
}

static void test_rte_write_low_high(void) {
    hype_ioapic_t io;
    hype_ioapic_reset(&io);
    /* entry 5: low = vector 0x33, unmasked, edge; high = destination 0x02 */
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 5u * 2u, 0x00000033u);
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 5u * 2u + 1u, 0x02000000u);
    CHECK_HEX("rte low read back", 0x00000033u, reg_read(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 5u * 2u));
    CHECK_HEX("rte high read back", 0x02000000u,
              reg_read(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 5u * 2u + 1u));
    CHECK_HEX("full 64-bit entry", 0x0200000000000033ULL, io.rte[5]);
}

static void test_edge_unmasked_delivers(void) {
    hype_ioapic_t io;
    uint8_t vec = 0;
    hype_ioapic_reset(&io);
    /* GSI2 (the PIT->GSI2 override): vector 0x30, unmasked, edge, Fixed. */
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 2u * 2u, 0x30u);
    CHECK_HEX("edge unmasked raises", 1, hype_ioapic_raise(&io, 2u, &vec));
    CHECK_HEX("delivers programmed vector", 0x30u, vec);
    /* edge re-raises every assertion */
    CHECK_HEX("edge raises again", 1, hype_ioapic_raise(&io, 2u, &vec));
}

static void test_level_sets_and_clears_remote_irr(void) {
    hype_ioapic_t io;
    uint8_t vec = 0;
    hype_ioapic_reset(&io);
    /* GSI11 (AHCI): vector 0x40, unmasked, LEVEL, Fixed. */
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 11u * 2u, 0x40u | HYPE_IOAPIC_RTE_TRIGGER_LEVEL);
    CHECK_HEX("level first assertion delivers", 1, hype_ioapic_raise(&io, 11u, &vec));
    CHECK_HEX("level vector", 0x40u, vec);
    CHECK_HEX("remote IRR now set", HYPE_IOAPIC_RTE_REMOTE_IRR,
              (uint32_t)io.rte[11] & HYPE_IOAPIC_RTE_REMOTE_IRR);
    CHECK_HEX("level does not re-inject while remote-IRR set", 0, hype_ioapic_raise(&io, 11u, &vec));
    hype_ioapic_eoi(&io, 0x40u);
    CHECK_HEX("EOI clears remote IRR", 0, (uint32_t)io.rte[11] & HYPE_IOAPIC_RTE_REMOTE_IRR);
    CHECK_HEX("level delivers again after EOI", 1, hype_ioapic_raise(&io, 11u, &vec));
}

static void test_guest_cannot_write_remote_irr(void) {
    hype_ioapic_t io;
    uint8_t vec = 0;
    hype_ioapic_reset(&io);
    /* Deliver a level IRQ so Remote-IRR is set, then try to clear it via a
     * normal RTE write -- hardware ignores guest writes to the RO bit. */
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 3u * 2u, 0x50u | HYPE_IOAPIC_RTE_TRIGGER_LEVEL);
    hype_ioapic_raise(&io, 3u, &vec);
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 3u * 2u,
              0x50u | HYPE_IOAPIC_RTE_TRIGGER_LEVEL); /* rewrite, no remote-IRR bit */
    CHECK_HEX("remote-IRR survives a guest RTE rewrite", HYPE_IOAPIC_RTE_REMOTE_IRR,
              (uint32_t)io.rte[3] & HYPE_IOAPIC_RTE_REMOTE_IRR);
}

static void test_masked_and_nonfixed_and_oob(void) {
    hype_ioapic_t io;
    uint8_t vec = 0xAB;
    hype_ioapic_reset(&io);
    /* masked (reset default) */
    CHECK_HEX("masked entry does not raise", 0, hype_ioapic_raise(&io, 4u, &vec));
    /* unmasked but NMI delivery mode (100b) -> not a plain vector */
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 4u * 2u, 0x60u | (0x4u << HYPE_IOAPIC_RTE_DELMODE_SHIFT));
    CHECK_HEX("non-Fixed delivery mode does not inject a vector", 0, hype_ioapic_raise(&io, 4u, &vec));
    /* out-of-range GSI */
    CHECK_HEX("out-of-range gsi does not raise", 0, hype_ioapic_raise(&io, HYPE_IOAPIC_NUM_RTES, &vec));
}

static void test_arb_register(void) {
    hype_ioapic_t io;
    hype_ioapic_reset(&io);
    reg_write(&io, HYPE_IOAPIC_INDEX_ID, 0x07u << 24);
    /* ARB (0x02) mirrors the ID in bits 27:24. */
    CHECK_HEX("ARB reflects id", 0x07u << 24, reg_read(&io, HYPE_IOAPIC_INDEX_ARB));
}

static void test_unimplemented_index_reads_zero(void) {
    hype_ioapic_t io;
    hype_ioapic_reset(&io);
    /* An index between ARB and the redirection base is unimplemented -> 0. */
    CHECK_HEX("unimplemented register reads 0", 0, reg_read(&io, 0x08u));
    /* An index past the last redirection entry is also unimplemented. */
    CHECK_HEX("index past last RTE reads 0", 0,
              reg_read(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + HYPE_IOAPIC_NUM_RTES * 2u));
}

static void test_readonly_registers_ignore_writes(void) {
    hype_ioapic_t io;
    uint32_t ver_before;
    hype_ioapic_reset(&io);
    ver_before = reg_read(&io, HYPE_IOAPIC_INDEX_VER);
    reg_write(&io, HYPE_IOAPIC_INDEX_VER, 0xDEADBEEFu);  /* VER is read-only */
    reg_write(&io, HYPE_IOAPIC_INDEX_ARB, 0xDEADBEEFu);  /* ARB is read-only */
    CHECK_HEX("VER unchanged by write", ver_before, reg_read(&io, HYPE_IOAPIC_INDEX_VER));
    CHECK_HEX("ARB unchanged by write (id still 0)", 0, reg_read(&io, HYPE_IOAPIC_INDEX_ARB));
}

static void test_eoi_ignores_nonmatching(void) {
    hype_ioapic_t io;
    uint8_t vec = 0;
    hype_ioapic_reset(&io);
    /* An EOI with no matching level entry, and an EOI targeting an EDGE entry,
     * are both no-ops (covers the eoi loop's negative branches). */
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 6u * 2u, 0x70u); /* edge, unmasked */
    hype_ioapic_raise(&io, 6u, &vec);
    hype_ioapic_eoi(&io, 0x70u); /* edge entry: nothing to clear */
    hype_ioapic_eoi(&io, 0x99u); /* no entry carries this vector */
    CHECK_HEX("edge entry unaffected by EOI", 0x00000070u, (uint32_t)io.rte[6]);
}

static void test_deassert_clears_remote_irr(void) {
    hype_ioapic_t io;
    uint8_t vec = 0;
    hype_ioapic_reset(&io);
    /* level line, unmasked */
    reg_write(&io, HYPE_IOAPIC_INDEX_REDIR_BASE + 16u * 2u, 0x80u | HYPE_IOAPIC_RTE_TRIGGER_LEVEL);
    hype_ioapic_raise(&io, 16u, &vec);
    CHECK_HEX("remote-IRR set after raise", HYPE_IOAPIC_RTE_REMOTE_IRR,
              (uint32_t)io.rte[16] & HYPE_IOAPIC_RTE_REMOTE_IRR);
    hype_ioapic_deassert(&io, 16u);
    CHECK_HEX("deassert clears remote-IRR", 0, (uint32_t)io.rte[16] & HYPE_IOAPIC_RTE_REMOTE_IRR);
    CHECK_HEX("re-injects after deassert", 1, hype_ioapic_raise(&io, 16u, &vec));
    /* out-of-range gsi is a no-op (must not crash / touch memory) */
    hype_ioapic_deassert(&io, HYPE_IOAPIC_NUM_RTES);
}

static void test_bad_offset_rejected(void) {
    hype_ioapic_t io;
    uint32_t v = 0;
    hype_ioapic_reset(&io);
    CHECK_HEX("read of unsupported offset rejected", (uint32_t)-1,
              (uint32_t)hype_ioapic_mmio_read(&io, 0x08u, &v));
    CHECK_HEX("write of unsupported offset rejected", (uint32_t)-1,
              (uint32_t)hype_ioapic_mmio_write(&io, 0x08u, 0));
}

int main(void) {
    test_reset_all_masked();
    test_version_register();
    test_id_read_write();
    test_ioregsel_latches();
    test_rte_write_low_high();
    test_edge_unmasked_delivers();
    test_level_sets_and_clears_remote_irr();
    test_guest_cannot_write_remote_irr();
    test_masked_and_nonfixed_and_oob();
    test_arb_register();
    test_unimplemented_index_reads_zero();
    test_readonly_registers_ignore_writes();
    test_eoi_ignores_nonmatching();
    test_deassert_clears_remote_irr();
    test_bad_offset_rejected();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
