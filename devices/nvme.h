#ifndef HYPE_DEVICES_NVME_H
#define HYPE_DEVICES_NVME_H

#include <stdint.h>
#include "../core/blk_backend.h"

/*
 * M5-10 (#202): a GUEST-FACING NVMe controller model -- what a VM's own driver talks to.
 *
 * Not to be confused with core/nvme_host.c, which is how hype reaches a REAL NVMe disk. The two are
 * unrelated directions: one answers commands, the other issues them. (The same distinction #325 draws
 * between hype's guest ATAPI model and a host-side ATAPI driver.)
 *
 * Design constraints established by the #335 spike, from OVMF's own NvmExpressDxe source:
 *
 *   - INTx-only is sufficient. The driver contains NO interrupt machinery: it POLLS the completion
 *     queue's phase tag, and creates its I/O completion queues with Ien = 0. So this model needs no
 *     MSI/MSI-X and need never deliver an interrupt to be usable by firmware.
 *   - The PHASE TAG is therefore load-bearing. If it is wrong, a polling driver simply never observes
 *     completion and spins to its timeout with NO other symptom -- no error, no fault, nothing in a
 *     log. That is why the phase-tag rule is modelled explicitly here and unit-tested before any of
 *     the queue plumbing exists.
 *   - INTMS/INTMC are never read by the firmware, so they are accepted and ignored rather than
 *     modelled.
 *
 * This header is slice 1: the register block and the phase-tag rule. Queues, IDENTIFY and I/O
 * READ/WRITE land on top of it.
 */

/* Register offsets in BAR0 (NVMe 1.4 §3.1). */
#define HYPE_NVME_REG_CAP 0x00u    /* Controller Capabilities (64-bit, RO) */
#define HYPE_NVME_REG_VS 0x08u     /* Version (RO) */
#define HYPE_NVME_REG_INTMS 0x0Cu  /* Interrupt Mask Set -- accepted, ignored (see above) */
#define HYPE_NVME_REG_INTMC 0x10u  /* Interrupt Mask Clear -- accepted, ignored */
#define HYPE_NVME_REG_CC 0x14u     /* Controller Configuration */
#define HYPE_NVME_REG_CSTS 0x1Cu   /* Controller Status */
#define HYPE_NVME_REG_AQA 0x24u    /* Admin Queue Attributes */
#define HYPE_NVME_REG_ASQ 0x28u    /* Admin Submission Queue base (64-bit) */
#define HYPE_NVME_REG_ACQ 0x30u    /* Admin Completion Queue base (64-bit) */
#define HYPE_NVME_REG_DOORBELL_BASE 0x1000u

#define HYPE_NVME_CC_EN (1u << 0)
#define HYPE_NVME_CSTS_RDY (1u << 0)
#define HYPE_NVME_CSTS_CFS (1u << 1) /* Controller Fatal Status */

/*
 * Doorbell stride: CAP.DSTRD is expressed as (2 ^ (2 + DSTRD)) bytes. 0 means the minimum, 4 bytes,
 * which is what every driver handles and what keeps the doorbell index arithmetic trivial.
 */
#define HYPE_NVME_CAP_DSTRD 0u
#define HYPE_NVME_DOORBELL_STRIDE 4u

/* One admin pair plus one I/O pair is the whole scope (#202). */
#define HYPE_NVME_MAX_QUEUES 2u
/* CAP.MQES is "max queue entries MINUS ONE", a classic off-by-one to get wrong in both directions. */
#define HYPE_NVME_QUEUE_ENTRIES 64u
#define HYPE_NVME_CAP_MQES (HYPE_NVME_QUEUE_ENTRIES - 1u)

