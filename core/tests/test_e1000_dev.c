#include <stdio.h>
#include <string.h>
#include "../../devices/e1000_dev.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_TRUE(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

static const uint8_t MAC[6] = {0x52, 0x54, 0x00, 0x11, 0x22, 0x33};

static uint32_t rd(hype_e1000_dev_t *d, uint32_t off) {
    uint32_t v = 0xDEADBEEFu;
    if (hype_e1000_dev_reg_read(d, off, 4, &v) != 0) {
        printf("FAIL: read(0x%x) unexpectedly rejected\n", off);
        failures++;
    }
    return v;
}

static void wr(hype_e1000_dev_t *d, uint32_t off, uint32_t v) {
    if (hype_e1000_dev_reg_write(d, off, 4, v) != 0) {
        printf("FAIL: write(0x%x) unexpectedly rejected\n", off);
        failures++;
    }
}

/* Brings the device up the way a driver does, so the readiness tests start from a state a guest
 * could actually have produced. */
static void bring_up(hype_e1000_dev_t *d) {
    hype_e1000_dev_reset(d, MAC);
    wr(d, HYPE_E1000_REG_TDBAL, 0x00200000u);
    wr(d, HYPE_E1000_REG_TDBAH, 0u);
    wr(d, HYPE_E1000_REG_TDLEN, 32u * HYPE_E1000_DESC_BYTES);
    wr(d, HYPE_E1000_REG_TDH, 0u);
    wr(d, HYPE_E1000_REG_TDT, 0u);
    wr(d, HYPE_E1000_REG_TCTL, HYPE_E1000_TCTL_EN);
    wr(d, HYPE_E1000_REG_RDBAL, 0x00300000u);
    wr(d, HYPE_E1000_REG_RDBAH, 0u);
    wr(d, HYPE_E1000_REG_RDLEN, 32u * HYPE_E1000_DESC_BYTES);
    wr(d, HYPE_E1000_REG_RDH, 0u);
    wr(d, HYPE_E1000_REG_RDT, 31u);
    wr(d, HYPE_E1000_REG_RCTL, HYPE_E1000_RCTL_EN);
}

/*
 * A driver waits for STATUS.LU before it will transmit, so a model reporting link-down is a guest
 * that never sends a packet. hype reports the GUEST's link -- which is to hype -- and deliberately
 * not the uplink's: a guest that stopped transmitting because of something beyond a router is not
 * how any real network behaves.
 */
static void test_link_is_always_up(void) {
    hype_e1000_dev_t d;

    hype_e1000_dev_reset(&d, MAC);
    CHECK_TRUE("link up", (rd(&d, HYPE_E1000_REG_STATUS) & HYPE_E1000_STATUS_LU) != 0u);
    CHECK_TRUE("full duplex", (rd(&d, HYPE_E1000_REG_STATUS) & HYPE_E1000_STATUS_FD) != 0u);
    CHECK_TRUE("an EEPROM is reported present",
               (rd(&d, HYPE_E1000_REG_EECD) & (1u << 8)) != 0u);
}

/* A driver reads the MAC from RAL/RAH or from the EEPROM. Both must give the same answer, because a
 * driver uses one or the other and hype cannot know which. */
static void test_mac_readable_two_ways_and_they_agree(void) {
    hype_e1000_dev_t d;
    uint32_t ral;
    uint32_t rah;
    hype_e1000_mac_t decoded;
    unsigned int i;
    uint8_t from_eeprom[6];

    hype_e1000_dev_reset(&d, MAC);
    ral = rd(&d, HYPE_E1000_REG_RAL0);
    rah = rd(&d, HYPE_E1000_REG_RAH0);

    /* Decoded with hype's OWN host-side decoder, which is the strongest form of this check: if the
     * guest device and the host driver disagreed about the encoding, hype could not drive its own
     * emulated NIC. */
    hype_e1000_mac_from_ral_rah(ral, rah, &decoded);
    CHECK_TRUE("hype's own decoder accepts the address the guest device reports", decoded.valid);
    for (i = 0; i < 6u; i++) {
        CHECK_HEX("RAL/RAH octet", MAC[i], decoded.addr[i]);
    }

    /* And via the EEPROM: three words, low byte first within each. */
    for (i = 0; i < 3u; i++) {
        uint32_t w;
        wr(&d, HYPE_E1000_REG_EERD, (uint32_t)i << HYPE_E1000_EERD_ADDR_SHIFT);
        w = rd(&d, HYPE_E1000_REG_EERD);
        CHECK_TRUE("the EEPROM read reports DONE", (w & HYPE_E1000_EERD_DONE) != 0u);
        w >>= HYPE_E1000_EERD_DATA_SHIFT;
        from_eeprom[i * 2u] = (uint8_t)(w & 0xFFu);
        from_eeprom[i * 2u + 1u] = (uint8_t)((w >> 8) & 0xFFu);
    }
    for (i = 0; i < 6u; i++) {
        CHECK_HEX("the EEPROM path gives the same octet", MAC[i], from_eeprom[i]);
    }
}

