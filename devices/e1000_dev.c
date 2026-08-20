#include "e1000_dev.h"

/* The EEPROM words a driver reads to find the MAC when RAL/RAH are empty. Words 0..2 hold the
 * address, low byte first within each word (manual 5.6). hype fills RAL/RAH too, so a driver that
 * reads either way gets the same answer -- a real part does the same, and a driver may use either. */
#define EEPROM_WORD_MAC0 0x00u
#define EEPROM_WORD_MAC1 0x01u
#define EEPROM_WORD_MAC2 0x02u
#define EEPROM_WORD_COUNT 0x40u

static int mac_usable(const uint8_t *mac) {
    unsigned int i;
    unsigned int zeros = 0;
    unsigned int ones = 0;

    for (i = 0; i < 6u; i++) {
        if (mac[i] == 0x00u) {
            zeros++;
        }
        if (mac[i] == 0xFFu) {
            ones++;
        }
    }
    /* All-zero is "no address" and all-ones is broadcast; neither is a legal SOURCE address, so a
     * guest given one transmits frames the first switch discards. Same refusal as
     * devices/virtio_net.c and core/e1000.c, deliberately: a bad address is worth refusing at every
     * point it can enter, because the symptom downstream is "the network does not work" with
     * nothing pointing at the address. */
    if (zeros == 6u || ones == 6u) {
        return 0;
    }
    /* Bit 0 of octet 0 is the group bit: a multicast address is not a unicast sender. */
    if ((mac[0] & 0x01u) != 0u) {
        return 0;
    }
    return 1;
}

int hype_e1000_dev_set_mac(hype_e1000_dev_t *dev, const uint8_t *mac) {
    unsigned int i;

    if (dev == 0 || mac == 0 || !mac_usable(mac)) {
        return -1;
    }
    for (i = 0; i < 6u; i++) {
        dev->mac[i] = mac[i];
    }
    return 0;
}

/* Everything a driver negotiates, cleared. NOT the MAC -- see the header. */
static void reset_registers(hype_e1000_dev_t *dev) {
    dev->ctrl = 0;
    dev->rctl = 0;
    dev->tctl = 0;
    dev->ims = 0;
    dev->icr = 0;
    dev->rdbal = 0;
    dev->rdbah = 0;
    dev->rdlen = 0;
    dev->rdh = 0;
    dev->rdt = 0;
    dev->tdbal = 0;
    dev->tdbah = 0;
    dev->tdlen = 0;
    dev->tdh = 0;
    dev->tdt = 0;
    dev->eerd = 0;
}

void hype_e1000_dev_reset(hype_e1000_dev_t *dev, const uint8_t *mac) {
    if (dev == 0) {
        return;
    }
    reset_registers(dev);
    if (mac != 0) {
        (void)hype_e1000_dev_set_mac(dev, mac);
    }
    /* #372's permissive default, as for every other model here: the microtests drive this with no
     * PCI at all, so a device that started with bus mastering off would be inert in exactly the
     * environment built to test it. The live path mirrors the guest's real bit in. */
    dev->bus_master = 1;
}

void hype_e1000_dev_set_bus_master(hype_e1000_dev_t *dev, int enabled) {
    if (dev == 0) {
        return;
    }
    dev->bus_master = (enabled != 0) ? 1 : 0;
}

static uint16_t eeprom_word(const hype_e1000_dev_t *dev, unsigned int addr) {
    /* The MAC, low byte first within each 16-bit word. Getting the byte order wrong here produces a
     * guest whose address is a byte-swapped version of the one hype's forwarding plane expects,
     * which looks like a completely different guest. */
    if (addr == EEPROM_WORD_MAC0) {
        return (uint16_t)((uint16_t)dev->mac[0] | ((uint16_t)dev->mac[1] << 8));
    }
    if (addr == EEPROM_WORD_MAC1) {
        return (uint16_t)((uint16_t)dev->mac[2] | ((uint16_t)dev->mac[3] << 8));
    }
    if (addr == EEPROM_WORD_MAC2) {
        return (uint16_t)((uint16_t)dev->mac[4] | ((uint16_t)dev->mac[5] << 8));
    }
    /* Every other word reads 0. A real part has a checksum word and configuration hype does not
     * model; a driver that validates the checksum will find it wrong, which is why RAL/RAH are also
     * populated -- a driver has two ways to learn the address and only needs one. */
    return 0u;
}

