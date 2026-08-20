#ifndef HYPE_DEVICES_VIRTIO_BLK_H
#define HYPE_DEVICES_VIRTIO_BLK_H

#include <stdint.h>

#include "../core/blk_backend.h" /* hype_blk_backend_t (queue backing) */
#include "../core/guest_mem.h"   /* hype_gpa_map_t (descriptor DMA) */

/*
 * M5-1: a modern (non-transitional, virtio 1.x) virtio-blk PCI device
 * -- what a real Linux/BSD guest's own inbox virtio_blk driver
 * discovers and drives for disk I/O, distinct from M4-5's AHCI/ATAPI
 * transport (that's a real-SATA-protocol optical drive; this is a
 * paravirtualized, protocol-of-its-own block device with no SATA/ATA
 * layer at all).
 *
 * PCI identity, the virtio-pci capability structure, common
 * configuration register layout, device-status handshake, virtqueue
 * wire format, and virtio_blk_req layout below were fetched and
 * confirmed directly from the real OASIS VIRTIO v1.1 specification
 * (docs.oasis-open.org) plus the Linux kernel's own
 * include/uapi/linux/{virtio_pci.h,virtio_ring.h,virtio_blk.h} and
 * QEMU's hw/virtio/virtio-pci.c / hw/block/virtio-blk.c, not
 * reconstructed from memory -- same discipline as this project's other
 * wire-format structs. Scoped to exactly one virtqueue (this project's
 * own single-queue convention, matching AHCI's own single-port scope)
 * and a single, fixed-size in-memory backing buffer -- a real
 * host-file-backed store is M5-3's own job ("blk_backend"), not this
 * task's.
 *
 * PCI identity: vendor 0x1AF4 (PCI_VENDOR_ID_REDHAT_QUMRANET, the
 * standard virtio vendor ID every implementation uses), device 0x1042
 * (0x1040 + virtio device-type ID 2 = block device, the modern/
 * non-transitional device-ID scheme), class 0x01/0x00/0x00 (mass
 * storage / SCSI controller -- QEMU's own real convention for
 * virtio-blk-pci, not the more generic 0x0180 "storage, other" a naive
 * reading of the spec might suggest; the spec itself is silent on
 * class code, so matching QEMU's actual convention is what real
 * BIOS/lspci-class tooling expects to see).
 */

#define HYPE_VIRTIO_BLK_PCI_VENDOR_ID 0x1AF4u
#define HYPE_VIRTIO_BLK_PCI_DEVICE_ID 0x1042u
#define HYPE_VIRTIO_BLK_PCI_CLASS_BASE 0x01u
#define HYPE_VIRTIO_BLK_PCI_CLASS_SUB 0x00u
#define HYPE_VIRTIO_BLK_PCI_CLASS_INTERFACE 0x00u

/* Device status bits (common cfg's device_status register) -- the
 * real virtio device-initialization handshake a driver performs in
 * this exact order. */
#define HYPE_VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define HYPE_VIRTIO_STATUS_DRIVER 0x02u
#define HYPE_VIRTIO_STATUS_DRIVER_OK 0x04u
#define HYPE_VIRTIO_STATUS_FEATURES_OK 0x08u
#define HYPE_VIRTIO_STATUS_DEVICE_NEEDS_RESET 0x40u
#define HYPE_VIRTIO_STATUS_FAILED 0x80u

/* The transport feature bit this project offers -- VIRTIO_F_VERSION_1
 * (bit 32 of the 64-bit feature space, i.e. bit 0 of the HIGH 32-bit
 * half a driver reads via device_feature_select=1). */
#define HYPE_VIRTIO_F_VERSION_1_BIT 32u

/*
 * VIRTIO_BLK_F_SEG_MAX (bit 2 of the LOW half), and why "offering zero optional bits is a safe
 * fallback" was true of the driver and false of the PERFORMANCE.
 *
 * Linux's virtblk_probe() does:
 *
 *     err = virtio_cread_feature(vdev, VIRTIO_BLK_F_SEG_MAX, ..., seg_max, &sg_elems);
 *     if (err || !sg_elems) sg_elems = 1;
 *     blk_queue_max_segments(q, sg_elems);
 *
 * So a device that does not offer this bit is telling the guest ONE SEGMENT PER REQUEST. The block
 * layer then cannot merge anything: every request is a single physically-contiguous run, which on a
 * 4 KiB-page host means every request is 4 KiB. Measured on this rig before this bit existed (#295
 * step 0): `dd bs=1M count=64` to a virtio-blk disk produced count=20480 max=8 hist=0/0/20480/0/0/0
 * -- twenty thousand 4 KiB writes, not one of them larger, at 443-656 KB/s. A one-megabyte write
 * was being shredded into 256 requests by hype's own silence.
 *
 * The fallback was safe in the sense that mattered at the time (the driver binds and works), and
 * that is why it stood -- but "the absence of every optional feature is handled" was read as "the
 * absence costs nothing", and those are different claims.
 *
 * hype has been able to serve multi-segment chains since #268, which transfers every data segment
 * in a chain and advances the LBA per segment. Without this bit that loop could never see more than
 * one segment, so #268's work was unreachable in practice from a Linux guest.
 */