/*
 * IMS is SET-ONLY. Treating it as an assignment would silently disable every cause the driver did
 * not mention in its most recent write -- and a driver enabling RXT0 after TXDW would lose TXDW.
 */
static void test_ims_sets_and_imc_clears(void) {
    hype_e1000_dev_t d;

    hype_e1000_dev_reset(&d, MAC);
    wr(&d, HYPE_E1000_REG_IMS, HYPE_E1000_ICR_TXDW);
    wr(&d, HYPE_E1000_REG_IMS, HYPE_E1000_ICR_RXT0);
    CHECK_HEX("both causes are enabled -- the second write did not replace the first",
              HYPE_E1000_ICR_TXDW | HYPE_E1000_ICR_RXT0, rd(&d, HYPE_E1000_REG_IMS));

    wr(&d, HYPE_E1000_REG_IMC, HYPE_E1000_ICR_TXDW);
    CHECK_HEX("IMC cleared exactly one", HYPE_E1000_ICR_RXT0, rd(&d, HYPE_E1000_REG_IMS));
}

static void test_icr_is_read_to_clear_and_write_to_clear(void) {
    hype_e1000_dev_t d;

    hype_e1000_dev_reset(&d, MAC);
    wr(&d, HYPE_E1000_REG_IMS, HYPE_E1000_ICR_RXT0);

    CHECK_HEX("nothing pending initially", 0, hype_e1000_dev_irq_pending(&d));
    CHECK_HEX("raising an UNMASKED cause asserts", 1, hype_e1000_dev_raise(&d, HYPE_E1000_ICR_RXT0));
    CHECK_HEX("and stays asserted -- it is level-triggered", 1, hype_e1000_dev_irq_pending(&d));
    CHECK_HEX("the driver reads the cause", HYPE_E1000_ICR_RXT0, rd(&d, HYPE_E1000_REG_ICR));
    CHECK_HEX("the read cleared it", 0, hype_e1000_dev_irq_pending(&d));

    /* A MASKED cause is recorded but must not assert a line: the driver did not ask to be told. */
    CHECK_HEX("a masked cause does not assert", 0, hype_e1000_dev_raise(&d, HYPE_E1000_ICR_LSC));
    CHECK_HEX("...but is still visible in ICR when read", HYPE_E1000_ICR_LSC,
              rd(&d, HYPE_E1000_REG_ICR));

    /* Write-to-clear, which older drivers use instead of relying on the read. */
    (void)hype_e1000_dev_raise(&d, HYPE_E1000_ICR_RXT0);
    wr(&d, HYPE_E1000_REG_ICR, HYPE_E1000_ICR_RXT0);
    CHECK_HEX("writing the bit cleared it", 0, hype_e1000_dev_irq_pending(&d));

    CHECK_HEX("a null device never asserts", 0, hype_e1000_dev_irq_pending(0));
    CHECK_HEX("raising on a null device is safe", 0, hype_e1000_dev_raise(0, 1u));
}

/*
 * CTRL.RST must clear everything the driver negotiated, not just CTRL. A model that cleared only its
 * own register would leave a driver reinitialising a device that still held its old ring addresses --
 * a bug that appears only on the second driver load.
 */