typedef struct {
    uint32_t cc;
    uint32_t csts;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;

    /* Per-queue head/tail as the guest has moved them via doorbells. */
    uint32_t sq_tail[HYPE_NVME_MAX_QUEUES];
    uint32_t cq_head[HYPE_NVME_MAX_QUEUES];
    /*
     * The controller's own notion of where it will post the next completion, and the phase tag it
     * will stamp on it. Kept per queue because each completion queue wraps independently.
     */
    uint32_t cq_tail[HYPE_NVME_MAX_QUEUES];
    uint8_t cq_phase[HYPE_NVME_MAX_QUEUES];
    /*
     * #202 slice 5: how far the CONTROLLER has consumed each submission queue. Distinct from
     * sq_tail (where the GUEST says it has written to) -- conflating the two is how a controller
     * either re-executes commands it already ran or skips ones it never did.
     */
    uint32_t sq_head[HYPE_NVME_MAX_QUEUES];
    /* I/O queue bases, as recorded by CREATE_IO_SQ/CQ. Queue 0 uses asq/acq. */
    uint64_t io_sq_base;
    uint64_t io_cq_base;
} hype_nvme_t;

/*
 * Resets to the power-on state: not ready, no queues, and every completion queue's phase tag at 1.
 *
 * Phase starting at 1 is not arbitrary. A driver zeroes its completion queue memory and then waits for
 * an entry whose phase differs from its own expectation, which starts at 1 -- so a controller that
 * posted its first completion with phase 0 would be indistinguishable from "nothing has been posted",
 * and the driver would poll forever.
 */
void hype_nvme_reset(hype_nvme_t *dev);

/* MMIO reads/writes against BAR0. Offsets outside the modelled set read 0 and ignore writes rather
 * than faulting: a driver probing for optional features must not take the VM down. */
uint32_t hype_nvme_mmio_read32(const hype_nvme_t *dev, uint32_t off);
void hype_nvme_mmio_write32(hype_nvme_t *dev, uint32_t off, uint32_t value);

/*
 * Advances the controller's completion-queue producer by one and returns the phase tag to stamp on
 * the entry just produced.
 *
 * The rule, and the reason this exists as its own tested function: the phase tag TOGGLES when the
 * producer wraps, not on every entry. A driver detects a new completion purely by the phase differing
 * from what it expects, so toggling too often makes half the completions invisible and never toggling
 * makes every completion after the first wrap invisible -- both silent.
 */
uint8_t hype_nvme_cq_advance(hype_nvme_t *dev, unsigned int qid);

/*
 * Decodes a doorbell write offset into (queue id, is_completion_queue). Returns 0 on success.
 *
 * VALID-3: an out-of-range or misaligned doorbell offset is REFUSED rather than clamped -- a guest
 * that writes a doorbell hype does not model must not silently move some other queue's index.
 */
int hype_nvme_doorbell_decode(uint32_t off, unsigned int *out_qid, int *out_is_cq);


/* ---- slice 2: admin commands (#202) ---------------------------------------------------------- */

/* Admin opcodes hype answers. Anything else is completed with INVALID_OPCODE rather than ignored: a
 * driver that gets silence waits forever, whereas a status it understands makes it move on. */
#define HYPE_NVME_ADMIN_CREATE_IO_SQ 0x01u
#define HYPE_NVME_ADMIN_CREATE_IO_CQ 0x05u
#define HYPE_NVME_ADMIN_IDENTIFY 0x06u
#define HYPE_NVME_ADMIN_SET_FEATURES 0x09u
#define HYPE_NVME_ADMIN_GET_FEATURES 0x0Au

/* I/O opcodes. */
#define HYPE_NVME_IO_WRITE 0x01u
#define HYPE_NVME_IO_READ 0x02u

/* IDENTIFY CNS values. */
#define HYPE_NVME_CNS_NAMESPACE 0x00u
#define HYPE_NVME_CNS_CONTROLLER 0x01u

/* Status codes (SCT=0 generic, in the CQE's status field). */
#define HYPE_NVME_SC_SUCCESS 0x00u
#define HYPE_NVME_SC_INVALID_OPCODE 0x01u
#define HYPE_NVME_SC_INVALID_FIELD 0x02u
#define HYPE_NVME_SC_DATA_XFER_ERROR 0x04u
#define HYPE_NVME_SC_LBA_OUT_OF_RANGE 0x80u

#define HYPE_NVME_SQE_BYTES 64u
#define HYPE_NVME_CQE_BYTES 16u
/* Both queue entry sizes are advertised as 2^6 = 64 / 2^4 = 16 in IDENTIFY; see the builder. */
#define HYPE_NVME_IDENTIFY_BYTES 4096u