#define HYPE_VIRTIO_BLK_F_SEG_MAX_BIT 2u

/*
 * What to report in `seg_max`, and it is a trade rather than "as large as possible".
 *
 * A request costs seg_max + 2 descriptors (header + segments + status), so a big seg_max means few
 * requests fit the 256-descriptor queue at once. And the drain issues ONE BACKEND I/O PER SEGMENT
 * (#268), so a long chain is not fewer disk commands -- it is fewer guest exits and fewer virtqueue
 * round trips for the same disk work. 32 segments is 128 KiB per request at 4 KiB a segment, and
 * 256 / (32 + 2) = 7 requests can still be outstanding.
 *
 * Turning those contiguous segments into ONE multi-PRDT AHCI command is #295, and it is a separate
 * change: this bit is what gives it something to gather.
 */
#define HYPE_VIRTIO_BLK_SEG_MAX 32u

/* Common configuration structure byte offsets (within the MMIO region
 * a VIRTIO_PCI_CAP_COMMON_CFG capability points at) -- spec §4.1.4.3. */
#define HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE_SELECT 0x00u
#define HYPE_VIRTIO_COMMON_CFG_DEVICE_FEATURE 0x04u
#define HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE_SELECT 0x08u
#define HYPE_VIRTIO_COMMON_CFG_DRIVER_FEATURE 0x0Cu
#define HYPE_VIRTIO_COMMON_CFG_MSIX_CONFIG 0x10u
#define HYPE_VIRTIO_COMMON_CFG_NUM_QUEUES 0x12u
#define HYPE_VIRTIO_COMMON_CFG_DEVICE_STATUS 0x14u
#define HYPE_VIRTIO_COMMON_CFG_CONFIG_GENERATION 0x15u
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_SELECT 0x16u
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_SIZE 0x18u
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_MSIX_VECTOR 0x1Au
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_ENABLE 0x1Cu
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_NOTIFY_OFF 0x1Eu
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_LO 0x20u
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_DESC_HI 0x24u
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_LO 0x28u
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_DRIVER_HI 0x2Cu
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_LO 0x30u
#define HYPE_VIRTIO_COMMON_CFG_QUEUE_DEVICE_HI 0x34u
#define HYPE_VIRTIO_COMMON_CFG_SIZE 0x38u /* 56 bytes total */

/* Device-specific configuration structure (virtio_blk_config) byte
 * offsets -- spec §5.2.4. This project models only the fields a
 * minimal driver actually reads. */
#define HYPE_VIRTIO_BLK_CFG_CAPACITY_LO 0x00u
#define HYPE_VIRTIO_BLK_CFG_CAPACITY_HI 0x04u
#define HYPE_VIRTIO_BLK_CFG_SIZE_MAX 0x08u
#define HYPE_VIRTIO_BLK_CFG_SEG_MAX 0x0Cu
#define HYPE_VIRTIO_BLK_CFG_GEOMETRY 0x10u
#define HYPE_VIRTIO_BLK_CFG_BLK_SIZE 0x14u
#define HYPE_VIRTIO_BLK_CFG_SIZE 0x18u /* 24 bytes total */

/* virtq_desc flags (spec §2.6.5). */
#define HYPE_VIRTQ_DESC_F_NEXT 0x0001u
#define HYPE_VIRTQ_DESC_F_WRITE 0x0002u
#define HYPE_VIRTQ_DESC_F_INDIRECT 0x0004u