static void test_reset_clears_the_rings_not_just_ctrl(void) {
    hype_e1000_dev_t d;

    bring_up(&d);
    CHECK_TRUE("transmit is ready before the reset", hype_e1000_dev_tx_ready(&d));

    wr(&d, HYPE_E1000_REG_CTRL, HYPE_E1000_CTRL_RST);
    CHECK_HEX("the ring base is gone", 0, rd(&d, HYPE_E1000_REG_TDBAL));
    CHECK_HEX("the ring length is gone", 0, rd(&d, HYPE_E1000_REG_TDLEN));
    CHECK_HEX("the transmitter is disabled", 0, rd(&d, HYPE_E1000_REG_TCTL));
    CHECK_HEX("interrupts are masked again", 0, rd(&d, HYPE_E1000_REG_IMS));
    CHECK_TRUE("...so nothing is ready", !hype_e1000_dev_tx_ready(&d));

    /* RST is self-clearing: it must never read back set, or a driver polling for it to clear hangs. */
    CHECK_HEX("RST does not read back", 0, rd(&d, HYPE_E1000_REG_CTRL) & HYPE_E1000_CTRL_RST);

    /* Identity survives: a card whose address changed under a driver reset would look swapped. */
    {
        hype_e1000_mac_t m;
        hype_e1000_mac_from_ral_rah(rd(&d, HYPE_E1000_REG_RAL0), rd(&d, HYPE_E1000_REG_RAH0), &m);
        CHECK_TRUE("the MAC survived the reset", m.valid);
        CHECK_HEX("...unchanged", MAC[0], m.addr[0]);
        CHECK_HEX("...to the last octet", MAC[5], m.addr[5]);
    }
}

static void test_readiness_requires_every_step(void) {
    hype_e1000_dev_t d;

    bring_up(&d);
    CHECK_TRUE("transmit ready", hype_e1000_dev_tx_ready(&d));
    CHECK_TRUE("receive ready", hype_e1000_dev_rx_ready(&d));

    d.bus_master = 0;
    CHECK_TRUE("bus mastering off means the device cannot reach the ring",
               !hype_e1000_dev_tx_ready(&d));
    CHECK_TRUE("...either ring", !hype_e1000_dev_rx_ready(&d));
    d.bus_master = 1;

    wr(&d, HYPE_E1000_REG_TCTL, 0u);
    CHECK_TRUE("a disabled transmitter is not ready", !hype_e1000_dev_tx_ready(&d));
    CHECK_TRUE("...and the receiver is unaffected", hype_e1000_dev_rx_ready(&d));
    wr(&d, HYPE_E1000_REG_TCTL, HYPE_E1000_TCTL_EN);

    /* A base address of 0 is refused: a ring at guest-physical 0 cannot be told apart from a driver
     * that has not published one, and walking it would read the guest's first page. */
    wr(&d, HYPE_E1000_REG_TDBAL, 0u);
    CHECK_TRUE("a zero ring base is not ready", !hype_e1000_dev_tx_ready(&d));
    wr(&d, HYPE_E1000_REG_TDBAL, 0x00200000u);

    wr(&d, HYPE_E1000_REG_TDLEN, 0u);
    CHECK_TRUE("a zero-length ring is not ready", !hype_e1000_dev_tx_ready(&d));

    CHECK_TRUE("a null device is never ready", !hype_e1000_dev_tx_ready(0));
    CHECK_TRUE("...either way", !hype_e1000_dev_rx_ready(0));
}

/*
 * RDLEN/TDLEN must be a whole number of descriptors. Rounding it would make the device and the
 * driver disagree about where the ring ends, and the disagreement shows up as one corrupt packet
 * every time the ring wraps.
 */
static void test_ring_length_must_be_whole_descriptors(void) {
    hype_e1000_dev_t d;

    hype_e1000_dev_reset(&d, MAC);
    wr(&d, HYPE_E1000_REG_TDLEN, 32u * HYPE_E1000_DESC_BYTES);
    CHECK_HEX("512 bytes is 32 descriptors", 32, hype_e1000_dev_tx_count(&d));

    /* The hardware requires 128-byte alignment, so the write masks the low bits -- and what is left
     * is checked for being a whole number of descriptors regardless. */
    wr(&d, HYPE_E1000_REG_TDLEN, 100u);
    CHECK_HEX("a sub-128-byte length masks to 0 and is refused", 0, hype_e1000_dev_tx_count(&d));

    wr(&d, HYPE_E1000_REG_RDLEN, 128u);
    CHECK_HEX("128 bytes is 8 descriptors", 8, hype_e1000_dev_rx_count(&d));
    CHECK_HEX("a null device has no descriptors", 0, hype_e1000_dev_rx_count(0));
    CHECK_HEX("...either ring", 0, hype_e1000_dev_tx_count(0));
}