/* A decoded submission-queue entry. Only the fields hype acts on. */
typedef struct {
    uint8_t opcode;
    uint16_t cid;    /* command identifier -- MUST be echoed in the completion, or the driver cannot
                      * match the completion to its command and will time out on a command that in
                      * fact succeeded. */
    uint32_t nsid;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
} hype_nvme_cmd_t;

/* Decodes 64 little-endian bytes of guest-supplied SQE. Pure; the caller fetches the bytes. */
void hype_nvme_sqe_decode(const uint8_t sqe[HYPE_NVME_SQE_BYTES], hype_nvme_cmd_t *out);

/*
 * Builds the 16-byte completion entry.
 *
 * `phase` comes from hype_nvme_cq_advance(). `sq_head` tells the driver how much of its submission
 * queue the controller has consumed -- a driver uses it to decide it may reuse those slots, so a wrong
 * value causes it to either stall (believing the queue full) or overwrite commands in flight.
 */
void hype_nvme_cqe_build(uint8_t cqe[HYPE_NVME_CQE_BYTES], uint16_t cid, uint16_t sq_head,
                         uint16_t sqid, uint8_t phase, uint16_t status);

/*
 * IDENTIFY CONTROLLER (CNS=1) payload, 4096 bytes.
 *
 * `serial` is copied into the SN field SPACE-padded, not NUL-padded -- these are fixed-width ASCII
 * fields per the spec, and a NUL-padded serial shows up as garbage in a guest's device listing.
 */
void hype_nvme_identify_controller(uint8_t buf[HYPE_NVME_IDENTIFY_BYTES], const char *serial);

/*
 * IDENTIFY NAMESPACE (CNS=0) payload, 4096 bytes, for a namespace of `total_sectors` 512-byte blocks.
 *
 * The block size is expressed as a POWER OF TWO in LBAF0.LBADS (9 => 512). That indirection is the
 * thing to get right: a wrong LBADS makes every guest LBA land at the wrong byte offset, and the guest
 * will happily read and write at those offsets without complaint.
 */
void hype_nvme_identify_namespace(uint8_t buf[HYPE_NVME_IDENTIFY_BYTES], uint64_t total_sectors);


/* ---- slice 3: PRP walking (#202) -------------------------------------------------------------- */

/*
 * NVMe describes a transfer's guest buffers with PRPs (Physical Region Pages), and the rules are
 * asymmetric in a way that is easy to get subtly wrong:
 *
 *   - PRP1 may carry a byte OFFSET within its page; every subsequent PRP must be page-aligned.
 *   - If the transfer fits in PRP1 plus one more page, PRP2 IS that page.
 *   - Otherwise PRP2 points at a PRP LIST: an array of 8-byte page addresses in guest memory, whose
 *     LAST entry chains to a further list when one page of entries is not enough.
 *
 * Those two meanings of PRP2 are the trap: treating a list pointer as a data page writes the guest's
 * PRP list full of file contents, and treating a data page as a list reads addresses out of data. Both
 * corrupt guest memory rather than failing, which is why this is a separate iterator with its own
 * tests rather than inline arithmetic.
 *
 * The iterator is pure: reading list entries from guest memory is an injected callback, so the whole
 * thing is testable with no VM.
 */

/* Reads `len` bytes of guest physical memory. Returns 0 on success. The implementation is expected to
 * BOUNDS-CHECK gpa+len against the VM's mapped range (VALID-3) and refuse otherwise. */
typedef int (*hype_nvme_guest_read_fn)(void *ctx, uint64_t gpa, uint32_t len, void *dst);

typedef struct {
    uint64_t prp1;
    uint64_t prp2;
    uint32_t page_size;
    uint64_t remaining;   /* bytes of the transfer not yet yielded */
    int first;            /* PRP1 has not been consumed yet */
    int using_list;       /* PRP2 is a list pointer rather than a data page */
    uint64_t list_gpa;    /* address of the next list ENTRY to read */
    unsigned int list_left; /* entries remaining in the current list page before it chains */
} hype_nvme_prp_iter_t;

