#include "mmio_decode.h"

/*
 * Given the index of the ModRM byte itself (bytes[modrm_index]),
 * returns the number of bytes ModRM plus any SIB/displacement bytes
 * that follow it occupy (i.e. NOT counting ModRM itself), or -1 if
 * `num_bytes` doesn't cover them. Handles every ModRM/SIB addressing
 * form the ISA defines; the actual address is never computed, only
 * skipped over. Also extracts ModRM.reg (*out_reg_field) and rejects
 * mod=11 (register-direct -- no memory operand at all, inconsistent
 * with this decoder only ever being called for a memory-access fault).
 */
static int decode_modrm_tail(const uint8_t *bytes, uint8_t num_bytes, uint8_t modrm_index,
                              uint8_t *out_reg_field, int *out_tail_len) {
    uint8_t modrm = bytes[modrm_index];
    uint8_t mod = (uint8_t)((modrm >> 6) & 0x03u);
    uint8_t rm = (uint8_t)(modrm & 0x07u);
    int tail = 0;

    *out_reg_field = (uint8_t)((modrm >> 3) & 0x07u);

    if (mod == 3u) {
        return -1; /* register-direct: no memory operand at all */
    }

    if (rm == 4u) {
        uint8_t sib_index = (uint8_t)(modrm_index + 1u);
        uint8_t sib;

        if (sib_index >= num_bytes) {
            return -1;
        }
        sib = bytes[sib_index];
        tail += 1; /* the SIB byte itself */
        if (mod == 0u && (sib & 0x07u) == 5u) {
            tail += 4; /* SIB with no base register: disp32 follows */
        }
    } else if (mod == 0u && rm == 5u) {
        tail += 4; /* RIP-relative: disp32 follows, no SIB */
    }

    if (mod == 1u) {
        tail += 1; /* disp8 */
    } else if (mod == 2u) {
        tail += 4; /* disp32 */
    }

    if ((uint32_t)modrm_index + 1u + (uint32_t)tail > num_bytes) {
        return -1;
    }

    *out_tail_len = tail;
    return 0;
}

/*
 * #307: 8-bit register operands. WITHOUT a REX prefix, ModRM.reg values 4-7 name
 * AH/CH/DH/BH -- the HIGH bytes of the first four registers -- not the low bytes of
 * RSP/RBP/RSI/RDI. Every caller resolves a register encoding to a whole 64-bit register and
 * takes its low byte, which cannot express that, so such a form would silently transfer the
 * wrong byte. Refused instead: a panic naming the instruction is recoverable, a device
 * register written from the wrong half of a register is not.
 *
 * `raw_reg_field` is the ModRM field BEFORE REX.R is folded in, since REX.R is what makes
 * the low-byte encodings available in the first place.
 */
static int byte_reg_unaddressable(int has_rex, uint8_t raw_reg_field) {
    return !has_rex && raw_reg_field >= 4u;
}

/*
 * #307: opcode-extension groups (0x80/0x81/0x83, and 0xC6/0xC7) map ModRM.reg to an
 * operation rather than to a register. REX.R is not part of that extension, so the raw
 * 3-bit field is what selects it.
 *
 * Returns 0 and sets *out_op for the ops whose destination this project is willing to
 * update, -1 for the rest:
 *   /2 ADC and /3 SBB consume carry-in, which is not modelled here -- writing a device
 *   register with the wrong CF is silent corruption;
 *   /7 CMP writes no destination at all, so it is a flags-only form that would take a
 *   different code path in every handler (the fault is a READ, not a write). Refused
 *   rather than half-supported, so no handler can reach a mem-destination CMP believing
 *   it has a value to store.
 */
static int group1_op(uint8_t raw_reg_field, hype_mmio_alu_op_t *out_op) {
    switch (raw_reg_field & 0x07u) {
        case 0u: *out_op = HYPE_MMIO_ALU_ADD; return 0;
        case 1u: *out_op = HYPE_MMIO_ALU_OR; return 0;
        case 4u: *out_op = HYPE_MMIO_ALU_AND; return 0;
        case 5u: *out_op = HYPE_MMIO_ALU_SUB; return 0;
        case 6u: *out_op = HYPE_MMIO_ALU_XOR; return 0;
        default: return -1;
    }
}

