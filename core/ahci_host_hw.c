#include "ahci_host.h"
#include "../devices/ahci.h" /* HYPE_AHCI_REG_* / HYPE_AHCI_PREG_* / PxCMD bits / signatures */

/*
 * Hardware shim for the host AHCI driver: real MMIO against the physical HBA.
 * Kept out of the unit-test build (coverage-exempt, like host_pci_hw.c) since it
 * pokes device registers. MMIO uses plain volatile pointers to identity-mapped
 * physical addresses -- the same pattern as arch/x86_64/cpu/lapic.c, relying on
 * the firmware's MTRRs keeping the PCI hole uncacheable (hype never reprograms
 * host MTRRs).
 */

/* DMA-visible structures the HBA reads/writes. In hype's own .bss, which is
 * identity-mapped, so the pointer == the physical address the HBA needs.
 * Alignment per AHCI 1.3.1 §10.1.2: command list 1 KiB, received FIS 256 B,
 * command table 128 B. */
static uint8_t g_cmd_list[1024] __attribute__((aligned(1024)));
static uint8_t g_recv_fis[256] __attribute__((aligned(256)));
static uint8_t g_cmd_table[256] __attribute__((aligned(128)));

/* PxTFD status-byte bits (bits 7:0 of PxTFD). */
#define TFD_STS_BSY 0x80u
#define TFD_STS_DRQ 0x08u
#define TFD_STS_ERR 0x01u

/* Bounded MMIO poll ceilings (spin iterations). Large enough to cover a real
 * spinning-rust seek; a timeout returns an error rather than hanging hype. */
#define SPIN_ENGINE 2000000u
#define SPIN_READY 2000000u
#define SPIN_CMD 20000000u

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(b + off) = v;
}

static volatile uint8_t *port_base(volatile uint8_t *abar, unsigned port) {
    return abar + HYPE_AHCI_PORT_BASE + (uint64_t)port * HYPE_AHCI_PORT_STRIDE;
}

/* Spin until (reg & mask) == 0, or the ceiling is hit. 0 = cleared, -1 = timeout. */
static int wait_clear(volatile uint8_t *b, uint32_t off, uint32_t mask, unsigned spins) {
    while (spins-- != 0u) {
        if ((rd32(b, off) & mask) == 0u) {
            return 0;
        }
    }
    return -1;
}

int hype_ahci_host_find_sata_port(uint64_t abar_phys) {
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    uint32_t pi = rd32(abar, HYPE_AHCI_REG_PI);
    unsigned p;

    for (p = 0; p < 32u; p++) {
        volatile uint8_t *pb;
        if ((pi & (1u << p)) == 0u) {
            continue;
        }
        pb = port_base(abar, p);
        /* DET (PxSSTS[3:0]) == 3: device present + PHY comm established. */
        if ((rd32(pb, HYPE_AHCI_PREG_SSTS) & 0xFu) != 3u) {
            continue;
        }
        /* Non-ATAPI SATA disk signature (ATAPI/CD would be 0xEB140101). */
        if (rd32(pb, HYPE_AHCI_PREG_SIG) == 0x00000101u) {
            return (int)p;
        }
    }
    return -1;
}

int hype_ahci_host_init(uint64_t abar_phys, unsigned port) {
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    volatile uint8_t *pb = port_base(abar, port);
    unsigned i;

    /* Quiesce the port: clear ST, wait for the command-list engine (CR) to stop;
     * clear FRE, wait for the FIS-receive engine (FR) to stop. */
    wr32(pb, HYPE_AHCI_PREG_CMD, rd32(pb, HYPE_AHCI_PREG_CMD) & ~HYPE_AHCI_PCMD_ST);
    if (wait_clear(pb, HYPE_AHCI_PREG_CMD, HYPE_AHCI_PCMD_CR, SPIN_ENGINE) != 0) {
        return -1;
    }
    wr32(pb, HYPE_AHCI_PREG_CMD, rd32(pb, HYPE_AHCI_PREG_CMD) & ~HYPE_AHCI_PCMD_FRE);
    if (wait_clear(pb, HYPE_AHCI_PREG_CMD, HYPE_AHCI_PCMD_FR, SPIN_ENGINE) != 0) {
        return -1;
    }

    /* Point the port at hype's own command list + received-FIS area (they stay
     * programmed for the life of the run; reads below only rewrite slot 0). */
    for (i = 0; i < sizeof(g_cmd_list); i++) {
        g_cmd_list[i] = 0;
    }
    for (i = 0; i < sizeof(g_recv_fis); i++) {
        g_recv_fis[i] = 0;
    }
    wr32(pb, HYPE_AHCI_PREG_CLB, (uint32_t)(uintptr_t)g_cmd_list);
    wr32(pb, HYPE_AHCI_PREG_CLBU, (uint32_t)((uint64_t)(uintptr_t)g_cmd_list >> 32));
    wr32(pb, HYPE_AHCI_PREG_FB, (uint32_t)(uintptr_t)g_recv_fis);
    wr32(pb, HYPE_AHCI_PREG_FBU, (uint32_t)((uint64_t)(uintptr_t)g_recv_fis >> 32));
    wr32(pb, HYPE_AHCI_PREG_SERR, 0xFFFFFFFFu); /* clear sticky errors (write-1-to-clear) */
    wr32(pb, HYPE_AHCI_PREG_IS, 0xFFFFFFFFu);

    /* Re-enable the engines (FRE before ST). */
    wr32(pb, HYPE_AHCI_PREG_CMD, rd32(pb, HYPE_AHCI_PREG_CMD) | HYPE_AHCI_PCMD_FRE);
    wr32(pb, HYPE_AHCI_PREG_CMD, rd32(pb, HYPE_AHCI_PREG_CMD) | HYPE_AHCI_PCMD_ST);
    return 0;
}

int hype_ahci_host_read(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count, void *dst) {
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    volatile uint8_t *pb = port_base(abar, port);

    /* Build slot 0's command header + a READ DMA EXT command table. The port was
     * already pointed at g_cmd_list / g_recv_fis by hype_ahci_host_init(). */
    if (hype_ahci_host_build_read_dma_ext(g_cmd_table, lba, count, (uint64_t)(uintptr_t)dst) != 0) {
        return -1;
    }
    hype_ahci_host_build_cmd_header(g_cmd_list, /*is_write=*/0, /*prdtl=*/1,
                                    (uint64_t)(uintptr_t)g_cmd_table);

    /* Wait for the device to be ready (not BSY, no DRQ) before issuing. */
    if (wait_clear(pb, HYPE_AHCI_PREG_TFD, TFD_STS_BSY | TFD_STS_DRQ, SPIN_READY) != 0) {
        return -1;
    }
    /* Issue slot 0 and poll PxCI until the HBA clears it (command complete). */
    wr32(pb, HYPE_AHCI_PREG_CI, 1u);
    if (wait_clear(pb, HYPE_AHCI_PREG_CI, 1u, SPIN_CMD) != 0) {
        return -1;
    }
    /* Surface an ATA error (TFD status ERR bit). */
    if ((rd32(pb, HYPE_AHCI_PREG_TFD) & TFD_STS_ERR) != 0u) {
        return -1;
    }
    return 0;
}
