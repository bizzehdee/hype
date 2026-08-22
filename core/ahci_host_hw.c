#include "ahci_host.h"
#include "../devices/ahci.h" /* HYPE_AHCI_REG_* / HYPE_AHCI_PREG_* / PxCMD bits / signatures */
#include "fatal.h"           /* hype_debug_print -- temporary GLADDER-10 stall instrumentation */
#include "ticket_lock.h"     /* #658: fair per-port lock, BSP-bounded -- see ahci_port_lock_or_fail */

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
 * command table 128 B.
 *
 * #352: PER PORT, not one set shared by all of them. hype_ahci_host_init() programs a port's
 * PxCLB/PxFB from these, so a single shared set pointed every initialised port at the same
 * command list and the same FIS-receive area. With one port in use that is invisible; #325's
 * optical drive puts a SECOND port in use at the same time as the ESP disk, and then guest-time
 * disk traffic overwrites the CD's command list between its own commands. That surfaced as every
 * guest read off the disc failing (`stream-rd ... ret=-1`) while resolution, which ran before the
 * disk port was busy, had succeeded.
 *
 * 32 ports of 1.5 KiB is 48 KiB of .bss, which is the cheap end of the trade against a class of
 * bug that only appears once two devices are live. */
#define AHCI_HOST_MAX_PORTS 32u
static uint8_t g_cmd_list[AHCI_HOST_MAX_PORTS][1024] __attribute__((aligned(1024)));
static uint8_t g_recv_fis[AHCI_HOST_MAX_PORTS][256] __attribute__((aligned(256)));
/*
 * #295: sized for the vectored write path -- 0x80 bytes of CFIS/ACMD ahead of the PRDT, then one
 * 16-byte entry per segment up to HYPE_AHCI_HOST_SG_MAX_PRDT (32, matching the seg_max hype's
 * virtio-blk advertises, so a whole multi-segment guest request rides one command). The old 256
 * held 8 entries, which silently bounded any merge at a quarter of what the guest is told it may
 * send. 128-byte alignment is the AHCI requirement; 32 ports * 640 B rounds to 20 KiB of .bss.
 */
static uint8_t g_cmd_table[AHCI_HOST_MAX_PORTS]
                          [HYPE_AHCI_HOST_CT_PRDT_OFF +
                           HYPE_AHCI_HOST_SG_MAX_PRDT * HYPE_AHCI_HOST_PRDT_ENTRY_SIZE]
    __attribute__((aligned(128)));

/*
 * #343: serialise commands per port.
 *
 * #352 made the command list, FIS-receive area and command table PER PORT, which fixed two
 * DIFFERENT ports clobbering each other. It does nothing for two vCPUs issuing on the SAME port --
 * and that is the normal case: every VM streams its ISO from the one host device the media
 * resolved to. Two guests reading concurrently then share one command list, one PxCI slot and one
 * receive area.
 *
 * Measured, not theorised: a verification build that re-read every streamed range and compared it
 * against what was written into guest memory found 3 mismatches in 4561 reads across two VMs, and
 * checking the host ISO showed the WRONG side was sometimes the delivered bytes and sometimes the
 * re-read -- which is a race, not a fixed mis-mapping. Wrong bytes reaching a guest's installation
 * media is how #343's FreeBSD guest ends up faulting on a page of its own kernel image.
 *
 * The USB host path already does exactly this (g_usb_xfer_lock, #346). This is the same fix for
 * the AHCI one. Host reads are not a throughput path -- they fill a bounce buffer -- so a spin
 * lock costs nothing worth measuring against silently corrupting a guest.
 *
 * #658: that exchange lock has no fairness -- exactly the shape #362 found on the identical USB
 * host lock: a guest AP issuing back-to-back transfers on a shared port can hold it "effectively
 * continuously" and starve a third contender indefinitely. core/blk_usb.c was hardened past this
 * twice (#362: a ticket lock for arrival-order fairness; #363: a bounded claim for the BSP, since
 * the BSP's own AHCI use -- install-to-physical, media reads, disk inventory, all from
 * boot/main.c -- must never block forever behind a guest core that stops making progress). AHCI's
 * lock never got either fix. This brings it up to the same shape, reusing the shared
 * core/ticket_lock.c primitive rather than re-deriving blk_usb.c's own private one.
 */