static void test_access_width_and_bounds(void) {
    hype_e1000_dev_t d;
    uint32_t v = 0;

    hype_e1000_dev_reset(&d, MAC);
    /* Every register in this part is 32 bits, so a narrower access is a driver bug and answering it
     * would hide the bug. */
    CHECK_HEX("a 1-byte read is refused", -1,
              hype_e1000_dev_reg_read(&d, HYPE_E1000_REG_STATUS, 1, &v));
    CHECK_HEX("a 2-byte read is refused", -1,
              hype_e1000_dev_reg_read(&d, HYPE_E1000_REG_STATUS, 2, &v));
    CHECK_HEX("a 1-byte write is refused", -1,
              hype_e1000_dev_reg_write(&d, HYPE_E1000_REG_CTRL, 1, 0));
    CHECK_HEX("an unaligned offset is refused", -1,
              hype_e1000_dev_reg_read(&d, HYPE_E1000_REG_STATUS + 1u, 4, &v));
    CHECK_HEX("past the BAR is refused", -1,
              hype_e1000_dev_reg_read(&d, HYPE_E1000_DEV_BAR_SIZE, 4, &v));
    CHECK_HEX("...for writes too", -1,
              hype_e1000_dev_reg_write(&d, HYPE_E1000_DEV_BAR_SIZE, 4, 0));
    CHECK_HEX("a null device read is refused", -1,
              hype_e1000_dev_reg_read(0, HYPE_E1000_REG_STATUS, 4, &v));
    CHECK_HEX("a null out pointer is refused", -1,
              hype_e1000_dev_reg_read(&d, HYPE_E1000_REG_STATUS, 4, 0));
    CHECK_HEX("a null device write is refused", -1,
              hype_e1000_dev_reg_write(0, HYPE_E1000_REG_CTRL, 4, 0));

    /* An unmodelled register inside the BAR reads 0 and absorbs writes -- the part's own behaviour
     * for reserved space, and what lets a real driver probe the window without faulting. */
    CHECK_HEX("an unmodelled register reads 0", 0, rd(&d, HYPE_E1000_REG_TIPG));
    wr(&d, HYPE_E1000_REG_TIPG, 0x00602008u);
    CHECK_HEX("...and still reads 0 after a write", 0, rd(&d, HYPE_E1000_REG_TIPG));
    /* The statistics block, which drivers read wholesale during init. */
    CHECK_HEX("a statistics register reads 0", 0, rd(&d, 0x4000u));
}

static void test_unusable_macs_are_refused(void) {
    hype_e1000_dev_t d;
    const uint8_t zero[6] = {0, 0, 0, 0, 0, 0};
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t mcast[6] = {0x01, 0x00, 0x5E, 0x01, 0x02, 0x03};

    hype_e1000_dev_reset(&d, MAC);
    CHECK_HEX("all-zero refused", -1, hype_e1000_dev_set_mac(&d, zero));
    CHECK_HEX("broadcast refused", -1, hype_e1000_dev_set_mac(&d, bcast));
    CHECK_HEX("multicast refused -- not a unicast sender", -1, hype_e1000_dev_set_mac(&d, mcast));
    CHECK_HEX("a null address refused", -1, hype_e1000_dev_set_mac(&d, 0));
    CHECK_HEX("a null device refused", -1, hype_e1000_dev_set_mac(0, MAC));
    CHECK_HEX("the previous address is kept", MAC[0], d.mac[0]);

    /* A reset with no MAC keeps what is there rather than zeroing it: an all-zero address would be
     * worse than a stale one. */
    hype_e1000_dev_reset(&d, 0);
    CHECK_HEX("reset with no MAC keeps the address", MAC[5], d.mac[5]);
    hype_e1000_dev_reset(0, MAC); /* must not fault */
    hype_e1000_dev_set_bus_master(0, 1);
    hype_e1000_dev_set_bus_master(&d, 0);
    CHECK_HEX("bus master cleared", 0, d.bus_master);
    hype_e1000_dev_set_bus_master(&d, 7);
    CHECK_HEX("any nonzero enables", 1, d.bus_master);
}