int hype_mmio_decode(const uint8_t *bytes, uint8_t num_bytes, hype_mmio_decode_t *out) {
    uint8_t i = 0;
    uint8_t rex = 0;
    int has_rex = 0;
    int operand16 = 0;
    uint8_t opcode;
    uint8_t reg_field;
    uint8_t raw_reg_field = 0;
    int tail_len;
    uint8_t modrm_index;

    if (num_bytes == 0) {
        return -1;
    }

    if (bytes[i] == 0x66u) {
        operand16 = 1;
        i++;
    }
    if (i >= num_bytes) {
        return -1;
    }

    if (bytes[i] >= 0x40u && bytes[i] <= 0x4Fu) {
        rex = bytes[i];
        has_rex = 1;
        i++;
    }
    if (i >= num_bytes) {
        return -1;
    }

    opcode = bytes[i];
    i++;

    if (opcode == 0x0Fu) {
        uint8_t opcode2;

        if (i >= num_bytes) {
            return -1;
        }
        opcode2 = bytes[i];
        i++;
        if (i >= num_bytes) {
            return -1;
        }
        modrm_index = i;
        if (decode_modrm_tail(bytes, num_bytes, modrm_index, &reg_field, &tail_len) != 0) {
            return -1;
        }
        if (has_rex && (rex & 0x04u)) {
            reg_field = (uint8_t)(reg_field | 0x08u);
        }
        out->instr_len = (uint8_t)(modrm_index + 1 + tail_len);
        out->op = HYPE_MMIO_ALU_MOV;
        out->has_imm = 0;
        out->imm_value = 0;
        out->mem_is_dst = 0;

        if (opcode2 == 0xB6u) { /* MOVZX r32/r64, r/m8 */
            out->is_write = 0;
            out->size_bytes = 1;
            out->reg = reg_field;
            out->zero_extend = 1;
            return 0;
        }
        if (opcode2 == 0xB7u) { /* MOVZX r32/r64, r/m16 */
            out->is_write = 0;
            out->size_bytes = 2;
            out->reg = reg_field;
            out->zero_extend = 1;
            return 0;
        }
        return -1;
    }

    if (i >= num_bytes) {
        return -1;
    }
    modrm_index = i;
    if (decode_modrm_tail(bytes, num_bytes, modrm_index, &reg_field, &tail_len) != 0) {
        return -1;
    }
    raw_reg_field = reg_field;
    if (has_rex && (rex & 0x04u)) {
        reg_field = (uint8_t)(reg_field | 0x08u);
    }
    out->instr_len = (uint8_t)(modrm_index + 1 + tail_len);
    out->op = HYPE_MMIO_ALU_MOV; /* #305: overridden below only by the ALU forms */
    out->has_imm = 0;            /* #306: set only by the imm store forms */
    out->imm_value = 0;
    out->mem_is_dst = 0;         /* #307: set only by the memory-destination ALU forms */

    switch (opcode) {
        case 0x88u: /* MOV r/m8, r8 (store) */
            if (byte_reg_unaddressable(has_rex, raw_reg_field)) {
                return -1;
            }
            out->is_write = 1;
            out->size_bytes = 1;
            out->reg = reg_field;
            out->zero_extend = 0;
            return 0;
        case 0x8Au: /* MOV r8, r/m8 (load) */
            if (byte_reg_unaddressable(has_rex, raw_reg_field)) {
                return -1;
            }
            out->is_write = 0;
            out->size_bytes = 1;
            out->reg = reg_field;
            out->zero_extend = 0;
            return 0;
        case 0x89u: /* MOV r/m16 or r/m32, r16/r32 (store) */
            out->is_write = 1;
            out->size_bytes = operand16 ? 2u : 4u;
            out->reg = reg_field;
            out->zero_extend = 0;
            return 0;
        case 0x8Bu: /* MOV r16/r32, r/m16 or r/m32 (load) */
            out->is_write = 0;
            out->size_bytes = operand16 ? 2u : 4u;
            out->reg = reg_field;
            out->zero_extend = operand16 ? 0 : 1;
            return 0;
        /*
         * #305: the memory-SOURCE ALU group, `<op> r16/r32, r/m16/r32`. Decoded as loads
         * because that is what the device sees -- one read -- with the operation recorded
         * so the caller can combine it with the destination register and set flags.
         *
         * zero_extend is 0 for all of them: unlike MOV, these are read-modify-register
         * operations whose 32-bit result still zero-extends the register, but the caller
         * computes the result from the OLD register value, so it must not have been
         * clobbered first. The caller writes the full 64-bit register itself.
         *
         * The memory-DESTINATION direction is the separate #307 group below.
         */
        case 0x03u: /* ADD r16/r32, r/m16/r32 */
        case 0x0Bu: /* OR  */
        case 0x23u: /* AND -- FreeBSD's LAPIC read */
        case 0x2Bu: /* SUB */
        case 0x33u: /* XOR */
        case 0x3Bu: /* CMP */
        case 0x85u: /* TEST r/m16/r32, r16/r32 -- commutative, so direction does not matter */
            out->is_write = 0;
            out->size_bytes = operand16 ? 2u : 4u;
            out->reg = reg_field;
            out->zero_extend = 0;
            out->op = (opcode == 0x03u)   ? HYPE_MMIO_ALU_ADD
                      : (opcode == 0x0Bu) ? HYPE_MMIO_ALU_OR
                      : (opcode == 0x23u) ? HYPE_MMIO_ALU_AND
                      : (opcode == 0x2Bu) ? HYPE_MMIO_ALU_SUB
                      : (opcode == 0x33u) ? HYPE_MMIO_ALU_XOR
                      : (opcode == 0x3Bu) ? HYPE_MMIO_ALU_CMP
                                          : HYPE_MMIO_ALU_TEST;
            return 0;
        /*
         * #306: MOV r/m, imm -- the stored value is in the instruction. FreeBSD's IO-APIC
         * register select is `mov dword [rbx], 1`.
         *
         * The ModRM reg field is an opcode EXTENSION here and must be /0; any other value
         * is a different instruction, so it is rejected rather than decoded as a MOV with
         * a stray register. The immediate follows the whole ModRM+SIB+displacement tail,
         * and its length has to land in instr_len -- getting that wrong resumes the guest
         * mid-instruction, which is far worse than the panic this replaces.
         */
        case 0xC6u:   /* MOV r/m8, imm8 */
        case 0xC7u: { /* MOV r/m16, imm16 or r/m32, imm32 */
            unsigned int imm_index = (unsigned int)(modrm_index + 1 + tail_len);
            unsigned int imm_len;

            if (reg_field != 0u) {
                return -1; /* opcode extension must be /0 */
            }
            out->size_bytes = (opcode == 0xC6u) ? 1u : (operand16 ? 2u : 4u);
            imm_len = (opcode == 0xC6u) ? 1u : (operand16 ? 2u : 4u);
            if (imm_index + imm_len > num_bytes) {
                return -1; /* the immediate is not in the bytes we were given */
            }
            out->is_write = 1;
            out->reg = 0;
            out->zero_extend = 0;
            out->has_imm = 1;
            out->imm_value = 0;
            {
                unsigned int k;
                for (k = 0; k < imm_len; k++) {
                    out->imm_value |= ((uint32_t)bytes[imm_index + k]) << (8u * k);
                }
            }
            out->instr_len = (uint8_t)(imm_index + imm_len);
            return 0;
        }
        /*
         * #307: `<op> r/m, r` -- the memory-DESTINATION ALU forms. FreeBSD enables AHCI
         * interrupts with `orl $0x2, (%rdx)` and hype panicked on it as undecodable; these
         * are the register-source counterparts of the same group.
         *
         * A read-modify-write is two device accesses, so the handler must read the register,
         * combine, and write back -- mem_is_dst says so. The fault is reported as a write
         * because that is what EXITINFO1/the exit qualification report for an RMW, and only
         * ops that actually write their destination are decoded here, so is_write is always
         * 1 and never disagrees.
         */
        case 0x00u: /* ADD r/m8, r8 */
        case 0x08u: /* OR  */
        case 0x20u: /* AND */
        case 0x28u: /* SUB */
        case 0x30u: /* XOR */
        case 0x01u: /* ADD r/m16/r32, r16/r32 */
        case 0x09u: /* OR  */
        case 0x21u: /* AND */
        case 0x29u: /* SUB */
        case 0x31u: /* XOR */
            if ((opcode & 0x01u) == 0u && byte_reg_unaddressable(has_rex, raw_reg_field)) {
                return -1;
            }
            out->is_write = 1;
            out->mem_is_dst = 1;
            out->size_bytes = (opcode & 0x01u) == 0u ? 1u : (operand16 ? 2u : 4u);
            out->reg = reg_field;
            out->zero_extend = 0;
            out->op = (opcode == 0x00u || opcode == 0x01u)   ? HYPE_MMIO_ALU_ADD
                      : (opcode == 0x08u || opcode == 0x09u) ? HYPE_MMIO_ALU_OR
                      : (opcode == 0x20u || opcode == 0x21u) ? HYPE_MMIO_ALU_AND
                      : (opcode == 0x28u || opcode == 0x29u) ? HYPE_MMIO_ALU_SUB
                                                             : HYPE_MMIO_ALU_XOR;
            return 0;
        /*
         * #307: group 1 -- `<op> r/m, imm`. This is the form FreeBSD's AHCI GHC.IE write
         * actually takes (`83 0a 02`). ModRM.reg is an opcode extension selecting the
         * operation; group1_op() refuses the ones this project will not emulate.
         *
         * 0x83's immediate is a single byte SIGN-EXTENDED to the operand width, which is why
         * `andl $-2, (%mem)` is three bytes rather than six. Zero-extending it instead would
         * clear the top 24 bits of a device register that the guest meant to preserve.
         */
        case 0x80u:   /* <op> r/m8, imm8 */
        case 0x81u:   /* <op> r/m16, imm16 or r/m32, imm32 */
        case 0x83u: { /* <op> r/m16/r/m32, imm8 (sign-extended) */
            unsigned int imm_index = (unsigned int)(modrm_index + 1 + tail_len);
            unsigned int imm_len;
            hype_mmio_alu_op_t op;

            if (group1_op(raw_reg_field, &op) != 0) {
                return -1;
            }
            out->size_bytes = (opcode == 0x80u) ? 1u : (operand16 ? 2u : 4u);
            imm_len = (opcode == 0x81u) ? (operand16 ? 2u : 4u) : 1u;
            if (imm_index + imm_len > num_bytes) {
                return -1;
            }
            out->is_write = 1;
            out->mem_is_dst = 1;
            out->reg = 0;
            out->zero_extend = 0;
            out->has_imm = 1;
            out->op = op;
            out->imm_value = 0;
            {
                unsigned int k;
                for (k = 0; k < imm_len; k++) {
                    out->imm_value |= ((uint32_t)bytes[imm_index + k]) << (8u * k);
                }
            }
            if (opcode == 0x83u && (out->imm_value & 0x80u) != 0u) {
                out->imm_value |= 0xFFFFFF00u; /* sign-extend imm8; masked to width on use */
            }
            out->instr_len = (uint8_t)(imm_index + imm_len);
            return 0;
        }
        default:
            return -1;
    }
}