/* virtio_blk_req header "type" field values (spec §5.2.6). */
#define HYPE_VIRTIO_BLK_T_IN 0u  /* read */
#define HYPE_VIRTIO_BLK_T_OUT 1u /* write */
#define HYPE_VIRTIO_BLK_T_FLUSH 4u
/*
 * #310: GET_ID -- the driver asks the device for its serial-number string.
 *
 * The vendored edk2 tree's IndustryStandard/VirtioBlk.h predates this request type (its list
 * stops at VIRTIO_BLK_T_FLUSH_OUT), so unlike most formats in this project there is no in-tree
 * primary source for it; the values come from the OASIS VIRTIO spec §5.2.6, where the field is
 * VIRTIO_BLK_ID_BYTES = 20 bytes of ASCII, NUL-PADDED rather than NUL-terminated (a full
 * 20-character serial fills the field with no terminator).
 *
 * FreeBSD's vtblk issues it during attach and complains when it fails --
 * "error getting device identifier: 45" (ENOTSUP, the direct translation of S_UNSUPP). Linux's
 * virtio_blk issues it too but swallows the failure, which is why this went unnoticed.
 */
#define HYPE_VIRTIO_BLK_T_GET_ID 8u
#define HYPE_VIRTIO_BLK_ID_BYTES 20u

/* virtio_blk_req status byte values (spec §5.2.6). */
#define HYPE_VIRTIO_BLK_S_OK 0x00u
#define HYPE_VIRTIO_BLK_S_IOERR 0x01u
#define HYPE_VIRTIO_BLK_S_UNSUPP 0x02u

#define HYPE_VIRTIO_BLK_SECTOR_SIZE 512u

/*
 * This project's own single-BAR layout for the four virtio-pci
 * capability regions (COMMON_CFG/NOTIFY_CFG/ISR_CFG/DEVICE_CFG) -- the
 * spec itself doesn't mandate a specific BAR/offset scheme, just that
 * a PCI capability points at wherever each region actually lives
 * (spec §4.1.4); this is this implementation's own choice, shared
 * between the exempt NPF glue (arch/x86_64/svm/svm_vcpu.c, which
 * dispatches by these sub-offsets) and whatever builds the device's
 * own PCI capability list bytes at setup time.
 */
#define HYPE_VIRTIO_BLK_BAR_COMMON_CFG_OFFSET 0x0000u
#define HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_OFFSET 0x1000u
#define HYPE_VIRTIO_BLK_BAR_NOTIFY_CFG_MULTIPLIER 4u
#define HYPE_VIRTIO_BLK_BAR_ISR_CFG_OFFSET 0x2000u
#define HYPE_VIRTIO_BLK_BAR_DEVICE_CFG_OFFSET 0x3000u
/* Power-of-two BAR size (hype_pci_set_bar_size()'s own requirement)
 * covering all four regions with headroom. */
#define HYPE_VIRTIO_BLK_BAR_SIZE 0x4000u

/* This project's own single-virtqueue scope: num_queues is always 1,
 * queue_select must be 0 for the queue registers to mean anything (any
 * other value reads back all-zero, "no such queue" -- the standard,
 * spec-legitimate convention for a queue index beyond num_queues). */
typedef struct {
    uint32_t device_feature_select;
    uint32_t driver_feature_select;
    uint64_t driver_features; /* accumulated across both 32-bit write halves */
    uint8_t device_status;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_enable;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
    uint8_t isr_status;
    uint64_t capacity_sectors; /* device config: capacity, in 512-byte sectors */
    /* Device-internal bookkeeping, not exposed through any register a
     * driver reads: how many avail-ring entries this device has
     * already consumed, so a NOTIFY only processes genuinely new
     * chains. Real hardware keeps the equivalent of this privately
     * too -- it's not part of the virtio wire format. */
    uint16_t last_avail_idx;
    /*
     * #310: the GET_ID serial string, exactly HYPE_VIRTIO_BLK_ID_BYTES of NUL-padded ASCII.
     *
     * Device IDENTITY, not negotiation state, so it is set by hype_virtio_blk_reset() and NOT
     * by reset_negotiation_state() -- a driver writing device_status = 0 resets the device it
     * is talking to, and a disk whose serial changed under that write would look to the guest
     * like the disk had been swapped mid-boot.
     */
    uint8_t serial[HYPE_VIRTIO_BLK_ID_BYTES];

    /*
     * #372: the guest's PCI Bus Master Enable, mirrored in so this model stays PCI-free.
     *
     * A virtio-blk device reaches the virtqueue, the descriptors and every data buffer by
     * mastering the bus, exactly as the AHCI controller does -- so with the bit clear a queue
     * notify must produce nothing. Defaults to enabled for the same reason as the AHCI model:
     * the microtests drive this with no PCI at all. See devices/ahci.h for the full argument.
     */
    int bus_master;
} hype_virtio_blk_t;