int hype_e1000_dev_reg_read(hype_e1000_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                           uint32_t *out_value) {
    if (dev == 0 || out_value == 0) {
        return -1;
    }
    if (size_bytes != 4u) {
        /* Every register in this part is 32 bits. A narrower access is a driver bug, and answering
         * it would hide the bug rather than fix it. */
        return -1;
    }
    if (offset >= HYPE_E1000_DEV_BAR_SIZE || (offset & 3u) != 0u) {
        return -1;
    }

    switch (offset) {
        case HYPE_E1000_REG_CTRL:
            /* RST is self-clearing, so it never reads back set. */
            *out_value = dev->ctrl & ~HYPE_E1000_CTRL_RST;
            return 0;
        case HYPE_E1000_REG_STATUS:
            /*
             * LINK IS ALWAYS UP, FULL DUPLEX, 1 Gb/s. A driver waits for LU before it will transmit,
             * so reporting anything else means a guest that never sends a packet. hype's own uplink
             * may be down, and that is deliberately not reflected here: the guest's link is to hype,
             * which is up whenever the VM is running. Reporting the UPLINK's state on the guest's
             * link would make a guest stop transmitting because of something on the other side of a
             * router, which no real network does.
             */
            *out_value = HYPE_E1000_STATUS_LU | HYPE_E1000_STATUS_FD | HYPE_E1000_STATUS_SPEED_1000;
            return 0;
        case HYPE_E1000_REG_EECD:
            /* EE_PRES: an EEPROM is present. Drivers check this before trying to read one. */
            *out_value = (1u << 8);
            return 0;
        case HYPE_E1000_REG_EERD: {
            unsigned int addr = (dev->eerd >> HYPE_E1000_EERD_ADDR_SHIFT) & 0xFFFFu;
            uint16_t w = (addr < EEPROM_WORD_COUNT) ? eeprom_word(dev, addr) : 0u;
            /* DONE is set on read rather than after a delay: the read completed the moment the
             * driver asked, and a model that made a driver poll would only be simulating latency. */
            *out_value = (dev->eerd & 0xFFFFu) | HYPE_E1000_EERD_DONE |
                         ((uint32_t)w << HYPE_E1000_EERD_DATA_SHIFT);
            return 0;
        }
        case HYPE_E1000_REG_ICR:
            /* READ-TO-CLEAR (manual 13.4.17). A driver's ISR reads this to find out why it was
             * interrupted, and the read is what acknowledges it. */
            *out_value = dev->icr;
            dev->icr = 0;
            return 0;
        case HYPE_E1000_REG_IMS:
            *out_value = dev->ims;
            return 0;
        case HYPE_E1000_REG_RCTL:
            *out_value = dev->rctl;
            return 0;
        case HYPE_E1000_REG_TCTL:
            *out_value = dev->tctl;
            return 0;
        case HYPE_E1000_REG_RDBAL:
            *out_value = dev->rdbal;
            return 0;
        case HYPE_E1000_REG_RDBAH:
            *out_value = dev->rdbah;
            return 0;
        case HYPE_E1000_REG_RDLEN:
            *out_value = dev->rdlen;
            return 0;
        case HYPE_E1000_REG_RDH:
            *out_value = dev->rdh;
            return 0;
        case HYPE_E1000_REG_RDT:
            *out_value = dev->rdt;
            return 0;
        case HYPE_E1000_REG_TDBAL:
            *out_value = dev->tdbal;
            return 0;
        case HYPE_E1000_REG_TDBAH:
            *out_value = dev->tdbah;
            return 0;
        case HYPE_E1000_REG_TDLEN:
            *out_value = dev->tdlen;
            return 0;
        case HYPE_E1000_REG_TDH:
            *out_value = dev->tdh;
            return 0;
        case HYPE_E1000_REG_TDT:
            *out_value = dev->tdt;
            return 0;
        case HYPE_E1000_REG_RAL0:
            *out_value = (uint32_t)dev->mac[0] | ((uint32_t)dev->mac[1] << 8) |
                         ((uint32_t)dev->mac[2] << 16) | ((uint32_t)dev->mac[3] << 24);
            return 0;
        case HYPE_E1000_REG_RAH0:
            /* AV (Address Valid) in bit 31, which is what tells a driver the low bits mean
             * anything. core/e1000.c's own decode refuses an RAH with AV clear, so omitting it here
             * would make hype's host driver reject hype's guest device. */
            *out_value = (uint32_t)dev->mac[4] | ((uint32_t)dev->mac[5] << 8) | (1u << 31);
            return 0;
        default:
            /* Reserved or unmodelled: reads 0. This is the part's own behaviour and it is what lets
             * a real driver probe the whole window without faulting. */
            *out_value = 0u;
            return 0;
    }
}

