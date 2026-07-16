#include "ahci.h"

void hype_ahci_reset(hype_ahci_t *ahci) {
    uint8_t *bytes = (uint8_t *)ahci;
    uint32_t i;

    for (i = 0; i < sizeof(*ahci); i++) {
        bytes[i] = 0;
    }

    ahci->cap = (1u << 31) /* S64A: supports 64-bit addressing */
                | (1u << 18); /* SAM: AHCI-only, no legacy IDE emulation */
                              /* NP (bits 4:0) = 0 -> 1 port; NCS (bits 12:8) = 0 -> 1 command slot */
    ahci->pi = 0x1u;          /* port 0 implemented */
    ahci->vs = 0x00010301u;   /* AHCI 1.3.1 */

    ahci->p_sig = HYPE_AHCI_SIG_ATAPI;
    ahci->p_ssts = 0x00000123u; /* IPM=1 (active), SPD=1 (Gen1), DET=3 (present, phy comm established) */
    ahci->p_tfd = 0x00000050u;  /* STATUS = DRDY|DSC, no ERR/BSY/DRQ */
}

int hype_ahci_mmio_read(const hype_ahci_t *ahci, uint32_t offset, uint8_t size_bytes, uint32_t *out_value) {
    if (size_bytes != 4u || (offset & 3u) != 0u) {
        return -1;
    }

    switch (offset) {
        case HYPE_AHCI_REG_CAP: *out_value = ahci->cap; return 0;
        case HYPE_AHCI_REG_GHC: *out_value = ahci->ghc; return 0;
        case HYPE_AHCI_REG_IS: *out_value = ahci->is; return 0;
        case HYPE_AHCI_REG_PI: *out_value = ahci->pi; return 0;
        case HYPE_AHCI_REG_VS: *out_value = ahci->vs; return 0;
        case HYPE_AHCI_REG_CCC_CTL: *out_value = ahci->ccc_ctl; return 0;
        case HYPE_AHCI_REG_CCC_PORTS: *out_value = ahci->ccc_ports; return 0;
        case HYPE_AHCI_REG_EM_LOC: *out_value = ahci->em_loc; return 0;
        case HYPE_AHCI_REG_EM_CTL: *out_value = ahci->em_ctl; return 0;
        case HYPE_AHCI_REG_CAP2: *out_value = ahci->cap2; return 0;
        case HYPE_AHCI_REG_BOHC: *out_value = ahci->bohc; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLB: *out_value = ahci->p_clb; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLBU: *out_value = ahci->p_clbu; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FB: *out_value = ahci->p_fb; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FBU: *out_value = ahci->p_fbu; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS: *out_value = ahci->p_is; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IE: *out_value = ahci->p_ie; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD: *out_value = ahci->p_cmd; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_TFD: *out_value = ahci->p_tfd; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SIG: *out_value = ahci->p_sig; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SSTS: *out_value = ahci->p_ssts; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SCTL: *out_value = ahci->p_sctl; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SERR: *out_value = ahci->p_serr; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SACT: *out_value = ahci->p_sact; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI: *out_value = ahci->p_ci; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SNTF: *out_value = ahci->p_sntf; return 0;
        default:
            if (offset >= HYPE_AHCI_MMIO_SIZE) {
                return -1;
            }
            *out_value = 0; /* reserved/unimplemented-but-in-range register */
            return 0;
    }
}

