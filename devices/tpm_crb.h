#ifndef HYPE_DEVICES_TPM_CRB_H
#define HYPE_DEVICES_TPM_CRB_H

#include <stdint.h>
#include "../core/tpm2.h"

/*
 * #433: the TPM 2.0 CRB (Command Response Buffer) interface at the architectural 0xFED40000 --
 * the register file a Windows fTPM-class driver and Linux's tpm_crb both speak. CRB rather than
 * the TIS FIFO because it is the simpler machine (a control area plus one in-page command/
 * response buffer) and what modern OSes expect from firmware-style TPMs.
 *
 * The layout mirrors QEMU's tpm-crb device -- the shape both target OS drivers are known to
 * drive: locality-0 control registers in the first 0x40 bytes, the control area at 0x40, and
 * the data buffer from 0x80 to the end of the 4 KiB page (advertised via CTRL_CMD/RSP_ADDR
 * pointing INTO this same MMIO page).
 *
 * PURE: reads/writes mutate this struct only; hype_tpm_crb_write() runs the command processor
 * synchronously when the guest rings CTRL_START (a real CRB completes asynchronously, but the
 * spec's contract is only that START reads 0 when done -- completing before the guest can read
 * back is legal and removes a whole polling state machine). Per-VM instances, no globals.
 */

#define HYPE_TPM_CRB_BASE 0xFED40000ull
#define HYPE_TPM_CRB_SIZE 0x1000u
#define HYPE_TPM_CRB_DATA_OFF 0x80u
#define HYPE_TPM_CRB_DATA_SIZE (HYPE_TPM_CRB_SIZE - HYPE_TPM_CRB_DATA_OFF)

/* register offsets (locality 0) */
#define HYPE_CRB_LOC_STATE 0x00u
#define HYPE_CRB_LOC_CTRL 0x08u
#define HYPE_CRB_LOC_STS 0x0Cu
#define HYPE_CRB_INTF_ID 0x30u      /* 64-bit */
#define HYPE_CRB_CTRL_EXT 0x38u
#define HYPE_CRB_CTRL_REQ 0x40u
#define HYPE_CRB_CTRL_STS 0x44u
#define HYPE_CRB_CTRL_CANCEL 0x48u
#define HYPE_CRB_CTRL_START 0x4Cu
#define HYPE_CRB_CTRL_INT_ENABLE 0x50u
#define HYPE_CRB_CTRL_INT_STS 0x54u
#define HYPE_CRB_CTRL_CMD_SIZE 0x58u
#define HYPE_CRB_CTRL_CMD_LADDR 0x5Cu
#define HYPE_CRB_CTRL_CMD_HADDR 0x60u
#define HYPE_CRB_CTRL_RSP_SIZE 0x64u
#define HYPE_CRB_CTRL_RSP_ADDR 0x68u /* 64-bit */

typedef struct hype_tpm_crb {
    hype_tpm2_t tpm;
    uint8_t data[HYPE_TPM_CRB_DATA_SIZE]; /* the in-page command/response buffer */
    uint32_t loc_state;
    uint32_t loc_sts;
    uint32_t ctrl_req;
    uint32_t ctrl_sts;
    uint32_t ctrl_start;
    uint64_t cmds; /* diagnostics: commands executed */
} hype_tpm_crb_t;

void hype_tpm_crb_reset(hype_tpm_crb_t *c, uint64_t (*entropy)(void *ctx), void *entropy_ctx);

/* MMIO accessors: offset within [0, HYPE_TPM_CRB_SIZE), size 1/2/4/8. Reads return the value;
 * out-of-range or torn accesses read as 0 / are dropped, like real reserved space. */
uint64_t hype_tpm_crb_read(hype_tpm_crb_t *c, uint32_t offset, unsigned int size);
void hype_tpm_crb_write(hype_tpm_crb_t *c, uint32_t offset, unsigned int size, uint64_t val);

#endif /* HYPE_DEVICES_TPM_CRB_H */
