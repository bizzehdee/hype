#ifndef HYPE_ARCH_MMIO_DECODE_H
#define HYPE_ARCH_MMIO_DECODE_H

#include <stdint.h>

/*
 * Minimal x86_64 instruction decoder for MMIO emulation (M4-3). Only
 * decodes the narrow MOV/MOVZX family a compiler actually generates
 * for a simple `volatile` byte/word/dword memory access (what EDK2's
 * own MmioRead8/MmioWrite8-style library calls compile down to) --
 * not a general disassembler. Given the raw bytes at the faulting
 * instruction's address (arch/x86_64/svm/svm_vcpu.c's own job to fetch
 * these -- originally intended to come from SVM's Decode Assist, the
 * VMCB's num_bytes_fetched/guest_instruction_bytes fields, but
 * confirmed empirically, via real QEMU/KVM runs, that nested SVM under
 * KVM does not reliably populate them even when the underlying CPU
 * advertises the feature via CPUID -- so the caller instead reads
 * guest memory directly at RIP, which this project's identity-mapped
 * guest/NPT setup makes a plain host pointer dereference) and knowing
 * separately (from the NPF exit's own EXITINFO1) whether the access
 * was a read or a write, this reports which register carries the
 * value and how wide the access is. The memory operand's addressing
 * mode (ModRM/SIB/displacement) IS decoded here, but only far enough
 * to compute the instruction's total length (instr_len) for RIP
 * advancement -- never to compute an address, since the fully-resolved
 * guest-physical fault address is already known directly from
 * EXITINFO2.
 *
 * Any instruction outside this narrow set is reported as unrecognized
 * (return -1) rather than guessed at -- emulating an MMIO access this
 * project doesn't recognize is not safe to approximate.
 */

typedef struct {
    int is_write;       /* 1 = register -> memory (store), 0 = memory -> register (load) */
    uint8_t size_bytes; /* 1, 2, or 4 */
    uint8_t reg;        /* x86-64 register encoding 0-15 (REX.R/B already folded in) */
    /* Only meaningful for reads: 1 if the result must zero-extend to
     * fill the entire 64-bit destination register (true for a plain
     * 32-bit MOV -- x86-64 always zero-extends a 32-bit register
     * write -- and for MOVZX; false for an 8/16-bit MOV, which only
     * updates the low bits and leaves the rest of the register
     * untouched). */
    int zero_extend;
    /* Total length of the decoded instruction in bytes (prefixes +
     * opcode(s) + ModRM + SIB/displacement if present) -- the caller
     * advances the guest's RIP by exactly this much to resume just
     * past the emulated access. */
    uint8_t instr_len;
} hype_mmio_decode_t;

/*
 * Decodes the instruction at the start of `bytes` (up to `num_bytes`
 * valid). Recognizes: MOV r/m8,r8 (0x88) and MOV r8,r/m8 (0x8A); MOV
 * r/m16/32,r16/32 (0x89) and MOV r16/32,r/m16/32 (0x8B), with an
 * optional 0x66 operand-size prefix selecting 16-bit over the default
 * 32-bit; MOVZX r32/64,r/m8 (0x0F 0xB6) and r32/64,r/m16 (0x0F 0xB7);
 * an optional REX prefix (0x40-0x4F) anywhere a real encoding allows
 * one; any ModRM/SIB/displacement addressing form (register-indirect,
 * disp8, disp32, SIB with or without an index/base, RIP-relative) --
 * all consumed only to compute instr_len, never interpreted as an
 * address. A ModRM byte encoding register-direct (mod=11, no memory
 * operand at all) is rejected -- inconsistent with this decoder only
 * ever being called for a memory-access fault. Returns 0 and fills
 * *out if recognized, non-zero otherwise. Pure decoding, no CPU/guest-
 * memory access of its own.
 */
int hype_mmio_decode(const uint8_t *bytes, uint8_t num_bytes, hype_mmio_decode_t *out);

/*
 * Given a decoded MMIO read's raw memory value (already masked to
 * size_bytes by the device model) and the destination register's
 * current 64-bit value, returns what that register should become:
 * zero_extend true replaces the whole register (matches a 32-bit MOV,
 * which x86-64 always zero-extends to 64 bits regardless of the
 * instruction's own mnemonic, and MOVZX); zero_extend false patches
 * only the low size_bytes bytes, leaving the rest of the register
 * untouched (an 8/16-bit MOV). Pure bit manipulation -- which real
 * machine register this applies to is the caller's job
 * (arch/x86_64/svm/svm_vcpu.c), since that mapping is backend-specific.
 */
uint64_t hype_mmio_merge_read_value(uint64_t old_reg_value, uint32_t mem_value, uint8_t size_bytes,
                                    int zero_extend);

/*
 * Given a decoded MMIO write's source register's current 64-bit value,
 * returns the size_bytes-wide value to hand to the device model
 * (low bytes only -- a store instruction never reads the register's
 * upper, unused bits). Pure bit manipulation.
 */
uint32_t hype_mmio_extract_write_value(uint64_t reg_value, uint8_t size_bytes);

#endif /* HYPE_ARCH_MMIO_DECODE_H */