/*
 * Resets to post-power-on state: device_status=0 (driver must reset-
 * then-negotiate from scratch), queue_size defaults to this project's
 * own fixed maximum (HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX), capacity fixed to
 * `capacity_sectors` (this milestone's own scope: capacity is a
 * construction-time property of the backing buffer, not guest-
 * settable -- mirrors hype_atapi_reset()'s own `media`/`media_size`
 * parameters).
 */
/*
 * Virtqueue size this device advertises (and the ceiling it clamps a driver's
 * own request to).
 *
 * #265: this was 8, and 8 is why the write path looked latency-bound with
 * nothing to be done about it. A virtio-blk request needs a MINIMUM of three
 * descriptors -- header, one data segment, status -- so an 8-entry queue can
 * hold at most floor(8/3) = 2 requests. A measured queue depth of "max 2, mean
 * 1.30" therefore said nothing whatsoever about how deeply the guest wanted to
 * queue: it was this constant, reflected back.
 *
 * That mattered because the depth measurement was taken to decide whether
 * coalescing adjacent requests could help the physical write path, and a depth
 * of 2 says it cannot. The number was real and the reading of it was wrong --
 * the ceiling was hype's, not the guest's.
 *
 * 256 matches what QEMU's own virtio-blk advertises, so a Linux guest here
 * queues the way it would against a real device. The rings themselves live in
 * GUEST memory and are allocated by the driver, so this costs hype nothing per
 * queue; what it costs is that anything sizing its own rings from this constant
 * must actually do so (see g_m5_1_* in boot/main.c) rather than hardcode 8.
 */
#define HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX 256u

void hype_virtio_blk_reset(hype_virtio_blk_t *dev, uint64_t capacity_sectors);

/*
 * #310: set the GET_ID serial string. `name` is NUL-terminated ASCII; up to
 * HYPE_VIRTIO_BLK_ID_BYTES characters are taken and the remainder of the field is NUL-padded,
 * per the spec's fixed-width field. Longer names are truncated rather than refused -- the field
 * is a fixed 20 bytes and a caller cannot make it bigger.
 *
 * Optional: hype_virtio_blk_reset() already installs a default, so a caller that does not care
 * about per-VM distinctness gets a valid, stable serial for free.
 */
void hype_virtio_blk_set_serial(hype_virtio_blk_t *dev, const char *name);

/* #372: mirror the guest's PCI Bus Master Enable in. With it clear, a queue notify walks nothing:
 * the descriptors are unreachable to a device that cannot master the bus. */
void hype_virtio_blk_set_bus_master(hype_virtio_blk_t *dev, int enabled);

/*
 * Reads/writes the common configuration register at `offset` (byte
 * offset within the region a VIRTIO_PCI_CAP_COMMON_CFG capability
 * points at). `size_bytes` must exactly match each register's own
 * real width (4 for every 32-bit field, 2 for 16-bit, 1 for 8-bit) --
 * a real driver never accesses these with any other width, and this
 * project rejects a mismatched width rather than guessing intent.
 * An in-range offset that isn't one of the defined registers reads as
 * 0 / ignores the write (the same "reserved reads as 0" convention
 * devices/ahci.h's own MMIO model already uses). Returns 0 on success,
 * -1 for an out-of-range offset or a width mismatch.
 */
int hype_virtio_blk_common_cfg_read(const hype_virtio_blk_t *dev, uint32_t offset, uint8_t size_bytes,
                                     uint32_t *out_value);
int hype_virtio_blk_common_cfg_write(hype_virtio_blk_t *dev, uint32_t offset, uint8_t size_bytes,
                                      uint32_t value);

/*
 * Reads the device-specific configuration register at `offset` (same
 * width/range rules as the common-cfg accessors above). Entirely
 * read-only -- capacity/size_max/seg_max/geometry/blk_size are fixed
 * device properties a driver only ever reads.
 */
int hype_virtio_blk_device_cfg_read(const hype_virtio_blk_t *dev, uint32_t offset, uint8_t size_bytes,
                                     uint32_t *out_value);

/*
 * Reads the 1-byte ISR status register -- real hardware semantics:
 * the read itself clears the pending interrupt status (`isr_status`
 * is reset to 0 as a side effect of this call, matching the spec's
 * own "reading this register returns the reason for the interrupt and
 * clears it" wording, §4.1.4.5).
 */
uint8_t hype_virtio_blk_isr_read(hype_virtio_blk_t *dev);

/*
 * True once the device is genuinely ready for I/O: DRIVER_OK is set,
 * the single queue is enabled, and its size/descriptor-table address
 * are nonzero. The exempt NPF glue (arch/x86_64/svm/svm_vcpu.c) uses
 * this to decide whether a queue-notify write should actually walk
 * the virtqueue at all, rather than processing a notification that
 * arrived before the driver finished setup.
 */
