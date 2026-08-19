/*
 * #550: M5-1 (virtio-blk), ported out of boot/main.c as a real guest-side driver.
 *
 * The in-binary test pointed the device at a synthetic in-RAM buffer its own launch code had set
 * up, and drove it with host-written register pokes. A storage test that cannot be pointed at
 * real storage is testing its own scaffolding -- #452 had to retire the ISO-2 microtest for
 * exactly that reason. Here the guest brings the device up itself, against a backing file named
 * by `[disk.*] backing = file`, so the same artifact can be aimed at a raw image, a qcow2 (#336)
 * or a thin-provisioned target (§10 decision 42) by editing a config rather than editing C.
 *
 * THE DRIVER, in spec order (§3.1.1 device initialisation):
 *
 *   find the device on the PCI bus, place its BAR itself, enable memory + bus-master
 *   reset, then ACKNOWLEDGE | DRIVER
 *   negotiate features: require VIRTIO_F_VERSION_1, offer nothing else
 *   FEATURES_OK, and read it back -- a device that clears it has rejected the driver
 *   set up virtqueue 0 (desc / avail / used), then DRIVER_OK
 *   write a pattern to a sector, read it back into a different buffer, compare
 *
 * WHAT THE GUEST CANNOT SEE, and why the harness has a second half: a read that returns what was
 * written proves the round trip through the device, but not that the bytes ever reached the
 * BACKING FILE -- a device serving from its own cache would pass. So the harness compares the
 * pattern in the host-side file after the run. That is #343's re-read-and-compare discipline, and
 * it is the only way to tell storage from a memory buffer with a storage-shaped interface.
 *
 * ONE FINDING, recorded here because it surprised me: hype's virtio-blk publishes NO virtio PCI
 * capability chain. The spec (§4.1.4) has a driver discover the common-cfg / notify / ISR /
 * device-cfg regions by walking vendor capabilities; hype instead fixes them at BAR offsets 0,
 * 0x1000, 0x2000 and 0x3000, and devices/virtio_blk.h says so ("this implementation's own
 * choice ... shared between the exempt NPF glue and whatever builds the device's own PCI
 * capability list bytes at setup time" -- nothing builds them). A real Linux or FreeBSD driver
 * walks the capabilities, so this test uses the fixed offsets and NAMES the assumption rather
 * than pretending it discovered them.
 *
 * cmdline (#546): `sector=N` picks the LBA. Default 1 rather than 0, so a test run cannot
 * overwrite a partition table on an image someone cares about.
 */
#include "micro_pci.h"

#define NAME "virtioblk"

#define VIRTIO_VENDOR_ID 0x1AF4u

/* Device status bits (spec §2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER 0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FEATURES_OK 0x08u
#define VIRTIO_STATUS_NEEDS_RESET 0x40u
#define VIRTIO_STATUS_FAILED 0x80u

/* common_cfg offsets (spec §4.1.4.3), and hype's fixed BAR layout. */
#define CFG_DEVICE_FEATURE_SELECT 0x00u
#define CFG_DEVICE_FEATURE 0x04u
#define CFG_DRIVER_FEATURE_SELECT 0x08u
#define CFG_DRIVER_FEATURE 0x0Cu
#define CFG_NUM_QUEUES 0x12u
#define CFG_DEVICE_STATUS 0x14u
#define CFG_QUEUE_SELECT 0x16u
#define CFG_QUEUE_SIZE 0x18u
#define CFG_QUEUE_ENABLE 0x1Cu
#define CFG_QUEUE_NOTIFY_OFF 0x1Eu
#define CFG_QUEUE_DESC_LO 0x20u
#define CFG_QUEUE_DESC_HI 0x24u
#define CFG_QUEUE_DRIVER_LO 0x28u
#define CFG_QUEUE_DRIVER_HI 0x2Cu
#define CFG_QUEUE_DEVICE_LO 0x30u
#define CFG_QUEUE_DEVICE_HI 0x34u

#define BAR_COMMON_CFG 0x0000u
#define BAR_NOTIFY_CFG 0x1000u
#define BAR_NOTIFY_MULTIPLIER 4u
#define BAR_DEVICE_CFG 0x3000u