static volatile unsigned int g_ahci_ticket_next[AHCI_HOST_MAX_PORTS];
static volatile unsigned int g_ahci_ticket_owner[AHCI_HOST_MAX_PORTS];

static volatile unsigned int g_ahci_bsp_apic = 0xFFFFFFFFu;
static volatile unsigned long long g_ahci_bsp_lock_timeouts;

/* Records which core is the BSP, the same way hype_blk_usb_set_bsp_apic() does, so
 * ahci_port_lock_or_fail() can tell "the BSP" from "a guest AP" without a vcpu context. */
void hype_ahci_host_set_bsp_apic(unsigned int apic_id) { g_ahci_bsp_apic = apic_id; }

/* Nonzero after a real-HW run means the BSP hit the bounded budget below at least once -- the
 * AHCI counterpart of hype_blk_usb_bsp_lock_timeouts(). */
unsigned long long hype_ahci_host_bsp_lock_timeouts(void) { return g_ahci_bsp_lock_timeouts; }

static unsigned int ahci_this_apic(void) {
    return (*(volatile uint32_t *)(uintptr_t)0xFEE00020u) >> 24;
}

/* Matches USB_BSP_LOCK_BUDGET's order of magnitude (core/blk_usb.c) -- large enough to ride out
 * ordinary contention, bounded so the BSP's console/keyboard/log never freeze behind a guest core
 * that has stopped making progress. */
#define AHCI_BSP_LOCK_BUDGET 20000000u

static int ahci_port_lock_bounded(unsigned port) {
    unsigned int budget = AHCI_BSP_LOCK_BUDGET;

    /* #377-shaped care (see core/blk_usb.c's own comment on this exact point): claim only while
     * next == owner, so a timing-out BSP never advances the queue out from under whoever is
     * really holding it or waiting ahead of it. A failed claim mutates neither counter. */
    while (budget-- != 0u) {
        if (hype_ticket_lock_try_claim(&g_ahci_ticket_next[port], &g_ahci_ticket_owner[port])) {
            return 0;
        }
        __builtin_ia32_pause();
    }
    g_ahci_bsp_lock_timeouts++;
    return -1;
}

/* Guest AP callers wait as long as it takes (returning short data to a guest is not an option);
 * the BSP gets the bounded claim above and fails instead, matching usb_xfer_lock_or_fail(). */
static int ahci_port_lock_or_fail(unsigned port) {
    if (ahci_this_apic() == g_ahci_bsp_apic) {
        return ahci_port_lock_bounded(port);
    }
    hype_ticket_lock_acquire(&g_ahci_ticket_next[port], &g_ahci_ticket_owner[port]);
    return 0;
}

static void ahci_port_unlock(unsigned port) {
    hype_ticket_lock_release(&g_ahci_ticket_owner[port]);
}

/* The unlocked bodies; each public entry point below takes the port lock and calls its own. */
static int ahci_atapi_read_locked(uint64_t abar_phys, unsigned port, uint32_t lba2k,
                                  uint16_t count2k, void *dst);
static int ahci_read_locked(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count,
                            void *dst);
static int ahci_write_locked(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count,
                             const void *src);
static int ahci_identify_locked(uint64_t abar_phys, unsigned port, void *dst512);


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

/* #325: which PxSIG the scan below matches. Defaults to a plain disk; the ATAPI entry point swaps
 * it for the duration of one call. Single-threaded, one scan at a time. */
static uint32_t g_scan_sig = HYPE_AHCI_HOST_SIG_ATA;

/*
 * #258: one port's worth of the scan, so a caller can walk 0..31 ONCE.
 *
 * Split out rather than letting a caller re-enter the whole scan per disk. Doing that made
 * enumeration quadratic, and since the PHY-settle retry below spins up to SPIN_READY on every
 * EMPTY port, a controller with two disks then paid ~30 x 2M spins re-scanning the empty tail.
 * Measured, not predicted: it stalled host discovery past a 110-second QEMU timeout before the
 * selection step ran at all.
 *
 * Returns 1 if `port` is implemented, its PHY is up, and its signature matches the current scan
 * target; 0 otherwise.
 */
