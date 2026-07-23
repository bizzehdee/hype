#ifndef HYPE_CORE_NVME_HOST_H
#define HYPE_CORE_NVME_HOST_H

#include <stdint.h>

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
#define HYPE_NVME_IO_READ 0x02u
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
 * Builds a 64-byte IDENTIFY Submission Queue Entry: opcode 0x06, `cns`
 * (0=namespace, 1=controller), `nsid`, result buffer PRP1. Zeroes first. Pure.
 */
void hype_nvme_build_identify_sqe(uint8_t sqe[64], uint16_t cid, uint32_t cns, uint32_t nsid,
                                  uint64_t prp1);

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

#endif /* HYPE_CORE_NVME_HOST_H */