uint64_t hype_mmio_merge_read_value(uint64_t old_reg_value, uint32_t mem_value, uint8_t size_bytes,
                                    int zero_extend) {
    uint64_t mask;

    if (zero_extend) {
        return (uint64_t)mem_value;
    }

    mask = (size_bytes == 1u) ? 0xFFULL : (size_bytes == 2u) ? 0xFFFFULL : 0xFFFFFFFFULL;
    return (old_reg_value & ~mask) | ((uint64_t)mem_value & mask);
}

uint32_t hype_mmio_store_value(const hype_mmio_decode_t *d, uint64_t reg_value) {
    if (d != (const hype_mmio_decode_t *)0 && d->has_imm) {
        return hype_mmio_extract_write_value((uint64_t)d->imm_value, d->size_bytes);
    }
    return hype_mmio_extract_write_value(reg_value, d ? d->size_bytes : 4u);
}

uint32_t hype_mmio_rmw_value(const hype_mmio_decode_t *d, uint64_t reg_value, uint32_t mem_value,
                             uint64_t *rflags) {
    uint32_t src;

    if (d == (const hype_mmio_decode_t *)0 || !d->mem_is_dst) {
        return mem_value; /* not an RMW: write back what was read rather than inventing a value */
    }
    src = d->has_imm ? hype_mmio_extract_write_value((uint64_t)d->imm_value, d->size_bytes)
                     : hype_mmio_extract_write_value(reg_value, d->size_bytes);
    /* Memory is the DESTINATION, so it is the first operand: `subl $2,(%rax)` is mem-2, not
     * 2-mem. Getting this backwards is invisible for AND/OR/XOR and wrong for ADD/SUB. */
    return hype_mmio_alu_apply(d->op, mem_value, src, d->size_bytes, rflags);
}