/*
 * #369: the PHY-settle wait, paid ONCE per controller instead of once per port.
 *
 * Every PHY on an HBA negotiates in parallel, so waiting for them one at a time buys nothing and
 * costs a full budget per port that never comes up. #258 made the inventory walk all 32 ports, and
 * since the wait used to live inside the per-port check, a 6-port controller with a disk on port 0
 * paid five whole SPIN_READY budgets -- 10,000,000 uncached MMIO reads, ~60 s under QEMU where each
 * one is a VM exit -- before any guest started. Long enough that the 120 s regression run timed out
 * and read as a hype hang.
 *
 * Settling here, across all implemented ports at once, means the wait is bounded by the slowest
 * PHY rather than by the number of empty sockets. hype_ahci_host_settle_continue() decides when to
 * stop; see its contract for how DET==0 (nothing there) is separated from DET==1/2 (coming up).
 *
 * Remembered for one controller, because every caller walks a single HBA's ports to completion
 * before moving on. A different abar re-settles, which is slower than a larger cache would be but
 * never wrong. hype does not hot-plug, so a settled controller stays settled.
 */
static uint64_t g_settled_abar;
static int g_settled_valid;

static void settle_controller(volatile uint8_t *abar, uint64_t abar_phys) {
    uint32_t pi;
    unsigned elapsed;

    if (g_settled_valid && g_settled_abar == abar_phys) {
        return;
    }
    pi = rd32(abar, HYPE_AHCI_REG_PI);
    for (elapsed = 0u; elapsed < SPIN_READY; elapsed++) {
        unsigned pending = 0u;
        unsigned negotiating = 0u;
        unsigned p;
        for (p = 0u; p < 32u; p++) {
            uint32_t det;
            if ((pi & (1u << p)) == 0u) {
                continue;
            }
            det = rd32(port_base(abar, p), HYPE_AHCI_PREG_SSTS) & 0xFu;
            if (det == 3u) {
                continue;
            }
            pending++;
            if (det == 1u || det == 2u) {
                negotiating++;
            }
        }
        if (!hype_ahci_host_settle_continue(pending, negotiating, elapsed)) {
            break;
        }
    }
    g_settled_abar = abar_phys;
    g_settled_valid = 1;
}

int hype_ahci_host_port_matches(uint64_t abar_phys, unsigned port) {
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    uint32_t pi;
    volatile uint8_t *pb;

    if (port >= 32u) {
        return 0;
    }
    settle_controller(abar, abar_phys);
    pi = rd32(abar, HYPE_AHCI_REG_PI);
    if ((pi & (1u << port)) == 0u) {
        return 0;
    }
    pb = port_base(abar, port);
    if ((rd32(pb, HYPE_AHCI_PREG_SSTS) & 0xFu) != 3u) {
        return 0; /* PHY never came up -> genuinely no device on this port */
    }
    /* #325: was hardcoded to the non-ATAPI signature, which made a real optical drive invisible
     * by construction. Now whichever signature the current scan is looking for. */
    return (rd32(pb, HYPE_AHCI_PREG_SIG) == g_scan_sig) ? 1 : 0;
}

int hype_ahci_host_find_sata_port_from(uint64_t abar_phys, unsigned start_port) {
    unsigned p;

    for (p = start_port; p < 32u; p++) {
        if (hype_ahci_host_port_matches(abar_phys, p)) {
            return (int)p;
        }
    }
    return -1;
}

/*
 * #258: the original single-shot scan, now a thin wrapper.
 *
 * It returned the FIRST matching port and the whole host-disk path was built on that one value, so
 * hype could only ever see one SATA disk per controller. On the ordinary desktop layout -- OS disk
 * on port 0, scratch on port 1 -- a `physical:` target on the scratch was unreachable by
 * construction. Callers that need every disk iterate with _from() instead; this stays for the
 * "just give me a disk" callers and for the ATAPI scan.
 */
int hype_ahci_host_find_sata_port(uint64_t abar_phys) {
    return hype_ahci_host_find_sata_port_from(abar_phys, 0u);
}

/*
 * #325: the same scan, for a packet device -- an operator with a bootable disc had to copy it onto a
 * partitioned disk first, which is a worse story than the hardware hype itself emulates.
 *
 * Implemented by re-running the existing scan with a different target signature rather than as a
 * second copy of the PHY-settle loop: that loop carries a hard-won retry (the AMD laptop's SATA SSD
 * was found only on some boots), and a duplicate would inevitably drift from it -- see #342.
 */