/* An EEPROM word beyond the array reads 0 rather than indexing past it. */
static void test_eeprom_out_of_range(void) {
    hype_e1000_dev_t d;

    hype_e1000_dev_reset(&d, MAC);
    wr(&d, HYPE_E1000_REG_EERD, 0x3Fu << HYPE_E1000_EERD_ADDR_SHIFT);
    CHECK_HEX("the last in-range word reads 0 data", 0,
              rd(&d, HYPE_E1000_REG_EERD) >> HYPE_E1000_EERD_DATA_SHIFT);
    wr(&d, HYPE_E1000_REG_EERD, 0x400u << HYPE_E1000_EERD_ADDR_SHIFT);
    CHECK_HEX("an out-of-range word reads 0", 0,
              rd(&d, HYPE_E1000_REG_EERD) >> HYPE_E1000_EERD_DATA_SHIFT);
    CHECK_TRUE("and still reports DONE, so a driver does not hang",
               (rd(&d, HYPE_E1000_REG_EERD) & HYPE_E1000_EERD_DONE) != 0u);
}

/* The ring pointers a driver programs must read back, including the high halves of the base
 * addresses -- a ring above 4 GB whose high half was dropped points somewhere else entirely. */
static void test_ring_pointers_round_trip(void) {
    hype_e1000_dev_t d;

    hype_e1000_dev_reset(&d, MAC);
    wr(&d, HYPE_E1000_REG_RDBAH, 0x00000002u);
    wr(&d, HYPE_E1000_REG_RDBAL, 0x40001000u);
    CHECK_HEX("receive base high", 0x00000002u, rd(&d, HYPE_E1000_REG_RDBAH));
    CHECK_HEX("receive base low", 0x40001000u, rd(&d, HYPE_E1000_REG_RDBAL));
    wr(&d, HYPE_E1000_REG_TDBAH, 0x00000003u);
    CHECK_HEX("transmit base high", 0x00000003u, rd(&d, HYPE_E1000_REG_TDBAH));

    /* The low 4 bits of a base address are reserved and masked off: a driver writing an unaligned
     * base gets the aligned one back, which is the part's behaviour. */
    wr(&d, HYPE_E1000_REG_RDBAL, 0x4000100Fu);
    CHECK_HEX("the base is 16-byte aligned", 0x40001000u, rd(&d, HYPE_E1000_REG_RDBAL));

    wr(&d, HYPE_E1000_REG_RDT, 31u);
    CHECK_HEX("the tail reads back", 31u, rd(&d, HYPE_E1000_REG_RDT));
    wr(&d, HYPE_E1000_REG_RDH, 5u);
    CHECK_HEX("the head reads back -- a driver zeroes both during init", 5u,
              rd(&d, HYPE_E1000_REG_RDH));
    wr(&d, HYPE_E1000_REG_TDT, 7u);
    CHECK_HEX("transmit tail reads back", 7u, rd(&d, HYPE_E1000_REG_TDT));
    wr(&d, HYPE_E1000_REG_TDH, 3u);
    CHECK_HEX("transmit head reads back", 3u, rd(&d, HYPE_E1000_REG_TDH));
    CHECK_HEX("CTRL reads back what was written", HYPE_E1000_CTRL_SLU,
              (wr(&d, HYPE_E1000_REG_CTRL, HYPE_E1000_CTRL_SLU), rd(&d, HYPE_E1000_REG_CTRL)));
}

int main(void) {
    test_link_is_always_up();
    test_mac_readable_two_ways_and_they_agree();
    test_ims_sets_and_imc_clears();
    test_icr_is_read_to_clear_and_write_to_clear();
    test_reset_clears_the_rings_not_just_ctrl();
    test_readiness_requires_every_step();
    test_ring_length_must_be_whole_descriptors();
    test_access_width_and_bounds();
    test_unusable_macs_are_refused();
    test_eeprom_out_of_range();
    test_ring_pointers_round_trip();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
