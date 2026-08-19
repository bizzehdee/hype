/*
 * AHCI bring-up from inside a guest: find the HBA, place its ABAR, start a port, and issue one
 * command through a command list the guest itself builds.
 *
 * Shared by ahci.c (#548, IDENTIFY against the HBA) and atadisk.c (#550, READ/WRITE DMA EXT
 * against a real backing file) because both need the identical sequence -- a command list, a
 * received-FIS area, a command header, a command table holding an H2D FIS and a PRDT. Writing
 * that twice is how two tests come to disagree about what "issued a command" means.
 *
 * REGISTER ACCESS IS EXPLICIT MOV, not a volatile store. hype decodes the faulting instruction to
 * service an MMIO exit and supports a specific set of forms; at -O2 clang folded one virtio store
 * into a form the device path refused, and the run died on a register that had been written
 * successfully moments before (#550). Writing the instruction out means these tests exercise the
 * device model rather than whatever the optimiser emitted.
 *
 * Header-only and static: each microtest links as its own freestanding binary.
 */
#ifndef MICRO_AHCI_H
#define MICRO_AHCI_H

#include "micro_pci.h"

/* HBA registers. */
#define AHCI_REG_CAP 0x00u
#define AHCI_REG_GHC 0x04u
#define AHCI_REG_IS 0x08u
#define AHCI_REG_PI 0x0Cu
#define AHCI_REG_VS 0x10u

#define AHCI_GHC_HR (1u << 0)
#define AHCI_GHC_IE (1u << 1)
#define AHCI_GHC_AE (1u << 31)

/* Port registers, at PORT_BASE + port * PORT_STRIDE. */
#define AHCI_PORT_BASE 0x100u
#define AHCI_PORT_STRIDE 0x80u
#define AHCI_PORT_COUNT 6u

#define AHCI_PREG_CLB 0x00u
#define AHCI_PREG_CLBU 0x04u
#define AHCI_PREG_FB 0x08u
#define AHCI_PREG_FBU 0x0Cu
#define AHCI_PREG_IS 0x10u
#define AHCI_PREG_IE 0x14u
#define AHCI_PREG_CMD 0x18u
#define AHCI_PREG_TFD 0x20u
#define AHCI_PREG_SIG 0x24u
#define AHCI_PREG_SSTS 0x28u
#define AHCI_PREG_SERR 0x30u
#define AHCI_PREG_CI 0x38u

#define AHCI_PCMD_ST (1u << 0)
#define AHCI_PCMD_FRE (1u << 4)
#define AHCI_PCMD_FR (1u << 14)
#define AHCI_PCMD_CR (1u << 15)

#define AHCI_PIS_DHRS (1u << 0)
#define AHCI_PIS_PSS (1u << 1)

#define AHCI_TFD_ERR (1u << 0)
#define AHCI_TFD_DRQ (1u << 3)
#define AHCI_TFD_BSY (1u << 7)

#define AHCI_SIG_ATA 0x00000101u
#define AHCI_SIG_ATAPI 0xEB140101u

/* ATA commands (devices/ata_disk.h). */
#define ATA_CMD_IDENTIFY_DEVICE 0xECu
#define ATA_CMD_READ_DMA_EXT 0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u

#define FIS_TYPE_H2D_REGISTER 0x27u
#define FIS_H2D_FLAG_C 0x80u

/*
 * Guest-physical layout for the structures the HBA DMAs against. 1 KB-aligned command list and
 * 256-byte-aligned FIS area, which is what the spec requires and what hype's decoder assumes.
 */
#define AHCI_CLB_GPA 0x700000ull   /* command list: 32 headers x 32 bytes */
#define AHCI_FB_GPA 0x701000ull    /* received FIS area */
#define AHCI_CTBA_GPA 0x702000ull  /* command table: CFIS + reserved + PRDT */
#define AHCI_DATA_GPA 0x710000ull  /* the transfer buffer */

#define AHCI_CT_PRDT_OFFSET 0x80u /* CFIS(64) + ACMD(16) + reserved(48) */

static volatile uint8_t *g_abar;

