#ifndef HYPE_DEVICES_VIRTIO_BLK_H
#define HYPE_DEVICES_VIRTIO_BLK_H

#include <stdint.h>

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

/* The one transport feature bit this project offers -- VIRTIO_F_VERSION_1
 * (bit 32 of the 64-bit feature space, i.e. bit 0 of the HIGH 32-bit
 * half a driver reads via device_feature_select=1). Offering zero
 * optional VIRTIO_BLK_F_* bits is spec- and driver-confirmed
 * sufficient for Linux's own virtio_blk to probe and bind (every
 * optional feature's absence is a safe, already-handled fallback
 * path in the real driver) -- deliberately minimal, matching this
 * project's "primitive now, integration later" bias. */
#define HYPE_VIRTIO_F_VERSION_1_BIT 32u

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
#define HYPE_VIRTIO_BLK_QUEUE_SIZE_MAX 8u

void hype_virtio_blk_reset(hype_virtio_blk_t *dev, uint64_t capacity_sectors);

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

#endif /* HYPE_DEVICES_VIRTIO_BLK_H */
