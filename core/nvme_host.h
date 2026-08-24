#ifndef HYPE_CORE_NVME_HOST_H
#define HYPE_CORE_NVME_HOST_H

#include <stdint.h>
#include "blk_backend.h" /* #715: hype_blk_seg_t, for hype_nvme_gather_segs() */

/*
 * M10-1b (#194, split from M10-1): a minimal HOST-side NVMe-over-PCIe driver, so
 * hype can read raw LBAs off a physical NVMe SSD post-ExitBootServices -- the
 * NVMe counterpart to core/ahci_host.c. Fills the same
 * (ctx, lba, count, dst) sector-read contract core/gpt.c / core/iso_stream.c /
 * core/blk_phys.c already consume, so NVMe media works through the existing
 * streaming/block stack unchanged.
 *
 * This header declares the pure, unit-testable pieces: the 64-byte Submission
 * Queue Entry encoders (READ, IDENTIFY), the 16-byte Completion Queue Entry
 * decoders, the IDENTIFY-NAMESPACE parser, and the CAP/doorbell helpers. The
 * register-poking bring-up (map BAR0, enable the controller, create the admin +
 * I/O queues, ring doorbells, poll the CQ) lives in the hardware shim
 * (nvme_host_hw.c), coverage-exempt like ahci_host_hw.c. Field layouts are the
 * NVMe base spec (rev 1.4) §3 (registers), §4.1/§4.2 (SQE/CQE), and the
 * IDENTIFY NAMESPACE data structure (§5.15.2).
 */

/* Controller register offsets (BAR0 MMIO), NVMe 1.4 §3.1. */
#define HYPE_NVME_REG_CAP 0x00u  /* Controller Capabilities (64-bit) */
#define HYPE_NVME_REG_CC 0x14u   /* Controller Configuration (32-bit) */
#define HYPE_NVME_REG_CSTS 0x1Cu /* Controller Status (32-bit) */
#define HYPE_NVME_REG_AQA 0x24u  /* Admin Queue Attributes */
#define HYPE_NVME_REG_ASQ 0x28u  /* Admin Submission Queue base (64-bit) */
#define HYPE_NVME_REG_ACQ 0x30u  /* Admin Completion Queue base (64-bit) */
#define HYPE_NVME_REG_DOORBELL_BASE 0x1000u

#define HYPE_NVME_SQE_SIZE 64u
#define HYPE_NVME_CQE_SIZE 16u
#define HYPE_NVME_SECTOR_SIZE 512u

/* Admin + I/O opcodes used here. */
#define HYPE_NVME_ADM_CREATE_IO_SQ 0x01u
#define HYPE_NVME_ADM_CREATE_IO_CQ 0x05u
#define HYPE_NVME_ADM_IDENTIFY 0x06u
#define HYPE_NVME_ADM_SET_FEATURES 0x09u
#define HYPE_NVME_FEAT_NUM_QUEUES 0x07u
#define HYPE_NVME_IO_READ 0x02u
#define HYPE_NVME_IO_WRITE 0x01u
#define HYPE_NVME_CNS_NAMESPACE 0x00u
#define HYPE_NVME_CNS_CONTROLLER 0x01u

/* CC field shifts (NVMe 1.4 §3.1.5). IOSQES/IOCQES are log2 of the entry size:
 * 6 => 64-byte SQE, 4 => 16-byte CQE. */
#define HYPE_NVME_CC_EN (1u << 0)
#define HYPE_NVME_CSTS_RDY (1u << 0)

/*
 * Builds a 64-byte READ (I/O) Submission Queue Entry: opcode 0x02, command id
 * `cid`, namespace `nsid`, data buffer PRP1/PRP2, starting LBA `slba`, and
 * `nlb_0based` = (blocks - 1) in CDW12[15:0] (NVMe's 0-based count). Zeroes the
 * whole entry first. Pure.
 */
void hype_nvme_build_read_sqe(uint8_t sqe[64], uint16_t cid, uint32_t nsid, uint64_t slba,
                              uint16_t nlb_0based, uint64_t prp1, uint64_t prp2);

/*
 * M10-1c (#197): builds a 64-byte WRITE (I/O) Submission Queue Entry -- identical
 * field layout to READ but opcode 0x01 (data flows guest/host -> device). Zeroes
 * the whole entry first. Pure.
 */
void hype_nvme_build_write_sqe(uint8_t sqe[64], uint16_t cid, uint32_t nsid, uint64_t slba,
                               uint16_t nlb_0based, uint64_t prp1, uint64_t prp2);