uint32_t hype_mmio_extract_write_value(uint64_t reg_value, uint8_t size_bytes) {
    uint32_t mask = (size_bytes == 1u) ? 0xFFu : (size_bytes == 2u) ? 0xFFFFu : 0xFFFFFFFFu;
    return (uint32_t)(reg_value & mask);
}

/* --- #305: the memory-source ALU group --- */

#define RFLAGS_CF (1ULL << 0)
#define RFLAGS_PF (1ULL << 2)
#define RFLAGS_AF (1ULL << 4)
#define RFLAGS_ZF (1ULL << 6)
#define RFLAGS_SF (1ULL << 7)
#define RFLAGS_OF (1ULL << 11)

int hype_mmio_alu_writes_reg(hype_mmio_alu_op_t op) {
    return (op != HYPE_MMIO_ALU_CMP && op != HYPE_MMIO_ALU_TEST);
}

static uint32_t width_mask(uint8_t size_bytes) {
    return (size_bytes == 1u) ? 0xFFu : (size_bytes == 2u) ? 0xFFFFu : 0xFFFFFFFFu;
}

/* PF is the EVEN parity of the low 8 bits of the result -- always the low byte, whatever
 * the operand width. */
static int even_parity_low_byte(uint32_t v) {
    unsigned int b = (unsigned int)(v & 0xFFu);
    unsigned int n = 0;
    while (b != 0u) {
        n += (b & 1u);
        b >>= 1;
    }
    return (n % 2u) == 0u;
}

