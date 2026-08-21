#include "tpm_crb.h"

/*
 * INTF_ID: InterfaceType=CRB(1), InterfaceVersion=CRB(1), CapCRB=1, CapLocality=0 (loc 0 only),
 * RID/VID/DID in the high half. Vendor 0x1014 is what several software TPMs report; the driver
 * keys off the interface type, not the vendor.
 */
#define CRB_INTF_ID_LO 0x4011u /* type=CRB(0b0001), version=CRB(0b0001), CapCRB */
#define CRB_INTF_ID_HI 0x00011014u

void hype_tpm_crb_reset(hype_tpm_crb_t *c, uint64_t (*entropy)(void *ctx), void *entropy_ctx) {
    unsigned i;
    hype_tpm2_reset(&c->tpm, entropy, entropy_ctx);
    for (i = 0; i < HYPE_TPM_CRB_DATA_SIZE; i++) c->data[i] = 0;
    /* locality 0 assigned + TPM established: the state a firmware-launched TPM presents */
    c->loc_state = 0x83u; /* tpmEstablished | locAssigned | tpmRegValidSts */
    c->loc_sts = 0x01u;   /* granted */
    c->ctrl_req = 0;
    c->ctrl_sts = 0;      /* tpmIdle clear -- ready */
    c->ctrl_start = 0;
    c->cmds = 0;
}

static uint32_t reg_read32(hype_tpm_crb_t *c, uint32_t off) {
    switch (off) {
    case HYPE_CRB_LOC_STATE: return c->loc_state;
    case HYPE_CRB_LOC_CTRL: return 0;
    case HYPE_CRB_LOC_STS: return c->loc_sts;
    case HYPE_CRB_INTF_ID: return CRB_INTF_ID_LO;
    case HYPE_CRB_INTF_ID + 4u: return CRB_INTF_ID_HI;
    case HYPE_CRB_CTRL_REQ: return c->ctrl_req;
    case HYPE_CRB_CTRL_STS: return c->ctrl_sts;
    case HYPE_CRB_CTRL_CANCEL: return 0;
    case HYPE_CRB_CTRL_START: return c->ctrl_start;
    case HYPE_CRB_CTRL_INT_ENABLE: return 0;
    case HYPE_CRB_CTRL_INT_STS: return 0;
    case HYPE_CRB_CTRL_CMD_SIZE: return HYPE_TPM_CRB_DATA_SIZE;
    case HYPE_CRB_CTRL_CMD_LADDR: return (uint32_t)(HYPE_TPM_CRB_BASE + HYPE_TPM_CRB_DATA_OFF);
    case HYPE_CRB_CTRL_CMD_HADDR: return (uint32_t)((HYPE_TPM_CRB_BASE + HYPE_TPM_CRB_DATA_OFF) >> 32);
    case HYPE_CRB_CTRL_RSP_SIZE: return HYPE_TPM_CRB_DATA_SIZE;
    case HYPE_CRB_CTRL_RSP_ADDR: return (uint32_t)(HYPE_TPM_CRB_BASE + HYPE_TPM_CRB_DATA_OFF);
    case HYPE_CRB_CTRL_RSP_ADDR + 4u:
        return (uint32_t)((HYPE_TPM_CRB_BASE + HYPE_TPM_CRB_DATA_OFF) >> 32);
    default: return 0;
    }
}

uint64_t hype_tpm_crb_read(hype_tpm_crb_t *c, uint32_t offset, unsigned int size) {
    if (offset >= HYPE_TPM_CRB_SIZE || size == 0u || size > 8u ||
        offset + size > HYPE_TPM_CRB_SIZE) {
        return 0;
    }
    if (offset >= HYPE_TPM_CRB_DATA_OFF) {
        uint64_t v = 0;
        unsigned i;
        for (i = 0; i < size; i++) {
            v |= (uint64_t)c->data[offset - HYPE_TPM_CRB_DATA_OFF + i] << (8u * i);
        }
        return v;
    }
    {
        /* register space: assemble from aligned 32-bit cells so 1/2/8-byte reads all work */
        uint64_t v = 0;
        unsigned i;
        for (i = 0; i < size; i++) {
            uint32_t byte_off = offset + i;
            uint32_t cell = reg_read32(c, byte_off & ~3u);
            v |= (uint64_t)((cell >> (8u * (byte_off & 3u))) & 0xFFu) << (8u * i);
        }
        return v;
    }
}