int hype_ahci_host_find_atapi_port(uint64_t abar_phys) {
    int port;

    g_scan_sig = HYPE_AHCI_HOST_SIG_ATAPI;
    port = hype_ahci_host_find_sata_port(abar_phys);
    g_scan_sig = HYPE_AHCI_HOST_SIG_ATA;
    return port;
}

/*
 * #325: read `count2k` 2048-byte sectors from a real optical drive. Same issue sequence as the ATA
 * path -- the difference is entirely in the command table (PACKET + CDB) and the header's A bit.
 *
 * An empty drive is a NORMAL state for optical media, unlike an absent disk, so a failure here is
 * reported by the return code for the caller to handle, never fatal.
 */
int hype_ahci_host_atapi_read(uint64_t abar_phys, unsigned port, uint32_t lba2k, uint16_t count2k,
                              void *dst) {
    int rc;
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    if (ahci_port_lock_or_fail(port) != 0) {
        return -1;
    }
    rc = ahci_atapi_read_locked(abar_phys, port, lba2k, count2k, dst);
    ahci_port_unlock(port);
    return rc;
}

static int ahci_atapi_read_locked(uint64_t abar_phys, unsigned port, uint32_t lba2k, uint16_t count2k,
                                  void *dst) {
    /* #352: port indexes the per-port DMA structures, so it must be in range before use. */
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    volatile uint8_t *pb = port_base(abar, port);
    int rc = 0;

    if (hype_ahci_host_build_atapi_read10(g_cmd_table[port], lba2k, count2k,
                                          (uint64_t)(uintptr_t)dst) != 0) {
        return -1;
    }
    hype_ahci_host_build_cmd_header_atapi(g_cmd_list[port], /*prdtl=*/1,
                                          (uint64_t)(uintptr_t)g_cmd_table[port]);

    if (wait_clear(pb, HYPE_AHCI_PREG_TFD, TFD_STS_BSY | TFD_STS_DRQ, SPIN_READY) != 0) {
        return -1;
    }
    wr32(pb, HYPE_AHCI_PREG_CI, 1u);
    if (wait_clear(pb, HYPE_AHCI_PREG_CI, 1u, SPIN_CMD) != 0) {
        rc = -1;
    } else if ((rd32(pb, HYPE_AHCI_PREG_TFD) & TFD_STS_ERR) != 0u) {
        /* Includes "no medium present" -- the drive is there, the disc is not. Normal, not fatal. */
        rc = -1;
    }
    return rc;
}

void hype_ahci_host_dump_ports(uint64_t abar_phys) {
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    uint32_t cap = rd32(abar, HYPE_AHCI_REG_CAP);
    uint32_t pi = rd32(abar, HYPE_AHCI_REG_PI);
    unsigned p;

    hype_debug_print("host-ahci: dump -- CAP=0x%08x PI=0x%08x (NP=%u ports impl)\n",
                     cap, pi, (unsigned)__builtin_popcount(pi));
    for (p = 0; p < 32u; p++) {
        volatile uint8_t *pb;
        uint32_t ssts, sig, cmd, tfd;
        if ((pi & (1u << p)) == 0u) continue;
        pb = port_base(abar, p);
        ssts = rd32(pb, HYPE_AHCI_PREG_SSTS);
        sig = rd32(pb, HYPE_AHCI_PREG_SIG);
        cmd = rd32(pb, HYPE_AHCI_PREG_CMD);
        tfd = rd32(pb, HYPE_AHCI_PREG_TFD);
        /* DET (SSTS[3:0]): 0=no dev, 1=dev-no-PHY, 3=dev+PHY. SPD (SSTS[7:4]) =
         * negotiated gen. SIG: 0x00000101 SATA disk, 0xEB140101 ATAPI. */
        hype_debug_print("host-ahci:   port %u: SSTS=0x%08x (DET=%u SPD=%u IPM=%u) SIG=0x%08x "
                         "CMD=0x%08x TFD=0x%08x\n", p, ssts, (unsigned)(ssts & 0xFu),
                         (unsigned)((ssts >> 4) & 0xFu), (unsigned)((ssts >> 8) & 0xFu),
                         sig, cmd, tfd);
    }
}