static void set_result_flags(uint64_t *rflags, uint32_t result, uint8_t size_bytes) {
    uint32_t mask = width_mask(size_bytes);
    uint32_t sign_bit = (uint32_t)(mask ^ (mask >> 1)); /* top bit OF the operand width */

    *rflags &= ~(RFLAGS_ZF | RFLAGS_SF | RFLAGS_PF);
    if ((result & mask) == 0u) {
        *rflags |= RFLAGS_ZF;
    }
    if ((result & sign_bit) != 0u) {
        *rflags |= RFLAGS_SF;
    }
    if (even_parity_low_byte(result)) {
        *rflags |= RFLAGS_PF;
    }
}

uint32_t hype_mmio_alu_apply(hype_mmio_alu_op_t op, uint32_t dst_value, uint32_t mem_value,
                             uint8_t size_bytes, uint64_t *rflags) {
    uint32_t mask = width_mask(size_bytes);
    uint32_t a = dst_value & mask;
    uint32_t b = mem_value & mask;
    uint32_t sign_bit = (uint32_t)(mask ^ (mask >> 1));
    uint32_t result;
    uint64_t discard = 0;

    if (rflags == (uint64_t *)0) {
        rflags = &discard; /* a caller that does not care must not be a null dereference */
    }

    switch (op) {
        case HYPE_MMIO_ALU_AND:
        case HYPE_MMIO_ALU_TEST:
            result = a & b;
            break;
        case HYPE_MMIO_ALU_OR:
            result = a | b;
            break;
        case HYPE_MMIO_ALU_XOR:
            result = a ^ b;
            break;
        case HYPE_MMIO_ALU_ADD:
            result = (a + b) & mask;
            /* CF from the unmasked sum; OF when both operands share a sign that the
             * result does not. */
            *rflags &= ~(RFLAGS_CF | RFLAGS_OF | RFLAGS_AF);
            if (((uint64_t)a + (uint64_t)b) > (uint64_t)mask) {
                *rflags |= RFLAGS_CF;
            }
            if (((a ^ result) & (b ^ result) & sign_bit) != 0u) {
                *rflags |= RFLAGS_OF;
            }
            if ((((a & 0x0Fu) + (b & 0x0Fu)) & 0x10u) != 0u) {
                *rflags |= RFLAGS_AF;
            }
            set_result_flags(rflags, result, size_bytes);
            return result;
        case HYPE_MMIO_ALU_SUB:
        case HYPE_MMIO_ALU_CMP:
            result = (a - b) & mask;
            /* CF is a BORROW for subtraction, not a carry. */
            *rflags &= ~(RFLAGS_CF | RFLAGS_OF | RFLAGS_AF);
            if (a < b) {
                *rflags |= RFLAGS_CF;
            }
            if ((((a ^ b) & (a ^ result)) & sign_bit) != 0u) {
                *rflags |= RFLAGS_OF;
            }
            if ((a & 0x0Fu) < (b & 0x0Fu)) {
                *rflags |= RFLAGS_AF;
            }
            set_result_flags(rflags, result, size_bytes);
            return result;
        case HYPE_MMIO_ALU_MOV:
        default:
            return b; /* no flags: MOV, and the safe fallback for an unknown op */
    }

    /* The bitwise group: CF and OF are CLEARED (not left stale), AF is architecturally
     * undefined so it is left exactly as the guest had it. */
    *rflags &= ~(RFLAGS_CF | RFLAGS_OF);
    set_result_flags(rflags, result, size_bytes);
    return result;
}