/*
 * #715: gathers `len` bytes starting at byte offset `off` into the logical concatenation of
 * `segs` (nsegs entries, each segs[i].count 512-byte sectors from segs[i].buf) into `dst`.
 *
 * The write path's own vectored command (hype_nvme_host_writev) needs this because NVMe's PRP
 * mechanism requires every entry PAST THE FIRST to be page-aligned -- true of the read/write
 * path's existing single bounce buffer, but not of an arbitrary virtio-blk segment boundary
 * (#295's own comment on hype_blk_seg_t: "the guest's pages are scattered", not page-aligned by
 * construction). AHCI's PRDT has no such restriction, which is why hype_ahci_host_writev can
 * point straight at the scattered guest buffers and this can't; gathering into one contiguous
 * bounce region first turns the same segments into a shape the existing (already-correct)
 * single-buffer PRP1/PRP2/PRP-list construction can use unchanged.
 *
 * Returns 0 on success, -1 if [off, off+len) runs past the segments' total byte length -- a
 * caller bug (the caller already bounds-checked the WHOLE vectored write against the backend's
 * capacity before building `segs`), never a guest-triggerable condition. Pure.
 */
int hype_nvme_gather_segs(const hype_blk_seg_t *segs, uint32_t nsegs, uint64_t off, uint32_t len,
                          void *dst);

/*
 * Builds a 64-byte IDENTIFY Submission Queue Entry: opcode 0x06, `cns`
 * (0=namespace, 1=controller), `nsid`, result buffer PRP1. Zeroes first. Pure.
 */
void hype_nvme_build_identify_sqe(uint8_t sqe[64], uint16_t cid, uint32_t cns, uint32_t nsid,
                                  uint64_t prp1);

/*
 * #255: Set Features -- Number of Queues (FID 0x07). The host is required to
 * NEGOTIATE how many I/O queues it will use before creating any; QEMU forgives
 * skipping this, but real controllers may accept Create I/O CQ/SQ and then
 * never service the queue (observed on SK hynix 1c5c:1d59: I/O CQE never
 * posted, admin path fine). CDW11 = (NCQR-1)<<16 | (NSQR-1). The completion's
 * DWORD0 reports how many the controller actually allocated, same encoding.
 */
void hype_nvme_build_set_num_queues_sqe(uint8_t sqe[64], uint16_t cid, uint16_t nsq,
                                        uint16_t ncq);

/* Completion entry DWORD0 (command-specific result -- e.g. Set Features
 * Number of Queues returns the allocated counts here). */
uint32_t hype_nvme_cqe_dw0(const uint8_t cqe[16]);

/* CQE decoders (NVMe 1.4 §4.6). The status word is CQE bytes 14-15: bit0 is the
 * phase tag, bits 15:1 are the Status Field (Status Code + Status Code Type). */
int hype_nvme_cqe_phase(const uint8_t cqe[16]);       /* phase-tag bit */
int hype_nvme_cqe_success(const uint8_t cqe[16]);     /* 1 if Status Field == 0 */
uint16_t hype_nvme_cqe_cid(const uint8_t cqe[16]);    /* command id echoed back */

/*
 * Parses an IDENTIFY NAMESPACE (CNS=0) 4096-byte structure: NSZE (total logical
 * blocks, byte 0) into *total_blocks, and the in-use LBA format's block size
 * (FLBAS[3:0] selects an LBAF entry at byte 128+idx*4; LBADS bits 23:16 = log2
 * of the block size) into *block_bytes. Returns 0 on success, -1 if NSZE is 0
 * or the block size is implausible. Pure.
 */
int hype_nvme_parse_identify_ns(const uint8_t idns[4096], uint64_t *total_blocks,
                                uint32_t *block_bytes);

/*
 * M10-6a (#227): parses the ASCII Serial Number (SN, bytes 4..23) and Model
 * Number (MN, bytes 24..63) out of an IDENTIFY CONTROLLER (CNS=1) 4096-byte
 * structure -- the fields the `physical:` target-disk guard (#124) keys on for
 * an NVMe drive, complementing the AHCI ATA serial (hype_ahci_host_parse_
 * identify). Both are left-justified, space-padded fixed fields; this trims
 * surrounding whitespace and NUL-terminates. serial_out must be >= 21 bytes,
 * model_out >= 41 bytes; either may be NULL to skip that field. Pure.
 */
void hype_nvme_parse_identify_ctrl(const uint8_t idctrl[4096], char serial_out[21],
                                   char model_out[41]);

/* CAP helpers (NVMe 1.4 §3.1.1): DSTRD (doorbell stride) is CAP[35:32]; the
 * doorbell for queue `qid` (is_cq selects the completion doorbell) sits at
 * DOORBELL_BASE + (2*qid + is_cq) * (4 << DSTRD). */
uint32_t hype_nvme_cap_dstrd(uint64_t cap);
uint32_t hype_nvme_doorbell_offset(uint32_t qid, int is_cq, uint32_t dstrd);

/* --- hardware bring-up (coverage-exempt shim; real MMIO) --- */