static inline uint32_t ahci_r32(uint32_t off) {
    uint32_t v;
    __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(g_abar + off) : "memory");
    return v;
}
static inline void ahci_w32(uint32_t off, uint32_t v) {
    __asm__ volatile("movl %0, (%1)" : : "r"(v), "r"(g_abar + off) : "memory");
}
static inline uint32_t ahci_pr32(unsigned port, uint32_t reg) {
    return ahci_r32(AHCI_PORT_BASE + port * AHCI_PORT_STRIDE + reg);
}
static inline void ahci_pw32(unsigned port, uint32_t reg, uint32_t v) {
    ahci_w32(AHCI_PORT_BASE + port * AHCI_PORT_STRIDE + reg, v);
}

static inline void ahci_zero(uint64_t gpa, uint32_t len) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)gpa;
    uint32_t i;
    for (i = 0; i < len; i++) {
        p[i] = 0u;
    }
}

/*
 * Find the HBA, place its ABAR and enable decoding. Returns 0 on success.
 * `name` appears in failure text so a caller's verdict names itself.
 */
#define AHCI_PCI_CLASS 0x010601u /* mass storage / SATA / AHCI 1.0 */

/*
 * Walk the bus for AHCI controllers and attach the Nth one, placing its ABAR.
 *
 * A CLASS-CODE WALK, not a fixed device number, because hype presents TWO AHCI controllers: the
 * optical HBA as a device of its own, and the SATA-disk HBA as an ICH9 FUNCTION on device 31 --
 * exactly where a real chipset puts it (00:1f.2). The first version of this header assumed one
 * controller at a known device, found the optical one, and concluded the VM had no disk (#548).
 * Enumerating is also what the ticket asks for and what a real driver does.
 *
 * Returns 0 on success. `index` selects among the controllers found, so a caller can try each.
 */
static int ahci_attach_nth(const char *name, unsigned index, unsigned *out_dev, unsigned *out_func) {
    unsigned dev, func, seen = 0;

    for (dev = 0; dev < 32u; dev++) {
        for (func = 0; func < 8u; func++) {
            uint32_t bar_size;
            uint64_t bar_gpa;

            if (!micro_pci_fpresent(dev, func)) {
                continue;
            }
            if (micro_pci_fclass(dev, func) != AHCI_PCI_CLASS) {
                continue;
            }
            if (seen++ != index) {
                continue;
            }
            micro_puts("micro/");
            micro_puts(name);
            micro_puts(": AHCI controller at ");
            micro_put_uint(dev);
            micro_puts(".");
            micro_put_uint(func);
            micro_puts(" vendor ");
            micro_put_hex(micro_pci_fread32(dev, func, MICRO_PCI_VENDOR_ID) & 0xFFFFu);
            micro_puts("\n");

            /* ABAR is BAR5, the index a real ICH9 uses, so a driver written against real
             * hardware finds it in the same place. Each controller gets its own window so
             * attaching a second does not unmap the first. */
            bar_size = micro_pci_fbar_size(dev, func, 5u);
            bar_gpa = micro_pci_fplace_bar(dev, func, 5u,
                                           MICRO_BAR_WINDOW + (uint64_t)index * 0x10000ull);
            g_abar = (volatile uint8_t *)(uintptr_t)bar_gpa;
            micro_puts("micro/");
            micro_puts(name);
            micro_puts(": ABAR size ");
            micro_put_hex(bar_size);
            micro_puts(" placed at ");
            micro_put_hex(bar_gpa);
            micro_puts("\n");
            if (bar_size < 0x400u) {
                return -1;
            }
            /* AHCI Enable before touching port registers: the spec leaves their meaning
             * undefined in legacy mode, so a driver that skips this reads whatever it likes. */
            ahci_w32(AHCI_REG_GHC, ahci_r32(AHCI_REG_GHC) | AHCI_GHC_AE);
            if (out_dev != 0) *out_dev = dev;
            if (out_func != 0) *out_func = func;
            return 0;
        }
    }
    return -1;
}

static int ahci_attach(const char *name) { return ahci_attach_nth(name, 0u, 0, 0); }