/*
 * Prepares an iterator for a `total_len`-byte transfer. Returns 0, or -1 if the request is malformed
 * (zero length, or a page size that is not a power of two).
 *
 * Whether PRP2 is a data page or a list is decided HERE, once, from the length -- not guessed per
 * segment.
 */
int hype_nvme_prp_init(hype_nvme_prp_iter_t *it, uint64_t prp1, uint64_t prp2, uint64_t total_len,
                       uint32_t page_size);

/*
 * Yields the next contiguous guest segment. Returns 1 when a segment was produced, 0 when the transfer
 * is complete, and -1 on a malformed descriptor or a failed list read.
 *
 * A misaligned continuation PRP is a REFUSAL, not something to mask off: the spec requires alignment,
 * so a misaligned entry means the list is not what hype thinks it is, and continuing would scatter the
 * transfer over addresses the guest never nominated.
 */
int hype_nvme_prp_next(hype_nvme_prp_iter_t *it, hype_nvme_guest_read_fn read_fn, void *ctx,
                       uint64_t *out_gpa, uint32_t *out_len);


/* ---- slice 4: I/O READ/WRITE (#202) ------------------------------------------------------------ */

/* Writes `len` bytes into guest physical memory. Bounds-checking gpa+len against the VM's mapped range
 * (VALID-3) is the implementation's responsibility -- this is the single point where a guest-supplied
 * address becomes a host write. */
typedef int (*hype_nvme_guest_write_fn)(void *ctx, uint64_t gpa, uint32_t len, const void *src);

/*
 * Executes one NVMe I/O command (READ 0x02 / WRITE 0x01) against `be`, returning an NVMe status code
 * (HYPE_NVME_SC_*). HYPE_NVME_SC_SUCCESS means the whole transfer completed.
 *
 * Two encodings here are the classic ways to get this wrong, and both are silent:
 *
 *   - NLB IS ZERO-BASED. CDW12's low 16 bits hold "number of logical blocks MINUS ONE", so a request
 *     for one block arrives as 0. Reading it literally transfers nothing for every single-block
 *     command -- and a driver whose read returns no data but reports success sees corrupt content, not
 *     an error.
 *   - SLBA IS 64-BIT, split across CDW10 (low) and CDW11 (high). Using only CDW10 works perfectly on
 *     any disk under 2 TiB and then wraps.
 *
 * `bounce` is caller-supplied staging of at least `page_size` bytes: data has to land somewhere host-
 * side between the backend and guest memory, and this module allocates nothing.
 *
 * Pure apart from the injected callbacks and the backend, so every path is unit tested with no VM.
 */
uint16_t hype_nvme_exec_io(const hype_nvme_cmd_t *cmd, hype_blk_backend_t *be,
                           uint64_t total_sectors, uint32_t page_size,
                           hype_nvme_guest_read_fn gread, hype_nvme_guest_write_fn gwrite,
                           void *gctx, uint8_t *bounce, uint32_t bounce_len);


/* ---- slice 5: the command processor (#202) ------------------------------------------------------ */

/* Everything the processor needs that is not controller state. Grouped so the call sites stay readable
 * and so a new dependency cannot be silently forgotten at one of them. */
typedef struct {
    hype_blk_backend_t *be;
    uint64_t total_sectors;
    uint32_t page_size;
    hype_nvme_guest_read_fn gread;
    hype_nvme_guest_write_fn gwrite;
    void *gctx;
    uint8_t *bounce;
    uint32_t bounce_len;
    const char *serial; /* reported by IDENTIFY CONTROLLER */
} hype_nvme_ctx_t;

/*
 * Drains submission queue `qid`: for every entry the guest has published (sq_head != sq_tail), fetch it,
 * execute it, and post a completion.
 *
 * Returns the number of commands processed, or -1 if the controller is not enabled or the arguments are
 * unusable. A command that FAILS still gets a completion with a status -- never silence, because a
 * driver that receives no completion waits for its timeout and cannot tell a failure from a hang.
 *
 * Pure apart from the injected callbacks and the backend.
 */
int hype_nvme_process_sq(hype_nvme_t *dev, unsigned int qid, const hype_nvme_ctx_t *c);

#endif /* HYPE_DEVICES_NVME_H */