#define BLK_CFG_CAPACITY_LO 0x00u
#define BLK_CFG_BLK_SIZE 0x14u

#define VIRTQ_DESC_F_NEXT 0x0001u
#define VIRTQ_DESC_F_WRITE 0x0002u

#define BLK_T_IN 0u
#define BLK_T_OUT 1u
#define BLK_S_OK 0x00u

#define SECTOR_SIZE 512u
#define QUEUE_SIZE 8u

/* Guest-physical scratch, clear of the payload at 16 MB and of the BAR window at 3 GB. */
#define DESC_GPA 0x600000ull
#define AVAIL_GPA 0x601000ull
#define USED_GPA 0x602000ull
#define HDR_GPA 0x603000ull
#define WBUF_GPA 0x604000ull
#define RBUF_GPA 0x605000ull
#define STATUS_GPA 0x606000ull

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

static volatile uint8_t *g_bar;

/*
 * Explicit MOV forms rather than plain volatile stores.
 *
 * hype decodes the faulting instruction to service an MMIO exit, and it supports a specific set
 * of forms. A C store leaves the encoding to the compiler: at -O2 clang folded one of these into
 * a form hype's virtio path refused, and the run died at device_status with "unhandled virtio-blk
 * MMIO at 0xc0000014" -- while the SAME register had been written successfully twice moments
 * earlier. Writing the instruction out means the test exercises the device model rather than
 * whatever the optimiser felt like emitting that day, and a future failure here is the device's
 * fault rather than codegen's.
 */
static uint8_t mmio_r8(uint32_t off) {
    uint8_t v;
    __asm__ volatile("movb (%1), %0" : "=q"(v) : "r"(g_bar + off) : "memory");
    return v;
}
static uint16_t mmio_r16(uint32_t off) {
    uint16_t v;
    __asm__ volatile("movw (%1), %0" : "=r"(v) : "r"(g_bar + off) : "memory");
    return v;
}
static uint32_t mmio_r32(uint32_t off) {
    uint32_t v;
    __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(g_bar + off) : "memory");
    return v;
}
static void mmio_w8(uint32_t off, uint8_t v) {
    __asm__ volatile("movb %0, (%1)" : : "q"(v), "r"(g_bar + off) : "memory");
}
static void mmio_w16(uint32_t off, uint16_t v) {
    __asm__ volatile("movw %0, (%1)" : : "r"(v), "r"(g_bar + off) : "memory");
}
static void mmio_w32(uint32_t off, uint32_t v) {
    __asm__ volatile("movl %0, (%1)" : : "r"(v), "r"(g_bar + off) : "memory");
}

static unsigned long long parse_uint(const char *s) {
    unsigned long long v = 0ull;
    if (s == 0) {
        return 0ull;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10ull + (unsigned long long)(*s - '0');
        s++;
    }
    return v;
}

/*
 * One request, three descriptors: header (device-readable), data, status byte (device-writable).
 * Returns 0 when the device reported S_OK.
 */
