#ifndef HYPE_DEVICES_NVME_H
#define HYPE_DEVICES_NVME_H

#include <stdint.h>

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

#endif /* HYPE_DEVICES_NVME_H */