/*
 * The first implemented port carrying a device of type `want_sig`, or -1. Pass 0 for "any".
 *
 * Taking the first port with ANY device is what the first version did, and it was wrong: the rig
 * attaches an ISO, so port 0 is an ATAPI drive and every disk test skipped itself while reporting
 * a pass. Every implemented port is listed either way, so a test that finds nothing says what WAS
 * there rather than only what was missing.
 */
static int ahci_find_port(const char *name, uint32_t want_sig, uint32_t *out_sig) {
    uint32_t pi = ahci_r32(AHCI_REG_PI);
    unsigned p;
    int found = -1;

    micro_puts("micro/");
    micro_puts(name);
    micro_puts(": ports implemented ");
    micro_put_hex(pi);
    micro_puts("\n");
    for (p = 0; p < AHCI_PORT_COUNT; p++) {
        uint32_t sig;
        if ((pi & (1u << p)) == 0u) {
            continue;
        }
        sig = ahci_pr32(p, AHCI_PREG_SIG);
        if (sig != AHCI_SIG_ATA && sig != AHCI_SIG_ATAPI) {
            continue;
        }
        micro_puts("micro/");
        micro_puts(name);
        micro_puts(": port ");
        micro_put_uint(p);
        micro_puts(" signature ");
        micro_put_hex(sig);
        micro_puts(sig == AHCI_SIG_ATA ? " (SATA disk)" : " (ATAPI)");
        if (found < 0 && (want_sig == 0u || sig == want_sig)) {
            micro_puts(" <- using this one");
            found = (int)p;
            if (out_sig != 0) {
                *out_sig = sig;
            }
        }
        micro_puts("\n");
    }
    return found;
}

/*
 * Attach whichever AHCI controller carries a device of type `want_sig`, and return its port.
 * Returns -1 with every controller and port listed, so "no disk" says what WAS found.
 */
static int ahci_find_device(const char *name, uint32_t want_sig, unsigned *out_ctrl) {
    unsigned ctrl;
    for (ctrl = 0; ctrl < 4u; ctrl++) {
        int port;
        if (ahci_attach_nth(name, ctrl, 0, 0) != 0) {
            break;
        }
        port = ahci_find_port(name, want_sig, 0);
        if (port >= 0) {
            if (out_ctrl != 0) {
                *out_ctrl = ctrl;
            }
            return port;
        }
    }
    return -1;
}

/* Stop the engines, point the port at guest-built structures, start it again. */
static void ahci_port_start(unsigned port) {
    unsigned long long spins;
    uint32_t cmd;

    cmd = ahci_pr32(port, AHCI_PREG_CMD);
    ahci_pw32(port, AHCI_PREG_CMD, cmd & ~(AHCI_PCMD_ST | AHCI_PCMD_FRE));
    /* Bounded: a port whose engines never stop is a device bug, and spinning forever on it turns
     * that into a test with no verdict. */
    for (spins = 0; spins < 1000000ull; spins++) {
        if ((ahci_pr32(port, AHCI_PREG_CMD) & (AHCI_PCMD_CR | AHCI_PCMD_FR)) == 0u) {
            break;
        }
    }

    ahci_zero(AHCI_CLB_GPA, 1024u);
    ahci_zero(AHCI_FB_GPA, 256u);
    ahci_zero(AHCI_CTBA_GPA, 256u);

    ahci_pw32(port, AHCI_PREG_CLB, (uint32_t)AHCI_CLB_GPA);
    ahci_pw32(port, AHCI_PREG_CLBU, (uint32_t)(AHCI_CLB_GPA >> 32));
    ahci_pw32(port, AHCI_PREG_FB, (uint32_t)AHCI_FB_GPA);
    ahci_pw32(port, AHCI_PREG_FBU, (uint32_t)(AHCI_FB_GPA >> 32));
    ahci_pw32(port, AHCI_PREG_SERR, 0xFFFFFFFFu); /* write-1-to-clear */
    ahci_pw32(port, AHCI_PREG_IS, 0xFFFFFFFFu);

    cmd = ahci_pr32(port, AHCI_PREG_CMD);
    ahci_pw32(port, AHCI_PREG_CMD, cmd | AHCI_PCMD_FRE | AHCI_PCMD_ST);
}

