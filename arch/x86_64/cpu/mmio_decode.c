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

int hype_mmio_decode(const uint8_t *bytes, uint8_t num_bytes, hype_mmio_decode_t *out) {
    uint8_t i = 0;
    uint8_t rex = 0;
    int has_rex = 0;
    int operand16 = 0;
    uint8_t opcode;
    uint8_t reg_field;
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
    if (has_rex && (rex & 0x04u)) {
        reg_field = (uint8_t)(reg_field | 0x08u);
    }
    out->instr_len = (uint8_t)(modrm_index + 1 + tail_len);

    switch (opcode) {
        case 0x88u: /* MOV r/m8, r8 (store) */
            out->is_write = 1;
            out->size_bytes = 1;
            out->reg = reg_field;
            out->zero_extend = 0;
            return 0;
        case 0x8Au: /* MOV r8, r/m8 (load) */
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

uint32_t hype_mmio_extract_write_value(uint64_t reg_value, uint8_t size_bytes) {
    uint32_t mask = (size_bytes == 1u) ? 0xFFu : (size_bytes == 2u) ? 0xFFFFu : 0xFFFFFFFFu;
    return (uint32_t)(reg_value & mask);
}