int hype_ahci_host_init(uint64_t abar_phys, unsigned port) {
    /* #352: port indexes the per-port DMA structures, so it must be in range before use. */
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
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
    for (i = 0; i < sizeof(g_cmd_list[port]); i++) {
        g_cmd_list[port][i] = 0;
    }
    for (i = 0; i < sizeof(g_recv_fis[port]); i++) {
        g_recv_fis[port][i] = 0;
    }
    wr32(pb, HYPE_AHCI_PREG_CLB, (uint32_t)(uintptr_t)g_cmd_list[port]);
    wr32(pb, HYPE_AHCI_PREG_CLBU, (uint32_t)((uint64_t)(uintptr_t)g_cmd_list[port] >> 32));
    wr32(pb, HYPE_AHCI_PREG_FB, (uint32_t)(uintptr_t)g_recv_fis[port]);
    wr32(pb, HYPE_AHCI_PREG_FBU, (uint32_t)((uint64_t)(uintptr_t)g_recv_fis[port] >> 32));
    wr32(pb, HYPE_AHCI_PREG_SERR, 0xFFFFFFFFu); /* clear sticky errors (write-1-to-clear) */
    wr32(pb, HYPE_AHCI_PREG_IS, 0xFFFFFFFFu);

    /* Polled driver: mask this port's completion interrupts (PxIE) AND the HBA's
     * global interrupt enable (GHC.IE), so the real controller never raises an IRQ
     * into hype's host IDT while we poll PxCI. Firmware may have left them enabled;
     * an unexpected AHCI interrupt landing on a vector hype doesn't handle was the
     * suspected cause of the intermittent streaming-boot hang. */
    wr32(pb, HYPE_AHCI_PREG_IE, 0u);
    wr32(abar, HYPE_AHCI_REG_GHC, rd32(abar, HYPE_AHCI_REG_GHC) & ~HYPE_AHCI_GHC_IE);

    /* Re-enable the engines (FRE before ST). */
    wr32(pb, HYPE_AHCI_PREG_CMD, rd32(pb, HYPE_AHCI_PREG_CMD) | HYPE_AHCI_PCMD_FRE);
    wr32(pb, HYPE_AHCI_PREG_CMD, rd32(pb, HYPE_AHCI_PREG_CMD) | HYPE_AHCI_PCMD_ST);
    return 0;
}

int hype_ahci_host_read(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count, void *dst) {
    int rc;
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    if (ahci_port_lock_or_fail(port) != 0) {
        return -1;
    }
    rc = ahci_read_locked(abar_phys, port, lba, count, dst);
    ahci_port_unlock(port);
    return rc;
}