int hype_virtio_blk_is_queue_ready(const hype_virtio_blk_t *dev);

/* A single virtq_desc entry (spec §2.6.5), 16 bytes on the wire. Pure
 * bit extraction, no guest-memory access -- the caller has already
 * read these bytes out of guest memory (mirrors
 * hype_ahci_decode_cmd_header()'s own split). */
typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} hype_virtq_desc_t;

void hype_virtq_decode_desc(const uint8_t raw[16], hype_virtq_desc_t *out);


/*
 * #265 step 3: queue-depth instrumentation -- how many requests were already
 * pending when the guest kicked the queue.
 *
 * This ONE number decides between two opposite fixes, which is why it is
 * measured before either is built (the ticket's own measure-first rule).
 *
 * The physical write path keeps a single AHCI command in flight and spins for
 * it, so throughput is 1/latency. If the guest always has exactly ONE request
 * pending per kick, it is waiting for each completion before submitting the
 * next -- there is never a second request to merge with, coalescing adjacent
 * requests cannot help at all, and the only lever is per-command latency. If
 * the depth is routinely greater than one, then adjacent requests can be
 * gathered into a single multi-PRDT AHCI command, turning N round trips into
 * one -- the real fix for a latency-bound path.
 *
 * The write-size histogram (hype_blk_wstats) already established that requests
 * are small; it cannot distinguish these two cases, because "many 1-sector
 * writes" looks identical whether they arrive one at a time or forty at a time.
 *
 * Aggregate across VMs and updated without locking, for the same reason the
 * write stats are: a lost increment on a diagnostic beats a lock on the I/O
 * path.
 */
#define HYPE_VIRTIO_BLK_DEPTH_BUCKETS 6u

typedef struct {
    uint64_t kicks;      /* queue notifies that found at least one new chain */
    uint64_t chains;     /* total chains drained across those kicks */
    uint32_t max_depth;  /* deepest single kick */
    uint32_t hist[HYPE_VIRTIO_BLK_DEPTH_BUCKETS];
} hype_virtio_blk_depth_t;

/*
 * Bucket index for a kick that drained `depth` chains:
 *   0: 1  (the decisive case -- nothing to coalesce)
 *   1: 2-3   2: 4-7   3: 8-15   4: 16-31   5: >=32
 * depth==0 is not a recorded kick and maps to bucket 0. Pure.
 */
unsigned hype_virtio_blk_depth_bucket(uint32_t depth);
void hype_virtio_blk_depth_reset(hype_virtio_blk_depth_t *d);
/* Fold one kick that drained `depth` chains into `d`. A depth of 0 is ignored:
 * a notify that found no new work says nothing about how deep the guest queues. */
void hype_virtio_blk_depth_record(hype_virtio_blk_depth_t *d, uint32_t depth);
/* Mean chains per kick, scaled by 100 so it needs no float (freestanding, and
 * hype_debug_print has no %f). 250 means 2.50 chains per kick. */
uint32_t hype_virtio_blk_depth_mean_x100(const hype_virtio_blk_depth_t *d);
/* The process-wide depth stats process_virtio_blk_queue() folds into. */
hype_virtio_blk_depth_t *hype_virtio_blk_depth(void);

/* Drains a virtio-blk virtqueue: walks the available ring, executes each
 * request against the block backend, DMAs data to/from guest RAM (dma_map;
 * 0 = identity), and posts completions to the used ring. Vendor-neutral; the
 * SVM and VMX virtio-blk MMIO handlers both call it when a queue is kicked.
 * Defined in arch/x86_64/svm/svm_vcpu.c. Returns 0 on success, -1 on error. */
int process_virtio_blk_queue(hype_virtio_blk_t *dev, const hype_blk_backend_t *be,
                             const hype_gpa_map_t *dma_map);

/*
 * #268: where request-rejection diagnostics go. NULL (the default) means
 * hype_debug_print, rate-limited to the first few so one bad guest cannot bury
 * the rest of the log.
 *
 * Injectable for two reasons. It is what makes the chain walk host-testable at
 * all -- hype_debug_print reaches the real UART through port I/O, which faults
 * in a user process -- and it lets a test assert on WHY a request was refused
 * rather than only on the status byte the guest sees. Installing a sink also
 * resets the rate-limit counter, so each test starts from a clean slate.
 */
void hype_virtio_blk_set_reject_sink(void (*sink)(const char *why));

#endif /* HYPE_DEVICES_VIRTIO_BLK_H */