int hype_ahci_mmio_write(hype_ahci_t *ahci, uint32_t offset, uint8_t size_bytes, uint32_t value) {
    if (size_bytes != 4u || (offset & 3u) != 0u) {
        return -1;
    }

    switch (offset) {
        case HYPE_AHCI_REG_GHC:
            /* GHC.HR (bit 0, HBA Reset) is self-clearing: real hardware
             * performs the reset and clears HR once it completes (AHCI
             * 1.3.1 SS3.1.2). Model the reset as instantaneous -- store
             * every other bit as written (notably AE, bit 31) but always
             * report HR clear, so a real AHCI driver's reset-completion
             * poll (EDK2 AhciReset()) sees HR==0 immediately instead of
             * spinning until timeout and abandoning the controller (the
             * exact stall that kept OVMF's storage stack from ever
             * enumerating the CD-ROM -- FW-1h). */
            ahci->ghc = value & ~(uint32_t)HYPE_AHCI_GHC_HR;
            return 0;
        case HYPE_AHCI_REG_IS: ahci->is &= ~value; return 0; /* RW1C */
        case HYPE_AHCI_REG_CCC_CTL: ahci->ccc_ctl = value; return 0;
        case HYPE_AHCI_REG_EM_CTL: ahci->em_ctl = value; return 0;
        case HYPE_AHCI_REG_BOHC: ahci->bohc = value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLB: ahci->p_clb = value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CLBU: ahci->p_clbu = value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FB: ahci->p_fb = value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_FBU: ahci->p_fbu = value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IS: ahci->p_is &= ~value; return 0; /* RW1C */
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_IE: ahci->p_ie = value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CMD: {
            uint32_t new_cmd = value;
            if (new_cmd & HYPE_AHCI_PCMD_ST) {
                new_cmd |= HYPE_AHCI_PCMD_CR;
            } else {
                new_cmd &= ~(uint32_t)HYPE_AHCI_PCMD_CR;
            }
            if (new_cmd & HYPE_AHCI_PCMD_FRE) {
                new_cmd |= HYPE_AHCI_PCMD_FR;
            } else {
                new_cmd &= ~(uint32_t)HYPE_AHCI_PCMD_FR;
            }
            ahci->p_cmd = new_cmd;
            return 0;
        }
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SCTL: ahci->p_sctl = value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SERR: ahci->p_serr &= ~value; return 0; /* RW1C */
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_CI: ahci->p_ci |= value; return 0;
        case HYPE_AHCI_PORT_BASE + HYPE_AHCI_PREG_SNTF: ahci->p_sntf &= ~value; return 0; /* RW1C */
        default:
            if (offset >= HYPE_AHCI_MMIO_SIZE) {
                return -1;
            }
            return 0; /* write to a read-only/unimplemented-but-in-range register: ignored */
    }
}

int hype_ahci_irq_pending(const hype_ahci_t *ahci) {
    /* AHCI 1.3.1 SS5.5.3: the HBA asserts its interrupt while GHC.IE is
     * set and some port has (PxIS & PxIE) != 0. Single-port model, so
     * only port 0 contributes. Computed from the enabled status bits
     * directly (rather than the latched global IS) so that once the
     * guest's ISR clears PxIS the condition deasserts, matching the
     * hardware level-sensitive line. */
    if ((ahci->ghc & HYPE_AHCI_GHC_IE) == 0) {
        return 0;
    }
    return (ahci->p_is & ahci->p_ie) != 0;
}

void hype_ahci_decode_cmd_header(const uint8_t raw[32], hype_ahci_cmd_header_t *out) {
    uint32_t opts = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) |
                    ((uint32_t)raw[3] << 24);
    uint32_t ctba = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) | ((uint32_t)raw[10] << 16) |
                    ((uint32_t)raw[11] << 24);
    uint32_t ctbau = (uint32_t)raw[12] | ((uint32_t)raw[13] << 8) | ((uint32_t)raw[14] << 16) |
                     ((uint32_t)raw[15] << 24);

    out->cfl = (uint8_t)(opts & 0x1Fu);
    out->is_atapi = (opts & (1u << 5)) != 0;
    out->is_write = (opts & (1u << 6)) != 0;
    out->prdtl = (uint16_t)(opts >> 16);
    out->cmd_table_phys = (uint64_t)ctba | ((uint64_t)ctbau << 32);
}

void hype_ahci_decode_prdt_entry(const uint8_t raw[16], hype_ahci_prdt_entry_t *out) {
    uint32_t dba = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) |
                   ((uint32_t)raw[3] << 24);
    uint32_t dbau = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) | ((uint32_t)raw[6] << 16) |
                    ((uint32_t)raw[7] << 24);
    uint32_t dbc_i = (uint32_t)raw[12] | ((uint32_t)raw[13] << 8) | ((uint32_t)raw[14] << 16) |
                     ((uint32_t)raw[15] << 24);

    out->data_phys = (uint64_t)dba | ((uint64_t)dbau << 32);
    out->byte_count = (dbc_i & 0x3FFFFFu) + 1u; /* DBC is bits 21:0, "byte count - 1" per spec */
}

void hype_ahci_decode_h2d_fis(const uint8_t raw[20], hype_ahci_h2d_fis_t *out) {
    uint64_t lba_low24 = (uint64_t)raw[4] | ((uint64_t)raw[5] << 8) | ((uint64_t)raw[6] << 16);
    uint64_t lba_high24 = (uint64_t)raw[8] | ((uint64_t)raw[9] << 8) | ((uint64_t)raw[10] << 16);

    out->command = raw[2];
    out->lba = lba_low24 | (lba_high24 << 24);
    out->device = raw[7];
    out->count = (uint16_t)(raw[12] | (raw[13] << 8));
}
