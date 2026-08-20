#include <stdio.h>
#include "../e1000.h"

static int failures;

#define CHECK(desc, cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", (desc));                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        if ((long long)(expected) != (long long)(actual)) {                                        \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected),            \
                   (long long)(actual));                                                           \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* QEMU's default e1000 MAC is 52:54:00:12:34:56 -- a real value to decode rather than a made-up one. */
static void test_mac_from_ral_rah(void) {
    hype_e1000_mac_t m;

    /* RAL = bytes 0..3 little-endian, RAH = bytes 4..5 plus AV. */
    hype_e1000_mac_from_ral_rah(0x00545452u, 0x80005634u, &m);
    CHECK("a populated RAL/RAH is valid", m.valid == 1);
    CHECK_INT("byte 0", 0x52, m.addr[0]);
    CHECK_INT("byte 1", 0x54, m.addr[1]);
    CHECK_INT("byte 2", 0x54, m.addr[2]);
    CHECK_INT("byte 3", 0x00, m.addr[3]);
    CHECK_INT("byte 4", 0x34, m.addr[4]);
    CHECK_INT("byte 5", 0x56, m.addr[5]);

    /* AV clear: the hardware says the entry is not populated, whatever the bytes look like. */
    hype_e1000_mac_from_ral_rah(0x00545452u, 0x00005634u, &m);
    CHECK("AV clear is refused", m.valid == 0);

    /* Populated but unusable. Both directions, because a NIC with no EEPROM reads one or the
     * other and transmitting from either looks like a cable fault, not a driver bug. */
    hype_e1000_mac_from_ral_rah(0x00000000u, 0x80000000u, &m);
    CHECK("an all-zero MAC is refused", m.valid == 0);
    hype_e1000_mac_from_ral_rah(0xFFFFFFFFu, 0x8000FFFFu, &m);
    CHECK("an all-ones MAC is refused", m.valid == 0);
}

static void test_mac_from_eeprom(void) {
    hype_e1000_mac_t m;

    /* Words hold two bytes each, LOW byte first: 0x5452 -> 52,54. */
    hype_e1000_mac_from_eeprom(0x5452u, 0x0054u, 0x5634u, &m);
    CHECK("an EEPROM MAC is valid", m.valid == 1);
    CHECK_INT("byte 0", 0x52, m.addr[0]);
    CHECK_INT("byte 1", 0x54, m.addr[1]);
    CHECK_INT("byte 2", 0x54, m.addr[2]);
    CHECK_INT("byte 3", 0x00, m.addr[3]);
    CHECK_INT("byte 4", 0x34, m.addr[4]);
    CHECK_INT("byte 5", 0x56, m.addr[5]);

    hype_e1000_mac_from_eeprom(0u, 0u, 0u, &m);
    CHECK("an all-zero EEPROM MAC is refused", m.valid == 0);
    hype_e1000_mac_from_eeprom(0xFFFFu, 0xFFFFu, 0xFFFFu, &m);
    CHECK("an all-ones EEPROM MAC is refused", m.valid == 0);
}

/*
 * The ring boundary is where this class of driver actually fails -- not on the first frame, but
 * after `size` of them, under load, once.
 */
static void test_ring_wrap_and_full(void) {
    CHECK_INT("next wraps at the end", 0, hype_e1000_ring_next(31u, 32u));
    CHECK_INT("next advances inside", 5, hype_e1000_ring_next(4u, 32u));
    CHECK_INT("a zero-size ring cannot advance", 0, hype_e1000_ring_next(7u, 0u));

    /* Empty: tail == head. Not full. */
    CHECK("an empty ring is not full", !hype_e1000_ring_full(0u, 0u, 32u));
    CHECK_INT("an empty ring uses nothing", 0, hype_e1000_ring_used(0u, 0u, 32u));

    /* One slot left before tail would meet head: THAT is full, one early. */
    CHECK("full one slot early", hype_e1000_ring_full(0u, 31u, 32u));
    CHECK("not full two slots early", !hype_e1000_ring_full(0u, 30u, 32u));

    /* Across the wrap: head 5, tail 4 -- advancing tail hits head. */
    CHECK("full across the wrap", hype_e1000_ring_full(5u, 4u, 32u));
    CHECK_INT("used across the wrap", 31, hype_e1000_ring_used(5u, 4u, 32u));

    /* A zero-length ring must never report space, or a caller writes descriptor 0 forever. */
    CHECK("a zero-size ring is always full", hype_e1000_ring_full(0u, 0u, 0u));
    CHECK_INT("a zero-size ring uses nothing", 0, hype_e1000_ring_used(0u, 0u, 0u));
}

static void test_descriptor_done_bits(void) {
    CHECK("TX DD set is done", hype_e1000_txd_done(0x01u));
    CHECK("TX DD clear is not done", !hype_e1000_txd_done(0x00u));
    CHECK("TX other bits alone are not done", !hype_e1000_txd_done(0x0Eu));
    CHECK("RX DD set is done", hype_e1000_rxd_done(0x01u));
    CHECK("RX DD clear is not done", !hype_e1000_rxd_done(0x02u));
}

/*
 * RDLEN/TDLEN is a length in BYTES and the hardware requires it 128-byte aligned. Refusing an
 * unrepresentable count is the point: rounding it would program a ring of a different size than
 * the driver believes it has, which desynchronises head and tail permanently.
 */
static void test_ring_len_bytes(void) {
    CHECK_INT("32 descriptors is 512 bytes", 512, hype_e1000_ring_len_bytes(32u));
    CHECK_INT("8 descriptors is the minimum aligned size", 128, hype_e1000_ring_len_bytes(8u));
    CHECK_INT("zero descriptors is refused", 0, hype_e1000_ring_len_bytes(0u));
    CHECK_INT("a non-multiple-of-8 count is refused", 0, hype_e1000_ring_len_bytes(10u));
    CHECK_INT("7 is refused", 0, hype_e1000_ring_len_bytes(7u));
}

int main(void) {
    test_mac_from_ral_rah();
    test_mac_from_eeprom();
    test_ring_wrap_and_full();
    test_descriptor_done_bits();
    test_ring_len_bytes();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
