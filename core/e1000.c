#include "e1000.h"

/* RAH bit 31: Address Valid. */
#define E1000_RAH_AV (1u << 31)

static int mac_is_degenerate(const uint8_t a[6]) {
    unsigned i;
    int all_zero = 1, all_ones = 1;
    for (i = 0; i < 6u; i++) {
        if (a[i] != 0x00u) all_zero = 0;
        if (a[i] != 0xFFu) all_ones = 0;
    }
    return all_zero || all_ones;
}

void hype_e1000_mac_from_ral_rah(uint32_t ral, uint32_t rah, hype_e1000_mac_t *out) {
    if (out == 0) {
        return;
    }
    out->addr[0] = (uint8_t)(ral & 0xFFu);
    out->addr[1] = (uint8_t)((ral >> 8) & 0xFFu);
    out->addr[2] = (uint8_t)((ral >> 16) & 0xFFu);
    out->addr[3] = (uint8_t)((ral >> 24) & 0xFFu);
    out->addr[4] = (uint8_t)(rah & 0xFFu);
    out->addr[5] = (uint8_t)((rah >> 8) & 0xFFu);
    /*
     * Both conditions matter. AV clear means the hardware itself does not consider the entry
     * populated; all-zero or all-ones means it does but the value is unusable. Transmitting from
     * either produces a fault that looks like a cable or switch problem rather than a driver one.
     */
    out->valid = ((rah & E1000_RAH_AV) != 0u && !mac_is_degenerate(out->addr)) ? 1 : 0;
}

void hype_e1000_mac_from_eeprom(uint16_t w0, uint16_t w1, uint16_t w2, hype_e1000_mac_t *out) {
    if (out == 0) {
        return;
    }
    /* LOW byte of each word first -- the opposite of how the words read as numbers, which is the
     * classic way to end up with a byte-swapped MAC that ARPs to nobody. */
    out->addr[0] = (uint8_t)(w0 & 0xFFu);
    out->addr[1] = (uint8_t)((w0 >> 8) & 0xFFu);
    out->addr[2] = (uint8_t)(w1 & 0xFFu);
    out->addr[3] = (uint8_t)((w1 >> 8) & 0xFFu);
    out->addr[4] = (uint8_t)(w2 & 0xFFu);
    out->addr[5] = (uint8_t)((w2 >> 8) & 0xFFu);
    out->valid = mac_is_degenerate(out->addr) ? 0 : 1;
}

unsigned int hype_e1000_ring_next(unsigned int index, unsigned int size) {
    if (size == 0u) {
        return 0u;
    }
    return (index + 1u) % size;
}

int hype_e1000_ring_full(unsigned int head, unsigned int tail, unsigned int size) {
    if (size == 0u) {
        return 1; /* a zero-length ring can hold nothing; never report space in it */
    }
    /*
     * tail == head encodes EMPTY, so the ring is full one slot early. Reporting space here is how
     * a driver overwrites a descriptor the hardware has not read yet -- the frame is "sent" and
     * never appears on the wire.
     */
    return hype_e1000_ring_next(tail, size) == (head % size);
}

unsigned int hype_e1000_ring_used(unsigned int head, unsigned int tail, unsigned int size) {
    if (size == 0u) {
        return 0u;
    }
    return (tail + size - (head % size)) % size;
}

int hype_e1000_txd_done(uint8_t status) { return (status & HYPE_E1000_TXD_STA_DD) != 0u; }

int hype_e1000_rxd_done(uint8_t status) { return (status & HYPE_E1000_RXD_STA_DD) != 0u; }

uint32_t hype_e1000_ring_len_bytes(unsigned int descriptors) {
    uint32_t bytes = (uint32_t)descriptors * HYPE_E1000_DESC_BYTES;
    if (descriptors == 0u || (bytes % 128u) != 0u) {
        return 0u; /* the hardware requires a 128-byte-aligned length; refuse rather than round */
    }
    return bytes;
}