static int ahci_read_locked(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count, void *dst) {
    /* #352: port indexes the per-port DMA structures, so it must be in range before use. */
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    volatile uint8_t *pb = port_base(abar, port);
    int rc = 0;
    static unsigned dbg = 0; /* GLADDER-10 stall localization: trace the first few reads */
    int trace = (dbg < 8u);
    /*
     * #346: a device whose command NEVER completes must not be re-probed. Each timeout burns
     * SPIN_CMD (20M) uncached MMIO reads -- 6-20 SECONDS on real silicon -- and the media scan
     * retries per partition per resolver, so one bad device turned boot into minutes of silent
     * crawl (the 2026-08-06 Crucial BX500 run: several reads completed, then one wedged and the
     * operator powered off). One loud line with the port state, then every later call fails
     * fast and the boot continues off the other devices.
     */
    static uint64_t g_dead_abar; static unsigned g_dead_port; static int g_dead;
    if (g_dead && g_dead_abar == abar_phys && g_dead_port == port) {
        return -1;
    }
    if (trace) {
        dbg++;
        hype_debug_print("ahci-rd[%u] enter lba=%llu cnt=%u tfd=0x%x ci=0x%x\n", dbg,
                         (unsigned long long)lba, (unsigned)count,
                         (unsigned)rd32(pb, HYPE_AHCI_PREG_TFD), (unsigned)rd32(pb, HYPE_AHCI_PREG_CI));
    }

    /* Build slot 0's command header + a READ DMA EXT command table. The port was
     * already pointed at this port's g_cmd_list / g_recv_fis by hype_ahci_host_init(). */
    if (hype_ahci_host_build_read_dma_ext(g_cmd_table[port], lba, count, (uint64_t)(uintptr_t)dst) != 0) {
        return -1;
    }
    hype_ahci_host_build_cmd_header(g_cmd_list[port], /*is_write=*/0, /*prdtl=*/1,
                                    (uint64_t)(uintptr_t)g_cmd_table[port]);

    /* Wait for the device to be ready (not BSY, no DRQ) before issuing. */
    if (wait_clear(pb, HYPE_AHCI_PREG_TFD, TFD_STS_BSY | TFD_STS_DRQ, SPIN_READY) != 0) {
        rc = -1;
        /* #346: this branch wedged silently too -- the 2026-08-06 run 3 photo shows completed
         * reads then nothing, with no CI-timeout line, so the stall can be HERE. Same
         * latch+dump as the CI branch below. */
        g_dead = 1; g_dead_abar = abar_phys; g_dead_port = port;
        hype_debug_print("ahci-rd: DEVICE NEVER READY port %u lba=%llu -- tfd=0x%x serr=0x%x; "
                         "marking device DEAD, boot continues without it (#346)\n",
                         port, (unsigned long long)lba,
                         (unsigned)rd32(pb, HYPE_AHCI_PREG_TFD),
                         (unsigned)rd32(pb, HYPE_AHCI_PREG_SERR));
    } else {
        if (trace) {
            hype_debug_print("ahci-rd[%u] ready, issuing\n", dbg);
        }
        /* Issue slot 0 and poll PxCI until the HBA clears it (command complete). */
        wr32(pb, HYPE_AHCI_PREG_CI, 1u);
        if (wait_clear(pb, HYPE_AHCI_PREG_CI, 1u, SPIN_CMD) != 0) {
            rc = -1;
            /* #346: the diagnosis the hung run could not give -- what the port looked like
             * when the completion never came. Printed once; the latch silences repeats. */
            g_dead = 1; g_dead_abar = abar_phys; g_dead_port = port;
            hype_debug_print("ahci-rd: COMMAND TIMED OUT port %u lba=%llu -- is=0x%x tfd=0x%x "
                             "serr=0x%x ci=0x%x sact=0x%x; marking device DEAD, boot continues "
                             "without it (#346)\n",
                             port, (unsigned long long)lba,
                             (unsigned)rd32(pb, HYPE_AHCI_PREG_IS),
                             (unsigned)rd32(pb, HYPE_AHCI_PREG_TFD),
                             (unsigned)rd32(pb, HYPE_AHCI_PREG_SERR),
                             (unsigned)rd32(pb, HYPE_AHCI_PREG_CI),
                             (unsigned)rd32(pb, HYPE_AHCI_PREG_SACT));
        } else if ((rd32(pb, HYPE_AHCI_PREG_TFD) & TFD_STS_ERR) != 0u) {
            rc = -1; /* ATA error (TFD status ERR bit) */
        }
    }
    if (trace) {
        hype_debug_print("ahci-rd[%u] done rc=%d\n", dbg, rc);
    }
    return rc;
}

int hype_ahci_host_write(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count,
                         const void *src) {
    int rc;
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    if (ahci_port_lock_or_fail(port) != 0) {
        return -1;
    }
    rc = ahci_write_locked(abar_phys, port, lba, count, src);
    ahci_port_unlock(port);
    return rc;
}

static int ahci_write_locked(uint64_t abar_phys, unsigned port, uint64_t lba, uint16_t count,
                             const void *src) {
    /* #352: port indexes the per-port DMA structures, so it must be in range before use. */
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    volatile uint8_t *pb = port_base(abar, port);
    int rc = 0;

    /* Mirror of hype_ahci_host_read() with a WRITE DMA EXT command table and the
     * command-header W bit set. x86 DMA is cache-coherent, so no flush needed. */
    if (hype_ahci_host_build_write_dma_ext(g_cmd_table[port], lba, count, (uint64_t)(uintptr_t)src) != 0) {
        return -1;
    }
    hype_ahci_host_build_cmd_header(g_cmd_list[port], /*is_write=*/1, /*prdtl=*/1,
                                    (uint64_t)(uintptr_t)g_cmd_table[port]);

    if (wait_clear(pb, HYPE_AHCI_PREG_TFD, TFD_STS_BSY | TFD_STS_DRQ, SPIN_READY) != 0) {
        return -1;
    }
    wr32(pb, HYPE_AHCI_PREG_CI, 1u);
    if (wait_clear(pb, HYPE_AHCI_PREG_CI, 1u, SPIN_CMD) != 0) {
        rc = -1;
    } else if ((rd32(pb, HYPE_AHCI_PREG_TFD) & TFD_STS_ERR) != 0u) {
        rc = -1;
    }
    return rc;
}