/*
 * Brings up the NVMe controller at `abar_phys` (BAR0, identity-mapped): resets +
 * enables it, creates the admin and one I/O queue pair, and IDENTIFYs namespace
 * 1. On success stores the namespace geometry for hype_nvme_host_read and
 * returns 0; returns -1 on timeout, a controller error, or a namespace whose
 * logical block size is not 512 bytes (the only size the 512-sector callers
 * support today). Post-ExitBootServices only.
 */
int hype_nvme_host_init(uint64_t abar_phys);

/*
 * Reads `count` 512-byte sectors starting at LBA `lba` from the initialised
 * controller into `dst` (a 512*count-byte, identity-mapped host buffer), via a
 * single I/O READ command polled to completion. Returns 0 on success, -1 on
 * timeout or an NVMe error. Signature matches hype_ahci_host_read.
 */
int hype_nvme_host_read(uint64_t abar_phys, uint64_t lba, uint16_t count, void *dst);

/*
 * M10-1c (#197): writes `count` 512-byte sectors from `src` to LBA `lba` on the
 * initialised controller, via I/O WRITE commands polled to completion (DMA
 * bounced through the page-aligned host buffer, PRP1/PRP2/PRP-list like the
 * read path). Returns 0 on success, -1 on timeout or an NVMe error. DESTRUCTIVE
 * -- gated by the §6d/phys_guard policy at the caller, never issued blindly.
 * Signature matches hype_ahci_host_write.
 */
int hype_nvme_host_write(uint64_t abar_phys, uint64_t lba, uint16_t count, const void *src);

/*
 * #715: the bounce buffer's own capacity, in 512-byte sectors -- 128 (64 KiB), matching
 * BOUNCE_SECTORS in nvme_host_hw.c. Exposed so blk_phys_hw.c's hype_blk_phys_enable_writev()
 * call can bound a single hype_nvme_host_writev() command to what the bounce buffer can hold in
 * one shot, the same way HYPE_AHCI_HOST_SG_MAX_PRDT bounds a single AHCI command's PRDT count.
 */
#define HYPE_NVME_WRITEV_MAX_SECTORS 128u

/*
 * #715: writes the `nsegs` scattered segments of `segs` (hype_blk_seg_t, core/blk_backend.h) as
 * ONE I/O WRITE command to LBA `lba` on the initialised controller -- the vectored sibling of
 * hype_nvme_host_write(), same shape as hype_ahci_host_writev(). Segments are gathered into the
 * existing bounce buffer first (hype_nvme_gather_segs()) rather than PRP-listed directly, because
 * NVMe's PRP mechanism requires page alignment past the first entry, which an arbitrary
 * virtio-blk segment boundary cannot guarantee (see hype_nvme_gather_segs()'s own comment).
 * Callers must keep the total sector count at or under HYPE_NVME_WRITEV_MAX_SECTORS -- one
 * bounce-buffer's worth -- exactly what hype_blk_phys_enable_writev()'s max_sectors cap already
 * guarantees when this is registered as the writev callback. Returns 0 on success, -1 on a
 * timeout, an NVMe error, or a total exceeding the bounce buffer's capacity. DESTRUCTIVE -- same
 * §6d/phys_guard gating as hype_nvme_host_write().
 */
int hype_nvme_host_writev(uint64_t abar_phys, uint64_t lba, const hype_blk_seg_t *segs,
                          uint32_t nsegs);

/*
 * #660: records which core is the BSP, mirroring hype_ahci_host_set_bsp_apic()/
 * hype_blk_usb_set_bsp_apic() -- so the shared queue/bounce-buffer lock can give the BSP a
 * bounded wait instead of the unbounded one guest AP callers get. Call once, from the same place
 * the other two are latched.
 */
void hype_nvme_host_set_bsp_apic(unsigned int apic_id);

/* Nonzero after a real-HW run means the BSP hit its bounded lock budget at least once -- the
 * NVMe counterpart of hype_ahci_host_bsp_lock_timeouts()/hype_blk_usb_bsp_lock_timeouts(). */
unsigned long long hype_nvme_host_bsp_lock_timeouts(void);

/* Total times a caller found the lock already held -- proves the lock is actually being
 * exercised under real contention rather than assumed dormant (#660 acceptance criterion 3). */
unsigned long long hype_nvme_host_lock_contended(void);

/*
 * M10-6a (#227): the identity + capacity captured by the last successful
 * hype_nvme_host_init() -- the fields a `physical:` NVMe target needs (serial
 * for the #124 guard match, total 512-byte sectors for the block backend's
 * capacity bound). serial_out >= 21 bytes, model_out >= 41 bytes (either may be
 * NULL). hype_nvme_host_total_sectors() returns 0 before a successful init.
 */
void hype_nvme_host_identity(char serial_out[21], char model_out[41]);
uint64_t hype_nvme_host_total_sectors(void);

#endif /* HYPE_CORE_NVME_HOST_H */
