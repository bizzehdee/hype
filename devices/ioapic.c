#include "ioapic.h"

void hype_ioapic_reset(hype_ioapic_t *io) {
    uint32_t i;
    io->ioregsel = 0;
    io->id = 0;
    for (i = 0; i < HYPE_IOAPIC_NUM_RTES; i++) {
        /* 82093AA reset: every entry masked, all other fields clear. */
        io->rte[i] = (uint64_t)HYPE_IOAPIC_RTE_MASK;
    }
}

int hype_ioapic_mmio_read(const hype_ioapic_t *io, uint32_t offset, uint32_t *out) {
    if (offset == HYPE_IOAPIC_REG_IOREGSEL) {
        *out = io->ioregsel;
        return 0;
    }
    if (offset == HYPE_IOAPIC_REG_IOWIN) {
        uint32_t index = io->ioregsel & 0xFFu;
        if (index == HYPE_IOAPIC_INDEX_ID) {
            *out = (io->id & 0x0Fu) << 24;
        } else if (index == HYPE_IOAPIC_INDEX_VER) {
            /* version in [7:0], max redirection entry (count-1) in [23:16]. */
            *out = (uint32_t)HYPE_IOAPIC_VERSION | ((HYPE_IOAPIC_NUM_RTES - 1u) << 16);
        } else if (index == HYPE_IOAPIC_INDEX_ARB) {
            *out = (io->id & 0x0Fu) << 24;
        } else if (index >= HYPE_IOAPIC_INDEX_REDIR_BASE &&
                   index < HYPE_IOAPIC_INDEX_REDIR_BASE + HYPE_IOAPIC_NUM_RTES * 2u) {
            uint32_t rel = index - HYPE_IOAPIC_INDEX_REDIR_BASE;
            uint32_t n = rel / 2u;
            if ((rel & 1u) == 0u) {
                *out = (uint32_t)(io->rte[n] & 0xFFFFFFFFu); /* low dword */
            } else {
                *out = (uint32_t)(io->rte[n] >> 32); /* high dword */
            }
        } else {
            *out = 0; /* unimplemented register reads as 0, like real silicon */
        }
        return 0;
    }
    return -1;
}

int hype_ioapic_mmio_write(hype_ioapic_t *io, uint32_t offset, uint32_t value) {
    if (offset == HYPE_IOAPIC_REG_IOREGSEL) {
        io->ioregsel = value & 0xFFu;
        return 0;
    }
    if (offset == HYPE_IOAPIC_REG_IOWIN) {
        uint32_t index = io->ioregsel & 0xFFu;
        if (index == HYPE_IOAPIC_INDEX_ID) {
            io->id = (value >> 24) & 0x0Fu;
        } else if (index == HYPE_IOAPIC_INDEX_VER || index == HYPE_IOAPIC_INDEX_ARB) {
            /* read-only registers: ignore writes. */
        } else if (index >= HYPE_IOAPIC_INDEX_REDIR_BASE &&
                   index < HYPE_IOAPIC_INDEX_REDIR_BASE + HYPE_IOAPIC_NUM_RTES * 2u) {
            uint32_t rel = index - HYPE_IOAPIC_INDEX_REDIR_BASE;
            uint32_t n = rel / 2u;
            if ((rel & 1u) == 0u) {
                /* Low dword: preserve the read-only Delivery-Status (bit 12)
                 * and Remote-IRR (bit 14) bits the guest must not overwrite. */
                uint32_t ro = (uint32_t)(io->rte[n] & 0xFFFFFFFFu) &
                              (HYPE_IOAPIC_RTE_DELIVERY_STATUS | HYPE_IOAPIC_RTE_REMOTE_IRR);
                uint32_t low = (value & ~(HYPE_IOAPIC_RTE_DELIVERY_STATUS | HYPE_IOAPIC_RTE_REMOTE_IRR)) | ro;
                io->rte[n] = (io->rte[n] & 0xFFFFFFFF00000000ULL) | (uint64_t)low;
            } else {
                io->rte[n] = (io->rte[n] & 0x00000000FFFFFFFFULL) | ((uint64_t)value << 32);
            }
        }
        return 0;
    }
    return -1;
}

int hype_ioapic_raise(hype_ioapic_t *io, uint32_t gsi, uint8_t *out_vector) {
    uint64_t entry;
    uint32_t delmode;

    if (gsi >= HYPE_IOAPIC_NUM_RTES) {
        return 0;
    }
    entry = io->rte[gsi];

    if ((entry & HYPE_IOAPIC_RTE_MASK) != 0) {
        return 0; /* masked -- no delivery */
    }
    delmode = (uint32_t)((entry & HYPE_IOAPIC_RTE_DELMODE_MASK) >> HYPE_IOAPIC_RTE_DELMODE_SHIFT);
    if (delmode != HYPE_IOAPIC_DELMODE_FIXED) {
        return 0; /* only Fixed-mode plain-vector delivery is modeled */
    }

    if ((entry & HYPE_IOAPIC_RTE_TRIGGER_LEVEL) != 0) {
        /* Level-triggered: don't re-inject while the previous assertion is
         * still being serviced (Remote-IRR set until the guest EOIs). */
        if ((entry & HYPE_IOAPIC_RTE_REMOTE_IRR) != 0) {
            return 0;
        }
        io->rte[gsi] = entry | HYPE_IOAPIC_RTE_REMOTE_IRR;
    }
    /* Edge-triggered: every assertion is an edge -> inject each time. */

    *out_vector = (uint8_t)(entry & HYPE_IOAPIC_RTE_VECTOR_MASK);
    return 1;
}

void hype_ioapic_eoi(hype_ioapic_t *io, uint8_t vector) {
    uint32_t i;
    for (i = 0; i < HYPE_IOAPIC_NUM_RTES; i++) {
        if ((io->rte[i] & HYPE_IOAPIC_RTE_TRIGGER_LEVEL) != 0 &&
            (io->rte[i] & HYPE_IOAPIC_RTE_REMOTE_IRR) != 0 &&
            (uint8_t)(io->rte[i] & HYPE_IOAPIC_RTE_VECTOR_MASK) == vector) {
            io->rte[i] &= ~(uint64_t)HYPE_IOAPIC_RTE_REMOTE_IRR;
        }
    }
}