/*
 * #295: one WRITE DMA EXT carrying the whole segment list -- one PRDT entry per segment, one
 * command completion instead of nsegs of them. The caller (blk_phys's batching loop) guarantees
 * nsegs <= HYPE_AHCI_HOST_SG_MAX_PRDT and the total <= one command's ceiling; both are re-checked
 * by the builder anyway, because this is the destructive path and "the caller promised" is not a
 * bounds check.
 */
static int ahci_writev_locked(uint64_t abar_phys, unsigned port, uint64_t lba,
                              const hype_ahci_host_sg_t *sg, unsigned int nsegs, uint16_t count) {
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    volatile uint8_t *pb = port_base(abar, port);
    int rc = 0;

    if (hype_ahci_host_build_write_dma_ext_sg(g_cmd_table[port], lba, count, sg, nsegs,
                                              HYPE_AHCI_HOST_SG_MAX_PRDT) != 0) {
        return -1;
    }
    hype_ahci_host_build_cmd_header(g_cmd_list[port], /*is_write=*/1, /*prdtl=*/(uint16_t)nsegs,
                                    (uint64_t)(uintptr_t)g_cmd_table[port]);

    if (wait_clear(pb, HYPE_AHCI_PREG_TFD, TFD_STS_BSY | TFD_STS_DRQ, SPIN_READY) != 0) {
        return -1;
    }
    wr32(pb, HYPE_AHCI_PREG_CI, 1u);
    if (wait_clear(pb, HYPE_AHCI_PREG_CI, 1u, SPIN_CMD) != 0) {
        rc = -1;
    } else if ((rd32(pb, HYPE_AHCI_PREG_TFD) & TFD_STS_ERR) != 0u) {
        rc = -1;
    }
    return rc;
}

int hype_ahci_host_writev(uint64_t abar_phys, unsigned port, uint64_t lba,
                          const hype_ahci_host_sg_t *sg, unsigned int nsegs, uint16_t count) {
    int rc;
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    if (ahci_port_lock_or_fail(port) != 0) {
        return -1;
    }
    rc = ahci_writev_locked(abar_phys, port, lba, sg, nsegs, count);
    ahci_port_unlock(port);
    return rc;
}

int hype_ahci_host_identify(uint64_t abar_phys, unsigned port, void *dst512) {
    int rc;
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    if (ahci_port_lock_or_fail(port) != 0) {
        return -1;
    }
    rc = ahci_identify_locked(abar_phys, port, dst512);
    ahci_port_unlock(port);
    return rc;
}

static int ahci_identify_locked(uint64_t abar_phys, unsigned port, void *dst512) {
    /* #352: port indexes the per-port DMA structures, so it must be in range before use. */
    if (port >= AHCI_HOST_MAX_PORTS) {
        return -1;
    }
    volatile uint8_t *abar = (volatile uint8_t *)(uintptr_t)abar_phys;
    volatile uint8_t *pb = port_base(abar, port);
    int rc = 0;

    /* Same slot-0 mechanism as hype_ahci_host_read(), but an IDENTIFY command
     * table -- a non-write data-in transfer of exactly 512 bytes into dst512. */
    hype_ahci_host_build_identify(g_cmd_table[port], (uint64_t)(uintptr_t)dst512);
    hype_ahci_host_build_cmd_header(g_cmd_list[port], /*is_write=*/0, /*prdtl=*/1,
                                    (uint64_t)(uintptr_t)g_cmd_table[port]);

    if (wait_clear(pb, HYPE_AHCI_PREG_TFD, TFD_STS_BSY | TFD_STS_DRQ, SPIN_READY) != 0) {
        return -1;
    }
    wr32(pb, HYPE_AHCI_PREG_CI, 1u);
    if (wait_clear(pb, HYPE_AHCI_PREG_CI, 1u, SPIN_CMD) != 0) {
        rc = -1;
    } else if ((rd32(pb, HYPE_AHCI_PREG_TFD) & TFD_STS_ERR) != 0u) {
        rc = -1;
    }
    return rc;
}