static int do_request(uint32_t type, uint64_t sector, uint64_t data_gpa, uint16_t notify_off,
                      uint16_t *used_idx_seen) {
    virtq_desc_t *desc = (virtq_desc_t *)(uintptr_t)DESC_GPA;
    volatile uint16_t *avail = (volatile uint16_t *)(uintptr_t)AVAIL_GPA;
    volatile uint16_t *used = (volatile uint16_t *)(uintptr_t)USED_GPA;
    volatile uint32_t *hdr = (volatile uint32_t *)(uintptr_t)HDR_GPA;
    volatile uint8_t *status = (volatile uint8_t *)(uintptr_t)STATUS_GPA;
    unsigned long long spins = 0;

    /* virtio_blk_req header: u32 type, u32 reserved, u64 sector (little-endian, native here). */
    hdr[0] = type;
    hdr[1] = 0u;
    *(volatile uint64_t *)(uintptr_t)(HDR_GPA + 8ull) = sector;
    *status = 0xFFu; /* so an untouched status byte is not mistaken for S_OK */

    desc[0].addr = HDR_GPA;
    desc[0].len = 16u;
    desc[0].flags = VIRTQ_DESC_F_NEXT;
    desc[0].next = 1u;

    desc[1].addr = data_gpa;
    desc[1].len = SECTOR_SIZE;
    /* A read needs the device to WRITE into this buffer; a write does not. */
    desc[1].flags = (uint16_t)(VIRTQ_DESC_F_NEXT | ((type == BLK_T_IN) ? VIRTQ_DESC_F_WRITE : 0u));
    desc[1].next = 2u;

    desc[2].addr = STATUS_GPA;
    desc[2].len = 1u;
    desc[2].flags = VIRTQ_DESC_F_WRITE;
    desc[2].next = 0u;

    /* avail ring: {u16 flags, u16 idx, u16 ring[]}. Publish the head, then bump idx. */
    avail[2 + (*used_idx_seen % QUEUE_SIZE)] = 0u;
    avail[1] = (uint16_t)(*used_idx_seen + 1u);

    mmio_w16(BAR_NOTIFY_CFG + (uint32_t)notify_off * BAR_NOTIFY_MULTIPLIER, 0u);

    /* used ring: {u16 flags, u16 idx, {u32 id, u32 len}[]}. Bounded, so a device that never
     * completes produces a named failure rather than a wedge with no verdict. */
    while (used[1] == *used_idx_seen) {
        if (++spins > 200000000ull) {
            micro_puts("micro/" NAME ": the device never advanced used->idx (still ");
            micro_put_uint(used[1]);
            micro_puts(")\n");
            return -1;
        }
    }
    *used_idx_seen = used[1];

    if (*status != BLK_S_OK) {
        micro_puts("micro/" NAME ": request status byte ");
        micro_put_hex(*status);
        micro_puts(" (expected 0 = S_OK)\n");
        return -1;
    }
    return 0;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cl = micro_cmdline(zero_page_gpa);
    uint64_t sector = 1ull;
    uint64_t bar_gpa;
    uint32_t bar_size;
    uint16_t vendor, num_queues, qsize, notify_off;
    uint8_t st;
    uint32_t feat_hi, capacity_lo, blk_size;
    uint16_t used_idx = 0;
    volatile uint8_t *wbuf = (volatile uint8_t *)(uintptr_t)WBUF_GPA;
    volatile uint8_t *rbuf = (volatile uint8_t *)(uintptr_t)RBUF_GPA;
    uint32_t i;
    unsigned long long spins;

    micro_puts("\n");

    if (cl != 0) {
        const char *v = micro_cmdline_value(cl, "sector");
        if (v != 0) {
            sector = parse_uint(v);
        }
    }

    vendor = micro_pci_vendor(MICRO_PCI_DEV_VIRTIO_BLK);
    micro_puts("micro/" NAME ": PCI dev ");
    micro_put_uint(MICRO_PCI_DEV_VIRTIO_BLK);
    micro_puts(" vendor ");
    micro_put_hex(vendor);
    micro_puts("\n");
    if (vendor != VIRTIO_VENDOR_ID) {
        micro_fail(NAME, "no virtio device on the bus -- this VM needs a [disk.*] with "
                         "bus = virtio-blk, or hype did not attach one");
        micro_halt();
    }

    /* BAR 4, not 0: hype puts the virtio-pci regions there (HYPE_FW_1_VIRTIO_BAR_INDEX). A real
     * driver would learn this from the vendor capabilities, which hype does not publish -- see the
     * note at the top. Assuming BAR0 is what the first version of this test did, and it failed
     * with "BAR0 is too small", which is the right way for a wrong assumption to end. */
    bar_size = micro_pci_bar_size(MICRO_PCI_DEV_VIRTIO_BLK, 4u);
    bar_gpa = micro_pci_place_bar(MICRO_PCI_DEV_VIRTIO_BLK, 4u, MICRO_BAR_WINDOW);
    g_bar = (volatile uint8_t *)(uintptr_t)bar_gpa;
    micro_puts("micro/" NAME ": BAR4 size ");
    micro_put_hex(bar_size);
    micro_puts(" placed at ");
    micro_put_hex(bar_gpa);
    micro_puts("\n");
    if (bar_size < 0x4000u) {
        micro_fail(NAME, "BAR4 is too small to hold the four virtio-pci regions");
        micro_halt();
    }
    micro_pci_write32(MICRO_PCI_DEV_VIRTIO_BLK, MICRO_PCI_COMMAND,
                      MICRO_PCI_CMD_MEM_SPACE | MICRO_PCI_CMD_BUS_MASTER);

    /* ---- reset, then ACKNOWLEDGE | DRIVER (spec §3.1.1 steps 1-2) ---------------------- */
    mmio_w8(BAR_COMMON_CFG + CFG_DEVICE_STATUS, 0u);
    spins = 0;
    while (mmio_r8(BAR_COMMON_CFG + CFG_DEVICE_STATUS) != 0u) {
        if (++spins > 10000000ull) {
            micro_fail(NAME, "device_status never read back 0 after a reset");
            micro_halt();
        }
    }
    mmio_w8(BAR_COMMON_CFG + CFG_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* ---- features (steps 3-6). Require VERSION_1; offer nothing optional. -------------- */
    mmio_w32(BAR_COMMON_CFG + CFG_DEVICE_FEATURE_SELECT, 1u);
    feat_hi = mmio_r32(BAR_COMMON_CFG + CFG_DEVICE_FEATURE);
    micro_puts("micro/" NAME ": device features[63:32] ");
    micro_put_hex(feat_hi);
    micro_puts("\n");
    if ((feat_hi & 1u) == 0u) {
        micro_fail(NAME, "the device does not offer VIRTIO_F_VERSION_1, which a modern driver "
                         "requires");
        micro_halt();
    }
    mmio_w32(BAR_COMMON_CFG + CFG_DRIVER_FEATURE_SELECT, 0u);
    mmio_w32(BAR_COMMON_CFG + CFG_DRIVER_FEATURE, 0u);
    mmio_w32(BAR_COMMON_CFG + CFG_DRIVER_FEATURE_SELECT, 1u);
    mmio_w32(BAR_COMMON_CFG + CFG_DRIVER_FEATURE, 1u); /* VERSION_1 only */

    st = (uint8_t)(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    mmio_w8(BAR_COMMON_CFG + CFG_DEVICE_STATUS, st);
    /* Read it back: a device that clears FEATURES_OK has refused what the driver offered, and
     * carrying on from there is how a driver ends up talking a protocol the device is not. */
    if ((mmio_r8(BAR_COMMON_CFG + CFG_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0u) {
        micro_fail(NAME, "the device cleared FEATURES_OK -- it rejected the negotiated features");
        micro_halt();
    }

    num_queues = mmio_r16(BAR_COMMON_CFG + CFG_NUM_QUEUES);
    if (num_queues == 0u) {
        micro_fail(NAME, "the device reports zero virtqueues");
        micro_halt();
    }

    /* ---- virtqueue 0 (step 7) ---------------------------------------------------------- */
    mmio_w16(BAR_COMMON_CFG + CFG_QUEUE_SELECT, 0u);
    qsize = mmio_r16(BAR_COMMON_CFG + CFG_QUEUE_SIZE);
    micro_puts("micro/" NAME ": queues=");
    micro_put_uint(num_queues);
    micro_puts(" queue0 max size=");
    micro_put_uint(qsize);
    micro_puts("\n");
    if (qsize == 0u) {
        micro_fail(NAME, "queue 0 reports size 0");
        micro_halt();
    }
    /* Shrink to something whose rings fit comfortably; the device must accept a smaller size. */
    mmio_w16(BAR_COMMON_CFG + CFG_QUEUE_SIZE, (uint16_t)QUEUE_SIZE);

    for (i = 0; i < 4096u; i++) {
        ((volatile uint8_t *)(uintptr_t)DESC_GPA)[i] = 0u;
        ((volatile uint8_t *)(uintptr_t)AVAIL_GPA)[i] = 0u;
        ((volatile uint8_t *)(uintptr_t)USED_GPA)[i] = 0u;
    }
    mmio_w32(BAR_COMMON_CFG + CFG_QUEUE_DESC_LO, (uint32_t)DESC_GPA);
    mmio_w32(BAR_COMMON_CFG + CFG_QUEUE_DESC_HI, (uint32_t)(DESC_GPA >> 32));
    mmio_w32(BAR_COMMON_CFG + CFG_QUEUE_DRIVER_LO, (uint32_t)AVAIL_GPA);
    mmio_w32(BAR_COMMON_CFG + CFG_QUEUE_DRIVER_HI, (uint32_t)(AVAIL_GPA >> 32));
    mmio_w32(BAR_COMMON_CFG + CFG_QUEUE_DEVICE_LO, (uint32_t)USED_GPA);
    mmio_w32(BAR_COMMON_CFG + CFG_QUEUE_DEVICE_HI, (uint32_t)(USED_GPA >> 32));
    notify_off = mmio_r16(BAR_COMMON_CFG + CFG_QUEUE_NOTIFY_OFF);
    mmio_w16(BAR_COMMON_CFG + CFG_QUEUE_ENABLE, 1u);

    mmio_w8(BAR_COMMON_CFG + CFG_DEVICE_STATUS, (uint8_t)(st | VIRTIO_STATUS_DRIVER_OK));
    st = mmio_r8(BAR_COMMON_CFG + CFG_DEVICE_STATUS);
    if ((st & (VIRTIO_STATUS_FAILED | VIRTIO_STATUS_NEEDS_RESET)) != 0u) {
        micro_puts("micro/" NAME ": device_status ");
        micro_put_hex(st);
        micro_puts("\n");
        micro_fail(NAME, "the device set FAILED or NEEDS_RESET during bring-up");
        micro_halt();
    }

    /* Device-specific config, read AFTER DRIVER_OK so it reflects a live device. */
    capacity_lo = mmio_r32(BAR_DEVICE_CFG + BLK_CFG_CAPACITY_LO);
    blk_size = mmio_r32(BAR_DEVICE_CFG + BLK_CFG_BLK_SIZE);
    micro_puts("micro/" NAME ": capacity ");
    micro_put_uint(capacity_lo);
    micro_puts(" sector(s), blk_size ");
    micro_put_uint(blk_size);
    micro_puts(", notify_off ");
    micro_put_uint(notify_off);
    micro_puts("\n");
    if (capacity_lo == 0u) {
        micro_fail(NAME, "the device reports zero capacity -- its backing file is missing or empty");
        micro_halt();
    }
    if (sector >= (uint64_t)capacity_lo) {
        micro_fail(NAME, "cmdline sector= is beyond the device's capacity");
        micro_halt();
    }

    /* ---- write a pattern, read it back ------------------------------------------------- */
    for (i = 0; i < SECTOR_SIZE; i++) {
        /* Position-dependent, so a device that returns a constant, a shifted copy or another
         * sector's contents fails rather than matching by luck. */
        wbuf[i] = (uint8_t)(0x5Au ^ (uint8_t)i ^ (uint8_t)(sector & 0xFFu));
        rbuf[i] = 0u;
    }
    if (do_request(BLK_T_OUT, sector, WBUF_GPA, notify_off, &used_idx) != 0) {
        micro_fail(NAME, "the write request did not complete with S_OK");
        micro_halt();
    }
    if (do_request(BLK_T_IN, sector, RBUF_GPA, notify_off, &used_idx) != 0) {
        micro_fail(NAME, "the read-back request did not complete with S_OK");
        micro_halt();
    }
    for (i = 0; i < SECTOR_SIZE; i++) {
        if (rbuf[i] != wbuf[i]) {
            micro_puts("micro/" NAME ": byte ");
            micro_put_uint(i);
            micro_puts(" read ");
            micro_put_hex(rbuf[i]);
            micro_puts(" wrote ");
            micro_put_hex(wbuf[i]);
            micro_puts("\n");
            micro_fail(NAME, "the sector did not read back what was written");
            micro_halt();
        }
    }

    micro_puts("micro/" NAME ": sector ");
    micro_put_uint(sector);
    micro_puts(" written and read back byte-for-byte (");
    micro_put_uint(SECTOR_SIZE);
    micro_puts(" bytes, pattern 0x5A^i^lba)\n");
    micro_puts("micro/" NAME ": the guest cannot see the backing FILE -- the harness compares it "
               "on the host, which is what distinguishes storage from a cache\n");
    micro_pass(NAME);
    micro_halt();
}