int hype_e1000_dev_reg_write(hype_e1000_dev_t *dev, uint32_t offset, uint8_t size_bytes,
                            uint32_t value) {
    if (dev == 0 || size_bytes != 4u) {
        return -1;
    }
    if (offset >= HYPE_E1000_DEV_BAR_SIZE || (offset & 3u) != 0u) {
        return -1;
    }

    switch (offset) {
        case HYPE_E1000_REG_CTRL:
            if ((value & HYPE_E1000_CTRL_RST) != 0u) {
                /*
                 * A device reset clears everything the driver negotiated -- not just CTRL. A model
                 * that only cleared this register would leave a driver reinitialising a device that
                 * still had its old ring addresses, which is the shape of a bug that appears only on
                 * the second driver load.
                 */
                reset_registers(dev);
                return 0;
            }
            dev->ctrl = value;
            return 0;
        case HYPE_E1000_REG_RCTL:
            dev->rctl = value;
            return 0;
        case HYPE_E1000_REG_TCTL:
            dev->tctl = value;
            return 0;
        case HYPE_E1000_REG_IMS:
            /* IMS is SET-only: writing a 1 enables that cause, writing a 0 changes nothing. IMC is
             * how a driver disables one. Treating IMS as an assignment would silently disable every
             * cause the driver did not mention in its latest write. */
            dev->ims |= value;
            return 0;
        case HYPE_E1000_REG_IMC:
            dev->ims &= ~value;
            return 0;
        case HYPE_E1000_REG_ICR:
            /* Writing a 1 clears that cause, in addition to the read-to-clear behaviour. Drivers do
             * both, depending on the vintage. */
            dev->icr &= ~value;
            return 0;
        case HYPE_E1000_REG_EERD:
            dev->eerd = value;
            return 0;
        case HYPE_E1000_REG_RDBAL:
            dev->rdbal = value & ~0xFu; /* 16-byte aligned, low bits are reserved */
            return 0;
        case HYPE_E1000_REG_RDBAH:
            dev->rdbah = value;
            return 0;
        case HYPE_E1000_REG_RDLEN:
            dev->rdlen = value & ~0x7Fu; /* 128-byte aligned, per the manual */
            return 0;
        case HYPE_E1000_REG_RDH:
            /* Hardware-owned, but writable while the receiver is disabled -- a driver zeroes both
             * pointers during init, and refusing the write would leave the ring misaligned with the
             * driver's own idea of it. */
            dev->rdh = value & 0xFFFFu;
            return 0;
        case HYPE_E1000_REG_RDT:
            dev->rdt = value & 0xFFFFu;
            return 0;
        case HYPE_E1000_REG_TDBAL:
            dev->tdbal = value & ~0xFu;
            return 0;
        case HYPE_E1000_REG_TDBAH:
            dev->tdbah = value;
            return 0;
        case HYPE_E1000_REG_TDLEN:
            dev->tdlen = value & ~0x7Fu;
            return 0;
        case HYPE_E1000_REG_TDH:
            dev->tdh = value & 0xFFFFu;
            return 0;
        case HYPE_E1000_REG_TDT:
            dev->tdt = value & 0xFFFFu;
            return 0;
        default:
            /* Reserved, or a register hype does not model (TIPG, the multicast table, the statistics
             * block). Absorbed, because a driver writes all of them during init and faulting on one
             * would stop a correct driver dead. */
            return 0;
    }
}

int hype_e1000_dev_raise(hype_e1000_dev_t *dev, uint32_t cause) {
    if (dev == 0) {
        return 0;
    }
    dev->icr |= cause;
    return ((dev->icr & dev->ims) != 0u) ? 1 : 0;
}

int hype_e1000_dev_irq_pending(const hype_e1000_dev_t *dev) {
    if (dev == 0) {
        return 0;
    }
    return ((dev->icr & dev->ims) != 0u) ? 1 : 0;
}

unsigned int hype_e1000_dev_rx_count(const hype_e1000_dev_t *dev) {
    if (dev == 0 || dev->rdlen == 0u || (dev->rdlen % HYPE_E1000_DESC_BYTES) != 0u) {
        return 0u;
    }
    return dev->rdlen / HYPE_E1000_DESC_BYTES;
}

unsigned int hype_e1000_dev_tx_count(const hype_e1000_dev_t *dev) {
    if (dev == 0 || dev->tdlen == 0u || (dev->tdlen % HYPE_E1000_DESC_BYTES) != 0u) {
        return 0u;
    }
    return dev->tdlen / HYPE_E1000_DESC_BYTES;
}

int hype_e1000_dev_tx_ready(const hype_e1000_dev_t *dev) {
    if (dev == 0) {
        return 0;
    }
    /* Every clause is something the driver does. A base address of 0 is refused because a ring at
     * guest-physical 0 is indistinguishable from a driver that has not published one yet, and
     * walking it would be reading the guest's first page. */
    return (dev->tctl & HYPE_E1000_TCTL_EN) != 0u && hype_e1000_dev_tx_count(dev) != 0u &&
           (dev->tdbal != 0u || dev->tdbah != 0u) && dev->bus_master != 0;
}

int hype_e1000_dev_rx_ready(const hype_e1000_dev_t *dev) {
    if (dev == 0) {
        return 0;
    }
    return (dev->rctl & HYPE_E1000_RCTL_EN) != 0u && hype_e1000_dev_rx_count(dev) != 0u &&
           (dev->rdbal != 0u || dev->rdbah != 0u) && dev->bus_master != 0;
}