/*
 * Build slot 0 and issue it: an H2D Register FIS carrying `command`, and one PRDT entry covering
 * `bytes` at AHCI_DATA_GPA. `write` sets the header's W bit.
 *
 * Returns 0 when CI cleared with no error bit in TFD.
 */
static int ahci_issue(const char *name, unsigned port, uint8_t command, uint64_t lba,
                      uint16_t count, uint32_t bytes, int write) {
    volatile uint8_t *hdr = (volatile uint8_t *)(uintptr_t)AHCI_CLB_GPA;
    volatile uint8_t *ct = (volatile uint8_t *)(uintptr_t)AHCI_CTBA_GPA;
    volatile uint8_t *prdt = ct + AHCI_CT_PRDT_OFFSET;
    unsigned long long spins;
    uint32_t tfd;

    ahci_zero(AHCI_CTBA_GPA, 256u);

    /* Command Header, 32 bytes: CFL in DWORDs, W bit, PRDTL, then the command-table address. */
    hdr[0] = 5u;                                    /* CFL = 5 DWORDs = a 20-byte H2D FIS */
    hdr[1] = (uint8_t)(write ? (1u << 6) : 0u);     /* W */
    hdr[2] = 1u;                                    /* PRDTL low  */
    hdr[3] = 0u;                                    /* PRDTL high */
    hdr[4] = 0u; hdr[5] = 0u; hdr[6] = 0u; hdr[7] = 0u; /* PRDBC, device-written */
    *(volatile uint32_t *)(hdr + 8) = (uint32_t)AHCI_CTBA_GPA;
    *(volatile uint32_t *)(hdr + 12) = (uint32_t)(AHCI_CTBA_GPA >> 32);

    /* H2D Register FIS (spec 10.3.4): type, C flag + command, LBA split across three bytes twice. */
    ct[0] = FIS_TYPE_H2D_REGISTER;
    ct[1] = FIS_H2D_FLAG_C;
    ct[2] = command;
    ct[3] = 0u;                                  /* features */
    ct[4] = (uint8_t)(lba & 0xFFu);
    ct[5] = (uint8_t)((lba >> 8) & 0xFFu);
    ct[6] = (uint8_t)((lba >> 16) & 0xFFu);
    ct[7] = (uint8_t)(1u << 6);                  /* device: LBA mode */
    ct[8] = (uint8_t)((lba >> 24) & 0xFFu);
    ct[9] = (uint8_t)((lba >> 32) & 0xFFu);
    ct[10] = (uint8_t)((lba >> 40) & 0xFFu);
    ct[11] = 0u;                                 /* features high */
    ct[12] = (uint8_t)(count & 0xFFu);
    ct[13] = (uint8_t)((count >> 8) & 0xFFu);

    /* PRDT entry: address, then byte count MINUS ONE, which is the field the spec defines and a
     * classic off-by-one if written as the length. */
    *(volatile uint32_t *)(prdt + 0) = (uint32_t)AHCI_DATA_GPA;
    *(volatile uint32_t *)(prdt + 4) = (uint32_t)(AHCI_DATA_GPA >> 32);
    *(volatile uint32_t *)(prdt + 8) = 0u;
    *(volatile uint32_t *)(prdt + 12) = bytes - 1u;

    ahci_pw32(port, AHCI_PREG_IS, 0xFFFFFFFFu);
    ahci_pw32(port, AHCI_PREG_CI, 1u); /* slot 0 */

    for (spins = 0; spins < 200000000ull; spins++) {
        if ((ahci_pr32(port, AHCI_PREG_CI) & 1u) == 0u) {
            break;
        }
    }
    if ((ahci_pr32(port, AHCI_PREG_CI) & 1u) != 0u) {
        micro_puts("micro/");
        micro_puts(name);
        micro_puts(": CI bit 0 never cleared -- the command was never completed\n");
        return -1;
    }
    tfd = ahci_pr32(port, AHCI_PREG_TFD);
    if ((tfd & AHCI_TFD_ERR) != 0u || (tfd & AHCI_TFD_BSY) != 0u) {
        micro_puts("micro/");
        micro_puts(name);
        micro_puts(": task file reports an error, TFD ");
        micro_put_hex(tfd);
        micro_puts("\n");
        return -1;
    }
    return 0;
}

#endif /* MICRO_AHCI_H */