static void crb_execute(hype_tpm_crb_t *c) {
    /* the command's own header carries its length; bound it to the buffer */
    uint32_t len;
    static uint8_t rsp[HYPE_TPM2_MAX_RSP];
    unsigned int rlen, i;

    if (HYPE_TPM_CRB_DATA_SIZE < 10u) return;
    len = ((uint32_t)c->data[2] << 24) | ((uint32_t)c->data[3] << 16) |
          ((uint32_t)c->data[4] << 8) | c->data[5];
    if (len < 10u || len > HYPE_TPM_CRB_DATA_SIZE || len > HYPE_TPM2_MAX_CMD) {
        len = 10u; /* let the processor answer COMMAND_SIZE from the header alone */
    }
    rlen = hype_tpm2_execute(&c->tpm, c->data, len, rsp);
    if (rlen > HYPE_TPM_CRB_DATA_SIZE) rlen = HYPE_TPM_CRB_DATA_SIZE;
    for (i = 0; i < rlen; i++) c->data[i] = rsp[i];
    c->cmds++;
}

void hype_tpm_crb_write(hype_tpm_crb_t *c, uint32_t offset, unsigned int size, uint64_t val) {
    if (offset >= HYPE_TPM_CRB_SIZE || size == 0u || size > 8u ||
        offset + size > HYPE_TPM_CRB_SIZE) {
        return;
    }
    if (offset >= HYPE_TPM_CRB_DATA_OFF) {
        unsigned i;
        for (i = 0; i < size; i++) {
            c->data[offset - HYPE_TPM_CRB_DATA_OFF + i] = (uint8_t)(val >> (8u * i));
        }
        return;
    }
    switch (offset) {
    case HYPE_CRB_LOC_CTRL:
        /* #433/#590: the locality handshake. requestAccess(bit0) grants locality 0 and marks it
         * assigned; Relinquish(bit2) releases it -- the driver polls LOC_STATE afterwards, and
         * without honouring these it saw locAssigned stuck set and logged
         * "TPM_LOC_STATE_x.Relinquish timed out". */
        if (val & 0x1u) {        /* requestAccess */
            c->loc_state |= 0x2u; /* locAssigned */
            c->loc_sts = 0x1u;    /* granted */
        }
        if (val & 0x4u) {        /* Relinquish */
            c->loc_state &= ~0x2u;
        }
        break;
    case HYPE_CRB_CTRL_REQ:
        /* cmdReady(bit0) / goIdle(bit1): acknowledge instantly -- this TPM is always ready */
        c->ctrl_req = 0;
        break;
    case HYPE_CRB_CTRL_START:
        if (val & 1u) {
            crb_execute(c);
            c->ctrl_start = 0; /* completion: START reads back 0 */
        }
        break;
    case HYPE_CRB_CTRL_CANCEL:
    case HYPE_CRB_CTRL_INT_ENABLE:
    case HYPE_CRB_CTRL_INT_STS:
        break; /* accepted, nothing to do: no interrupts */
    default:
        break; /* read-only or reserved: dropped, like real hardware */
    }
}

int hype_tpm_crb_decode_movs(const uint8_t *bytes, unsigned int n, unsigned int *elem,
                             unsigned int *len, int *is_rep) {
    unsigned int i = 0;
    int rep = 0, opsize = 0, rexw = 0;

    if (bytes == 0 || n == 0u) {
        return 0;
    }
    /* Legacy + REX prefixes in any order. movs takes no ModRM, so once the opcode byte is
     * reached the instruction ends. */
    for (; i < n; i++) {
        uint8_t b = bytes[i];
        if (b == 0xF3u) { rep = 1; continue; }       /* REP */
        if (b == 0xF2u) { continue; }                /* REPNE (accepted, same iteration) */
        if (b == 0x66u) { opsize = 1; continue; }    /* operand-size override */
        if (b == 0x67u || b == 0x2Eu || b == 0x3Eu || b == 0x26u ||
            b == 0x64u || b == 0x65u || b == 0x36u) { continue; } /* addr-size / segment */
        if (b >= 0x40u && b <= 0x4Fu) { if (b & 0x08u) rexw = 1; continue; } /* REX, W bit */
        break;
    }
    if (i >= n) {
        return 0;
    }
    if (bytes[i] == 0xA4u) {          /* MOVSB */
        *elem = 1u;
    } else if (bytes[i] == 0xA5u) {   /* MOVSW/D/Q */
        *elem = rexw ? 8u : (opsize ? 2u : 4u);
    } else {
        return 0;
    }
    *len = i + 1u;
    *is_rep = rep;
    return 1;
}
