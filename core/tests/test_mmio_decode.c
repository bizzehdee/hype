#include <stdio.h>
#include "../../arch/x86_64/cpu/mmio_decode.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void expect_decode_ok(const char *desc, const uint8_t *bytes, uint8_t num_bytes, int exp_is_write,
                              uint8_t exp_size, uint8_t exp_reg, int exp_zero_extend) {
    hype_mmio_decode_t out;
    int rc = hype_mmio_decode(bytes, num_bytes, &out);

    if (rc != 0) {
        printf("FAIL: %s: expected decode success, got failure\n", desc);
        failures++;
        return;
    }
    CHECK_HEX("is_write", exp_is_write, out.is_write);
    CHECK_HEX("size_bytes", exp_size, out.size_bytes);
    CHECK_HEX("reg", exp_reg, out.reg);
    CHECK_HEX("zero_extend", exp_zero_extend, out.zero_extend);
}

static void expect_decode_ok_len(const char *desc, const uint8_t *bytes, uint8_t num_bytes, int exp_is_write,
                                  uint8_t exp_size, uint8_t exp_reg, int exp_zero_extend,
                                  uint8_t exp_instr_len) {
    hype_mmio_decode_t out;
    int rc = hype_mmio_decode(bytes, num_bytes, &out);

    if (rc != 0) {
        printf("FAIL: %s: expected decode success, got failure\n", desc);
        failures++;
        return;
    }
    CHECK_HEX("is_write", exp_is_write, out.is_write);
    CHECK_HEX("size_bytes", exp_size, out.size_bytes);
    CHECK_HEX("reg", exp_reg, out.reg);
    CHECK_HEX("zero_extend", exp_zero_extend, out.zero_extend);
    CHECK_HEX("instr_len", exp_instr_len, out.instr_len);
}

static void expect_decode_fail(const char *desc, const uint8_t *bytes, uint8_t num_bytes) {
    hype_mmio_decode_t out;
    int rc = hype_mmio_decode(bytes, num_bytes, &out);

    if (rc == 0) {
        printf("FAIL: %s: expected decode failure, got success\n", desc);
        failures++;
    }
}

/* MOV byte [rbx], al -- 88 03 (ModRM: mod=00,reg=000(AL),rm=011(RBX), no REX) */
static void test_mov_store_byte_no_rex(void) {
    uint8_t bytes[] = {0x88u, 0x03u};
    expect_decode_ok("mov [rbx], al", bytes, sizeof(bytes), 1, 1, 0, 0);
}

/* MOV byte [rax], r9b -- 44 88 08 (REX.R=1 extends reg to r9=1001b) */
static void test_mov_store_byte_rex_r(void) {
    uint8_t bytes[] = {0x44u, 0x88u, 0x08u};
    expect_decode_ok("mov [rax], r9b (REX.R)", bytes, sizeof(bytes), 1, 1, 9, 0);
}

/* MOV al, byte [rbx] -- 8A 03 */
static void test_mov_load_byte(void) {
    uint8_t bytes[] = {0x8Au, 0x03u};
    expect_decode_ok("mov al, [rbx]", bytes, sizeof(bytes), 0, 1, 0, 0);
}

/* MOV dword [rbx], ecx -- 89 0B (reg field 001 = rcx/ecx = 1) */
static void test_mov_store_dword(void) {
    uint8_t bytes[] = {0x89u, 0x0Bu};
    expect_decode_ok("mov [rbx], ecx", bytes, sizeof(bytes), 1, 4, 1, 0);
}

/* MOV ecx, dword [rbx] -- 8B 0B -- 32-bit load always zero-extends */
static void test_mov_load_dword_zero_extends(void) {
    uint8_t bytes[] = {0x8Bu, 0x0Bu};
    expect_decode_ok("mov ecx, [rbx]", bytes, sizeof(bytes), 0, 4, 1, 1);
}

/* 66 89 0B -- MOV word [rbx], cx -- 16-bit store, no zero extend concept */
static void test_mov_store_word_prefix(void) {
    uint8_t bytes[] = {0x66u, 0x89u, 0x0Bu};
    expect_decode_ok("mov [rbx], cx", bytes, sizeof(bytes), 1, 2, 1, 0);
}

/* 66 8B 0B -- MOV cx, word [rbx] -- 16-bit load does NOT auto zero-extend */
static void test_mov_load_word_prefix_no_zero_extend(void) {
    uint8_t bytes[] = {0x66u, 0x8Bu, 0x0Bu};
    expect_decode_ok("mov cx, [rbx]", bytes, sizeof(bytes), 0, 2, 1, 0);
}

/* 0F B6 0B -- MOVZX ecx, byte [rbx] */
static void test_movzx_byte(void) {
    uint8_t bytes[] = {0x0Fu, 0xB6u, 0x0Bu};
    expect_decode_ok("movzx ecx, byte [rbx]", bytes, sizeof(bytes), 0, 1, 1, 1);
}

/* 4C 0F B6 03 -- MOVZX r8, byte [rbx] (REX.W+R, reg field 000 with REX.R -> r8) */
static void test_movzx_byte_with_rex(void) {
    uint8_t bytes[] = {0x4Cu, 0x0Fu, 0xB6u, 0x03u};
    expect_decode_ok("movzx r8, byte [rbx] (REX)", bytes, sizeof(bytes), 0, 1, 8, 1);
}

/* 0F B7 0B -- MOVZX ecx, word [rbx] */
static void test_movzx_word(void) {
    uint8_t bytes[] = {0x0Fu, 0xB7u, 0x0Bu};
    expect_decode_ok("movzx ecx, word [rbx]", bytes, sizeof(bytes), 0, 2, 1, 1);
}

static void test_rejects_unrecognized_opcode(void) {
    /* XCHG r/m32, r32 -- a genuine memory-touching instruction this project does not
     * emulate. (This test used to use ADD r/m32,r32, which #307 now supports on purpose.) */
    uint8_t bytes[] = {0x87u, 0x03u};
    expect_decode_fail("unrecognized opcode", bytes, sizeof(bytes));
}

static void test_rejects_unrecognized_0f_opcode(void) {
    uint8_t bytes[] = {0x0Fu, 0xAFu, 0x03u}; /* IMUL, not MOVZX */
    expect_decode_fail("unrecognized 0F opcode", bytes, sizeof(bytes));
}

static void test_rejects_zero_length(void) {
    expect_decode_fail("zero-length input", (const uint8_t *)"", 0);
}

static void test_rejects_truncated_after_prefix(void) {
    uint8_t bytes[] = {0x66u};
    expect_decode_fail("truncated after 0x66 prefix", bytes, sizeof(bytes));
}

static void test_rejects_truncated_after_rex(void) {
    uint8_t bytes[] = {0x44u};
    expect_decode_fail("truncated after REX prefix", bytes, sizeof(bytes));
}

static void test_rejects_truncated_after_opcode(void) {
    uint8_t bytes[] = {0x88u};
    expect_decode_fail("truncated after opcode, no ModRM", bytes, sizeof(bytes));
}

static void test_rejects_truncated_after_0f(void) {
    uint8_t bytes[] = {0x0Fu};
    expect_decode_fail("truncated after 0x0F escape", bytes, sizeof(bytes));
}

static void test_rejects_truncated_0f_before_modrm(void) {
    uint8_t bytes[] = {0x0Fu, 0xB6u};
    expect_decode_fail("truncated 0F opcode, no ModRM", bytes, sizeof(bytes));
}

static void test_merge_read_value_zero_extend(void) {
    uint64_t result = hype_mmio_merge_read_value(0xFFFFFFFFFFFFFFFFULL, 0x000000AAu, 1, 1);
    CHECK_HEX("zero-extend clears upper bits", 0x00000000000000AAULL, result);
}

static void test_merge_read_value_no_zero_extend_byte(void) {
    uint64_t result = hype_mmio_merge_read_value(0x1122334455667788ULL, 0x000000AAu, 1, 0);
    CHECK_HEX("byte merge preserves upper bits", 0x11223344556677AAULL, result);
}

static void test_merge_read_value_no_zero_extend_word(void) {
    uint64_t result = hype_mmio_merge_read_value(0x1122334455667788ULL, 0x0000BEEFu, 2, 0);
    CHECK_HEX("word merge preserves upper bits", 0x112233445566BEEFULL, result);
}

static void test_merge_read_value_dword_zero_extend(void) {
    uint64_t result = hype_mmio_merge_read_value(0x1122334455667788ULL, 0xDEADBEEFu, 4, 1);
    CHECK_HEX("dword zero-extend clears upper 32 bits", 0x00000000DEADBEEFULL, result);
}

static void test_extract_write_value_byte(void) {
    uint32_t result = hype_mmio_extract_write_value(0x1122334455667788ULL, 1);
    CHECK_HEX("extract byte", 0x88u, result);
}

static void test_extract_write_value_word(void) {
    uint32_t result = hype_mmio_extract_write_value(0x1122334455667788ULL, 2);
    CHECK_HEX("extract word", 0x7788u, result);
}

static void test_extract_write_value_dword(void) {
    uint32_t result = hype_mmio_extract_write_value(0x1122334455667788ULL, 4);
    CHECK_HEX("extract dword", 0x55667788u, result);
}

/* mov [rbx+0x10], al -- disp8 addressing (mod=01) */
static void test_disp8_addressing(void) {
    uint8_t bytes[] = {0x88u, 0x43u, 0x10u};
    expect_decode_ok_len("mov [rbx+0x10], al", bytes, sizeof(bytes), 1, 1, 0, 0, 3);
}

/* mov [rbx+0x100], al -- disp32 addressing (mod=10) */
static void test_disp32_addressing(void) {
    uint8_t bytes[] = {0x88u, 0x83u, 0x00u, 0x01u, 0x00u, 0x00u};
    expect_decode_ok_len("mov [rbx+0x100], al", bytes, sizeof(bytes), 1, 1, 0, 0, 6);
}

/* mov [rax+rbx], cl -- SIB, mod=00, base present (no disp) */
static void test_sib_with_base_no_disp(void) {
    uint8_t bytes[] = {0x88u, 0x0Cu, 0x18u};
    expect_decode_ok_len("mov [rax+rbx], cl", bytes, sizeof(bytes), 1, 1, 1, 0, 3);
}

/* mov eax, [rcx*4+0x12345678] -- SIB, mod=00, base=101 (none) -> disp32 */
static void test_sib_no_base_disp32(void) {
    uint8_t bytes[] = {0x8Bu, 0x04u, 0x8Du, 0x78u, 0x56u, 0x34u, 0x12u};
    expect_decode_ok_len("mov eax, [rcx*4+0x12345678]", bytes, sizeof(bytes), 0, 4, 0, 1, 7);
}

/* mov [rip+0x1000], al -- mod=00, rm=101 -> RIP-relative disp32 */
static void test_rip_relative_addressing(void) {
    uint8_t bytes[] = {0x88u, 0x05u, 0x00u, 0x10u, 0x00u, 0x00u};
    expect_decode_ok_len("mov [rip+0x1000], al", bytes, sizeof(bytes), 1, 1, 0, 0, 6);
}

/* mov al, cl -- mod=11, register-direct: no memory operand at all */
static void test_rejects_register_direct(void) {
    uint8_t bytes[] = {0x8Au, 0xC1u};
    expect_decode_fail("register-direct ModRM (mod=11)", bytes, sizeof(bytes));
}

static void test_rejects_truncated_sib(void) {
    uint8_t bytes[] = {0x88u, 0x0Cu}; /* SIB byte itself missing */
    expect_decode_fail("truncated before SIB byte", bytes, sizeof(bytes));
}

static void test_rejects_truncated_disp8(void) {
    uint8_t bytes[] = {0x88u, 0x43u}; /* disp8 byte missing */
    expect_decode_fail("truncated before disp8", bytes, sizeof(bytes));
}

static void test_rejects_truncated_disp32(void) {
    uint8_t bytes[] = {0x88u, 0x83u, 0x00u, 0x01u}; /* only 2 of 4 disp32 bytes */
    expect_decode_fail("truncated before full disp32", bytes, sizeof(bytes));
}

/* --- #305: the memory-source ALU group --- */

static void test_decodes_and_r32_m32(void) {
    /* The exact instruction FreeBSD reads the LAPIC Spurious Interrupt Vector with, taken
     * from the panic hype used to produce: `and edx, dword [rcx+0xf0]`. */
    static const uint8_t insn[] = {0x23, 0x91, 0xF0, 0x00, 0x00, 0x00};
    hype_mmio_decode_t d;

    CHECK_HEX("decodes", 0, hype_mmio_decode(insn, (unsigned)sizeof(insn), &d));
    CHECK_HEX("it is a read", 0, d.is_write);
    CHECK_HEX("dword", 4, (int)d.size_bytes);
    CHECK_HEX("destination is EDX (reg 2)", 2, (int)d.reg);
    CHECK_HEX("op is AND", (int)HYPE_MMIO_ALU_AND, (int)d.op);
    CHECK_HEX("length prefix+opcode+modrm+disp32", 6, (int)d.instr_len);
    /* zero_extend must be 0: the caller needs the OLD register value to compute the
     * result, so it must not be told to discard it. */
    CHECK_HEX("zero_extend clear", 0, d.zero_extend);
    CHECK_HEX("AND writes its register", 1, hype_mmio_alu_writes_reg(d.op));
}

static void test_decodes_the_rest_of_the_group(void) {
    struct { uint8_t opcode; int op; const char *name; } cases[] = {
        {0x03u, (int)HYPE_MMIO_ALU_ADD, "ADD"},
        {0x0Bu, (int)HYPE_MMIO_ALU_OR, "OR"},
        {0x23u, (int)HYPE_MMIO_ALU_AND, "AND"},
        {0x2Bu, (int)HYPE_MMIO_ALU_SUB, "SUB"},
        {0x33u, (int)HYPE_MMIO_ALU_XOR, "XOR"},
        {0x3Bu, (int)HYPE_MMIO_ALU_CMP, "CMP"},
        {0x85u, (int)HYPE_MMIO_ALU_TEST, "TEST"},
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t insn[3];
        hype_mmio_decode_t d;
        insn[0] = cases[i].opcode;
        insn[1] = 0x01u; /* mod=00 reg=000(eax) rm=001(ecx): [rcx] */
        insn[2] = 0x00u;
        CHECK_HEX(cases[i].name, 0, hype_mmio_decode(insn, 3u, &d));
        CHECK_HEX("op", cases[i].op, (int)d.op);
        CHECK_HEX("read", 0, d.is_write);
        CHECK_HEX("dword", 4, (int)d.size_bytes);
    }
    /* Only CMP and TEST leave the register alone. */
    CHECK_HEX("CMP writes no register", 0, hype_mmio_alu_writes_reg(HYPE_MMIO_ALU_CMP));
    CHECK_HEX("TEST writes no register", 0, hype_mmio_alu_writes_reg(HYPE_MMIO_ALU_TEST));
}

static void test_mov_is_still_a_mov(void) {
    /* The MOV path must be untouched -- every Linux guest depends on it. */
    static const uint8_t insn[] = {0x8B, 0x01};
    hype_mmio_decode_t d;

    CHECK_HEX("decodes", 0, hype_mmio_decode(insn, 2u, &d));
    CHECK_HEX("op is MOV", (int)HYPE_MMIO_ALU_MOV, (int)d.op);
    CHECK_HEX("and it still zero-extends", 1, d.zero_extend);
}

static void test_alu_bitwise_flags(void) {
    /*
     * The bitwise group CLEARS CF and OF rather than leaving them stale, and leaves AF
     * alone because AF is architecturally undefined for them. Getting "clears" wrong is
     * invisible until a guest branches on the flag.
     */
    uint64_t rflags = 0x8D5u; /* CF, PF, AF, ZF, SF and OF-adjacent bits all set */
    uint32_t r;

    r = hype_mmio_alu_apply(HYPE_MMIO_ALU_AND, 0xF0F0F0F0u, 0x0F0F0F0Fu, 4u, &rflags);
    CHECK_HEX("AND result", 0u, r);
    CHECK_HEX("ZF set for a zero result", (1u << 6), (unsigned)(rflags & (1u << 6)));
    CHECK_HEX("CF cleared", 0u, (unsigned)(rflags & 1u));
    CHECK_HEX("OF cleared", 0u, (unsigned)(rflags & (1u << 11)));
    CHECK_HEX("PF set (zero has even parity)", (1u << 2), (unsigned)(rflags & (1u << 2)));
    CHECK_HEX("AF untouched", (1u << 4), (unsigned)(rflags & (1u << 4)));

    rflags = 0;
    r = hype_mmio_alu_apply(HYPE_MMIO_ALU_OR, 0x00000000u, 0x80000001u, 4u, &rflags);
    CHECK_HEX("OR result", 0x80000001u, r);
    CHECK_HEX("SF set for a negative result", (1u << 7), (unsigned)(rflags & (1u << 7)));
    CHECK_HEX("ZF clear", 0u, (unsigned)(rflags & (1u << 6)));

    rflags = 0;
    r = hype_mmio_alu_apply(HYPE_MMIO_ALU_XOR, 0xAAAAAAAAu, 0xAAAAAAAAu, 4u, &rflags);
    CHECK_HEX("XOR of equals is zero", 0u, r);
    CHECK_HEX("ZF set", (1u << 6), (unsigned)(rflags & (1u << 6)));
}

static void test_alu_test_and_cmp_do_not_produce_a_register_value_by_accident(void) {
    /* They still RETURN a result (the caller simply must not store it), and the flags are
     * what matter. */
    uint64_t rflags = 0;

    (void)hype_mmio_alu_apply(HYPE_MMIO_ALU_TEST, 0x1u, 0x2u, 4u, &rflags);
    CHECK_HEX("TEST of disjoint bits sets ZF", (1u << 6), (unsigned)(rflags & (1u << 6)));
    rflags = 0;
    (void)hype_mmio_alu_apply(HYPE_MMIO_ALU_CMP, 5u, 5u, 4u, &rflags);
    CHECK_HEX("CMP of equals sets ZF", (1u << 6), (unsigned)(rflags & (1u << 6)));
    rflags = 0;
    (void)hype_mmio_alu_apply(HYPE_MMIO_ALU_CMP, 1u, 2u, 4u, &rflags);
    CHECK_HEX("CMP borrow sets CF", 1u, (unsigned)(rflags & 1u));
}

static void test_alu_arithmetic_flags(void) {
    uint64_t rflags = 0;
    uint32_t r;

    r = hype_mmio_alu_apply(HYPE_MMIO_ALU_ADD, 0xFFFFFFFFu, 1u, 4u, &rflags);
    CHECK_HEX("ADD wraps", 0u, r);
    CHECK_HEX("carry out sets CF", 1u, (unsigned)(rflags & 1u));
    CHECK_HEX("ZF set", (1u << 6), (unsigned)(rflags & (1u << 6)));

    rflags = 0;
    r = hype_mmio_alu_apply(HYPE_MMIO_ALU_ADD, 0x7FFFFFFFu, 1u, 4u, &rflags);
    CHECK_HEX("signed overflow result", 0x80000000u, r);
    CHECK_HEX("OF set for positive+positive=negative", (1u << 11),
              (unsigned)(rflags & (1u << 11)));
    CHECK_HEX("CF clear (no unsigned carry)", 0u, (unsigned)(rflags & 1u));

    rflags = 0;
    r = hype_mmio_alu_apply(HYPE_MMIO_ALU_SUB, 3u, 5u, 4u, &rflags);
    CHECK_HEX("SUB borrows", 0xFFFFFFFEu, r);
    CHECK_HEX("CF set for a borrow", 1u, (unsigned)(rflags & 1u));
    CHECK_HEX("SF set", (1u << 7), (unsigned)(rflags & (1u << 7)));
}

static void test_alu_respects_operand_width(void) {
    /* A 16-bit form must not let the upper half affect the flags. */
    uint64_t rflags = 0;
    uint32_t r = hype_mmio_alu_apply(HYPE_MMIO_ALU_AND, 0xFFFF0000u, 0xFFFF0000u, 2u, &rflags);

    CHECK_HEX("16-bit AND result is masked", 0u, r);
    CHECK_HEX("ZF set from the low 16 bits only", (1u << 6), (unsigned)(rflags & (1u << 6)));

    rflags = 0;
    r = hype_mmio_alu_apply(HYPE_MMIO_ALU_AND, 0x0000FF80u, 0x0000FF80u, 2u, &rflags);
    CHECK_HEX("16-bit result", 0xFF80u, r);
    CHECK_HEX("SF from bit 15, not bit 31", (1u << 7), (unsigned)(rflags & (1u << 7)));
}

static void test_alu_null_rflags_is_safe(void) {
    /* A caller that does not care about flags must not be a null dereference. */
    CHECK_HEX("still computes", 0x0Fu,
              hype_mmio_alu_apply(HYPE_MMIO_ALU_AND, 0xFFu, 0x0Fu, 4u, 0));
    CHECK_HEX("MOV returns the memory value", 0x1234u,
              hype_mmio_alu_apply(HYPE_MMIO_ALU_MOV, 0xFFFFu, 0x1234u, 4u, 0));
}

/* --- #306: MOV r/m, imm (the store-an-immediate forms) --- */

static void test_decodes_mov_m32_imm32(void) {
    /* FreeBSD's IO-APIC register select, from the panic hype used to produce:
     * `mov dword [rbx], 1`. */
    static const uint8_t insn[] = {0xC7, 0x03, 0x01, 0x00, 0x00, 0x00};
    hype_mmio_decode_t d;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, (unsigned)sizeof(insn), &d));
    CHECK_HEX("it is a write", 1u, (unsigned)d.is_write);
    CHECK_HEX("dword", 4u, d.size_bytes);
    CHECK_HEX("carries an immediate", 1u, (unsigned)d.has_imm);
    CHECK_HEX("immediate value", 1u, d.imm_value);
    CHECK_HEX("length opcode+modrm+imm32", 6u, d.instr_len);
    CHECK_HEX("op is MOV", (unsigned)HYPE_MMIO_ALU_MOV, (unsigned)d.op);
    /* And the resolver hands back the immediate, not a register's contents. */
    CHECK_HEX("store value is the immediate", 1u, hype_mmio_store_value(&d, 0xDEADBEEFu));
}

static void test_decodes_mov_m8_imm8(void) {
    static const uint8_t insn[] = {0xC6, 0x00, 0xAB};
    hype_mmio_decode_t d;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 3u, &d));
    CHECK_HEX("byte", 1u, d.size_bytes);
    CHECK_HEX("immediate", 0xABu, d.imm_value);
    CHECK_HEX("length opcode+modrm+imm8", 3u, d.instr_len);
}

static void test_imm_length_is_added_for_each_addressing_form(void) {
    /* instr_len wrong by even one byte resumes the guest MID-INSTRUCTION, which is a far
     * nastier failure than the panic this replaces -- so every tail shape is pinned. */
    struct { const char *name; uint8_t b[12]; unsigned len; unsigned expect; } cases[] = {
        /* [rax], imm32 */
        {"no disp", {0xC7, 0x00, 0x44, 0x33, 0x22, 0x11}, 6u, 6u},
        /* [rax+0x10], imm32 : mod=01 disp8 */
        {"disp8", {0xC7, 0x40, 0x10, 0x44, 0x33, 0x22, 0x11}, 7u, 7u},
        /* [rax+0x11223344], imm32 : mod=10 disp32 */
        {"disp32", {0xC7, 0x80, 0x44, 0x33, 0x22, 0x11, 0x99, 0x88, 0x77, 0x66}, 10u, 10u},
        /* [rax+rcx*1], imm32 : mod=00 rm=100 SIB */
        {"SIB", {0xC7, 0x04, 0x08, 0x44, 0x33, 0x22, 0x11}, 7u, 7u},
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hype_mmio_decode_t d;
        CHECK_HEX(cases[i].name, 0u,
                  (unsigned)hype_mmio_decode(cases[i].b, cases[i].len, &d));
        CHECK_HEX("instr_len", cases[i].expect, d.instr_len);
        CHECK_HEX("has_imm", 1u, (unsigned)d.has_imm);
    }
}

static void test_imm_store_rejects_a_non_zero_opcode_extension(void) {
    /* For 0xC6/0xC7 the ModRM reg field is an opcode EXTENSION and must be /0. Anything
     * else is a different instruction, so decoding it as a MOV would emulate something the
     * guest never asked for. */
    static const uint8_t insn[] = {0xC7, 0x08, 0x01, 0x00, 0x00, 0x00}; /* reg field = 1 */
    hype_mmio_decode_t d;

    CHECK_HEX("rejected", (unsigned)-1,
              (unsigned)hype_mmio_decode(insn, (unsigned)sizeof(insn), &d));
}

static void test_imm_store_rejects_a_truncated_immediate(void) {
    /* Only 3 of the 4 immediate bytes present: decoding it would read past what the
     * caller fetched and invent a value to write to a device register. */
    static const uint8_t insn[] = {0xC7, 0x00, 0x01, 0x00, 0x00};
    hype_mmio_decode_t d;

    CHECK_HEX("rejected", (unsigned)-1, (unsigned)hype_mmio_decode(insn, 5u, &d));
}

static void test_store_value_falls_back_to_the_register(void) {
    /* The register-source path must be untouched -- every Linux guest depends on it. */
    static const uint8_t insn[] = {0x89, 0x01}; /* mov [rcx], eax */
    hype_mmio_decode_t d;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 2u, &d));
    CHECK_HEX("no immediate", 0u, (unsigned)d.has_imm);
    CHECK_HEX("store value comes from the register", 0xBEEFu,
              hype_mmio_store_value(&d, 0x1234BEEFu) & 0xFFFFu);
}

/* --- #307: memory-destination read-modify-write --- */

static void test_decodes_the_freebsd_ahci_ghc_write(void) {
    /*
     * The exact instruction FreeBSD 15.0 panicked hype on, byte for byte, from the run log:
     * `83 0a 02` = orl $0x2,(%rdx) -- AHCI GHC.IE. Group 1, extension /1 (OR), imm8.
     */
    static const uint8_t insn[] = {0x83, 0x0A, 0x02};
    hype_mmio_decode_t d;
    uint64_t rflags = 0;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 3u, &d));
    CHECK_HEX("memory is the destination", 1u, (unsigned)d.mem_is_dst);
    CHECK_HEX("reported as a write", 1u, (unsigned)d.is_write);
    CHECK_HEX("dword", 4u, d.size_bytes);
    CHECK_HEX("op is OR", (unsigned)HYPE_MMIO_ALU_OR, (unsigned)d.op);
    CHECK_HEX("carries an immediate", 1u, (unsigned)d.has_imm);
    CHECK_HEX("immediate", 2u, d.imm_value);
    CHECK_HEX("length opcode+modrm+imm8", 3u, d.instr_len);
    /* GHC already had AE (bit 31) set, so the write-back must PRESERVE it and add IE. */
    CHECK_HEX("result ORs into the register's current value", 0x80000002u,
              hype_mmio_rmw_value(&d, 0u, 0x80000000u, &rflags));
}

static void test_group1_ops_and_widths(void) {
    struct { uint8_t opcode; uint8_t modrm; int op; uint8_t size; const char *name; } cases[] = {
        {0x83u, 0x02u, (int)HYPE_MMIO_ALU_ADD, 4u, "addl imm8"},
        {0x83u, 0x0Au, (int)HYPE_MMIO_ALU_OR, 4u, "orl imm8"},
        {0x83u, 0x22u, (int)HYPE_MMIO_ALU_AND, 4u, "andl imm8"},
        {0x83u, 0x2Au, (int)HYPE_MMIO_ALU_SUB, 4u, "subl imm8"},
        {0x83u, 0x32u, (int)HYPE_MMIO_ALU_XOR, 4u, "xorl imm8"},
        {0x80u, 0x0Au, (int)HYPE_MMIO_ALU_OR, 1u, "orb imm8"},
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t insn[3];
        hype_mmio_decode_t d;
        insn[0] = cases[i].opcode;
        insn[1] = cases[i].modrm;
        insn[2] = 0x05u;
        CHECK_HEX(cases[i].name, 0u, (unsigned)hype_mmio_decode(insn, 3u, &d));
        CHECK_HEX("op", (unsigned)cases[i].op, (unsigned)d.op);
        CHECK_HEX("size", cases[i].size, d.size_bytes);
        CHECK_HEX("mem_is_dst", 1u, (unsigned)d.mem_is_dst);
        CHECK_HEX("instr_len", 3u, d.instr_len);
    }
}

static void test_group1_imm32_and_word_widths(void) {
    /* 0x81 carries a full-width immediate, and the 0x66 prefix shortens BOTH the operand
     * and the immediate to 16 bits -- a length mistake here resumes mid-instruction. */
    static const uint8_t dword[] = {0x81, 0x0A, 0x78, 0x56, 0x34, 0x12};
    static const uint8_t word[] = {0x66, 0x81, 0x0A, 0x34, 0x12};
    hype_mmio_decode_t d;

    CHECK_HEX("imm32 decodes", 0u, (unsigned)hype_mmio_decode(dword, 6u, &d));
    CHECK_HEX("imm32 value", 0x12345678u, d.imm_value);
    CHECK_HEX("imm32 length", 6u, d.instr_len);
    CHECK_HEX("imm32 width", 4u, d.size_bytes);

    CHECK_HEX("imm16 decodes", 0u, (unsigned)hype_mmio_decode(word, 5u, &d));
    CHECK_HEX("imm16 value", 0x1234u, d.imm_value);
    CHECK_HEX("imm16 length prefix+opcode+modrm+imm16", 5u, d.instr_len);
    CHECK_HEX("imm16 width", 2u, d.size_bytes);
}

static void test_group1_imm8_is_sign_extended(void) {
    /*
     * `andl $-2,(%rdx)` is `83 22 fe`: a single 0xFE byte meaning 0xFFFFFFFE. Zero-extending
     * it instead would compute mem & 0xFE and silently clear the top 24 bits of a device
     * register the guest meant to leave alone -- exactly how a driver's carefully preserved
     * configuration gets wiped.
     */
    static const uint8_t insn[] = {0x83, 0x22, 0xFE};
    hype_mmio_decode_t d;
    uint64_t rflags = 0;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 3u, &d));
    CHECK_HEX("immediate sign-extended", 0xFFFFFFFEu, d.imm_value);
    CHECK_HEX("clears only the low bit", 0xDEADBEEEu,
              hype_mmio_rmw_value(&d, 0u, 0xDEADBEEFu, &rflags));
}

static void test_reg_to_mem_ops(void) {
    struct { uint8_t opcode; int op; uint8_t size; const char *name; } cases[] = {
        {0x01u, (int)HYPE_MMIO_ALU_ADD, 4u, "add r/m32, r32"},
        {0x09u, (int)HYPE_MMIO_ALU_OR, 4u, "or  r/m32, r32"},
        {0x21u, (int)HYPE_MMIO_ALU_AND, 4u, "and r/m32, r32"},
        {0x29u, (int)HYPE_MMIO_ALU_SUB, 4u, "sub r/m32, r32"},
        {0x31u, (int)HYPE_MMIO_ALU_XOR, 4u, "xor r/m32, r32"},
        {0x00u, (int)HYPE_MMIO_ALU_ADD, 1u, "add r/m8, r8"},
        {0x08u, (int)HYPE_MMIO_ALU_OR, 1u, "or  r/m8, r8"},
        {0x20u, (int)HYPE_MMIO_ALU_AND, 1u, "and r/m8, r8"},
        {0x28u, (int)HYPE_MMIO_ALU_SUB, 1u, "sub r/m8, r8"},
        {0x30u, (int)HYPE_MMIO_ALU_XOR, 1u, "xor r/m8, r8"},
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t insn[2];
        hype_mmio_decode_t d;
        insn[0] = cases[i].opcode;
        insn[1] = 0x11u; /* mod=00 reg=010(edx/dl) rm=001([rcx]) */
        CHECK_HEX(cases[i].name, 0u, (unsigned)hype_mmio_decode(insn, 2u, &d));
        CHECK_HEX("op", (unsigned)cases[i].op, (unsigned)d.op);
        CHECK_HEX("size", cases[i].size, d.size_bytes);
        CHECK_HEX("mem_is_dst", 1u, (unsigned)d.mem_is_dst);
        CHECK_HEX("source register is EDX", 2u, d.reg);
        CHECK_HEX("no immediate", 0u, (unsigned)d.has_imm);
    }
}

static void test_rmw_source_is_the_register_when_there_is_no_immediate(void) {
    static const uint8_t insn[] = {0x09, 0x11}; /* or (%rcx), %edx */
    hype_mmio_decode_t d;
    uint64_t rflags = 0;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 2u, &d));
    CHECK_HEX("register supplies the operand", 0x0000000Fu,
              hype_mmio_rmw_value(&d, 0xFFFFFFFF00000005u, 0x0000000Au, &rflags));
}

static void test_rmw_direction_matters_for_subtraction(void) {
    /*
     * The whole reason direction is a separate field: `subl $2,(%rax)` is mem-2, and the
     * memory-SOURCE form `sub (%rax),%ecx` is reg-mem. Computing one as the other is
     * invisible for AND/OR/XOR and wrong for ADD/SUB, so it is pinned explicitly.
     */
    static const uint8_t mem_dst[] = {0x83, 0x28, 0x02}; /* subl $2,(%rax) */
    hype_mmio_decode_t d;
    uint64_t rflags = 0;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(mem_dst, 3u, &d));
    CHECK_HEX("op is SUB", (unsigned)HYPE_MMIO_ALU_SUB, (unsigned)d.op);
    CHECK_HEX("computes mem - imm, not imm - mem", 8u, hype_mmio_rmw_value(&d, 0u, 10u, &rflags));
    CHECK_HEX("no borrow", 0u, (unsigned)(rflags & 0x1u));

    /* And a borrow the other way sets CF, so the guest's next JB is right. */
    CHECK_HEX("wraps", 0xFFFFFFFFu, hype_mmio_rmw_value(&d, 0u, 1u, &rflags));
    CHECK_HEX("borrow set", 1u, (unsigned)(rflags & 0x1u));
}

static void test_rmw_sets_flags_from_the_result(void) {
    static const uint8_t insn[] = {0x83, 0x22, 0x00}; /* andl $0,(%rdx) */
    hype_mmio_decode_t d;
    uint64_t rflags = 0x8D5u; /* CF and OF set going in */

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 3u, &d));
    CHECK_HEX("result", 0u, hype_mmio_rmw_value(&d, 0u, 0xFFFFFFFFu, &rflags));
    CHECK_HEX("ZF set", 0x40u, (unsigned)(rflags & 0x40u));
    CHECK_HEX("CF cleared by the bitwise op", 0u, (unsigned)(rflags & 0x1u));
    CHECK_HEX("OF cleared by the bitwise op", 0u, (unsigned)(rflags & 0x800u));
}

static void test_rmw_refuses_adc_sbb_and_cmp(void) {
    /*
     * /2 ADC and /3 SBB need carry-in this decoder does not model; /7 CMP writes no
     * destination and would take a different path in every handler. All three must panic
     * visibly with the bytes in the message rather than be approximated -- a device register
     * updated with the wrong carry is silent corruption.
     */
    struct { uint8_t modrm; const char *name; } cases[] = {
        {0x12u, "ADC (/2)"}, {0x1Au, "SBB (/3)"},
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t insn[3];
        hype_mmio_decode_t d;
        insn[0] = 0x83u;
        insn[1] = cases[i].modrm;
        insn[2] = 0x01u;
        CHECK_HEX(cases[i].name, (unsigned)-1, (unsigned)hype_mmio_decode(insn, 3u, &d));
    }
    /* #457: /7 CMP is DECODED now -- as the flags-only READ it is, not as an RMW. OVMF's
     * flash FVB driver validates the variable-store header with `cmpb $imm, off(%reg)`. */
    {
        uint8_t insn[3];
        hype_mmio_decode_t d;
        insn[0] = 0x83u;
        insn[1] = 0x3Au; /* /7 = CMP, mem operand [rdx] */
        insn[2] = 0x01u;
        CHECK_HEX("imm CMP (/7) decodes", 0u, (unsigned)hype_mmio_decode(insn, 3u, &d));
        CHECK_HEX("imm CMP is a READ", 0u, (unsigned)d.is_write);
        CHECK_HEX("imm CMP has no mem destination", 0u, (unsigned)d.mem_is_dst);
        CHECK_HEX("imm CMP carries the immediate", 1u, (unsigned)d.has_imm);
        CHECK_HEX("imm CMP op", (unsigned)HYPE_MMIO_ALU_CMP, (unsigned)d.op);
        CHECK_HEX("imm8 sign-extends via 0x83", 0x1u, d.imm_value);
    }
    /* 8-bit form, the exact OVMF shape: cmpb $0x2, 0x37(%rdi). */
    {
        static const uint8_t ovmf_cmp[] = {0x80, 0x7F, 0x37, 0x02};
        hype_mmio_decode_t d;
        CHECK_HEX("OVMF cmpb decodes", 0u, (unsigned)hype_mmio_decode(ovmf_cmp, 4u, &d));
        CHECK_HEX("OVMF cmpb width", 1u, (unsigned)d.size_bytes);
        CHECK_HEX("OVMF cmpb is a READ", 0u, (unsigned)d.is_write);
        CHECK_HEX("OVMF cmpb immediate", 0x2u, d.imm_value);
        CHECK_HEX("OVMF cmpb length", 4u, (unsigned)d.instr_len);
    }
    /* The memory-SOURCE CMP (0x3B) is a different instruction and stays supported. */
    {
        static const uint8_t src_cmp[] = {0x3B, 0x01};
        hype_mmio_decode_t d;
        CHECK_HEX("mem-source CMP still decodes", 0u, (unsigned)hype_mmio_decode(src_cmp, 2u, &d));
        CHECK_HEX("and memory is NOT its destination", 0u, (unsigned)d.mem_is_dst);
    }
}

static void test_rmw_rejects_a_truncated_immediate(void) {
    static const uint8_t insn[] = {0x83, 0x0A}; /* immediate missing entirely */
    hype_mmio_decode_t d;

    CHECK_HEX("rejected", (unsigned)-1, (unsigned)hype_mmio_decode(insn, 2u, &d));
}

static void test_rmw_length_across_addressing_forms(void) {
    struct { const char *name; uint8_t b[12]; unsigned len; unsigned expect; } cases[] = {
        {"no disp", {0x83, 0x0A, 0x02}, 3u, 3u},
        {"disp8", {0x83, 0x4A, 0x04, 0x02}, 4u, 4u},
        {"disp32", {0x83, 0x8A, 0x04, 0x00, 0x00, 0x00, 0x02}, 7u, 7u},
        {"SIB", {0x83, 0x0C, 0x08, 0x02}, 4u, 4u},
        {"imm32 + disp8", {0x81, 0x4A, 0x04, 0x78, 0x56, 0x34, 0x12}, 7u, 7u},
    };
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hype_mmio_decode_t d;
        CHECK_HEX(cases[i].name, 0u, (unsigned)hype_mmio_decode(cases[i].b, cases[i].len, &d));
        CHECK_HEX("instr_len", cases[i].expect, d.instr_len);
    }
}

static void test_rmw_value_is_a_no_op_for_a_plain_store(void) {
    /* A handler that reaches the RMW resolver on a non-RMW decode must write back what it
     * read, not a value derived from an operand the instruction never had. */
    static const uint8_t insn[] = {0x89, 0x01}; /* mov (%rcx), %eax */
    hype_mmio_decode_t d;
    uint64_t rflags = 0x1u;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 2u, &d));
    CHECK_HEX("returns the memory value untouched", 0x1234u,
              hype_mmio_rmw_value(&d, 0xFFFFu, 0x1234u, &rflags));
    CHECK_HEX("and leaves flags alone", 0x1u, (unsigned)rflags);
    CHECK_HEX("a null decode is also safe", 0x99u,
              hype_mmio_rmw_value((const hype_mmio_decode_t *)0, 0u, 0x99u, &rflags));
}

static void test_rejects_unaddressable_byte_registers(void) {
    /*
     * Without REX, ModRM.reg 4-7 name AH/CH/DH/BH. Every caller resolves an encoding to a
     * whole 64-bit register and takes its low byte, which cannot express a high-byte
     * operand -- so `mov %ch,(%rax)` would transfer CL's value instead of CH's. Refused.
     */
    static const uint8_t no_rex[] = {0x88, 0x28};      /* mov %ch,(%rax) : reg=101 */
    static const uint8_t with_rex[] = {0x40, 0x88, 0x28}; /* mov %bpl,(%rax) : REX makes it legal */
    hype_mmio_decode_t d;

    CHECK_HEX("high-byte register refused", (unsigned)-1, (unsigned)hype_mmio_decode(no_rex, 2u, &d));
    CHECK_HEX("REX form accepted", 0u, (unsigned)hype_mmio_decode(with_rex, 3u, &d));
    CHECK_HEX("and it is register 5", 5u, d.reg);
    /* The dword forms are unaffected -- reg 4-7 are plain ESP/EBP/ESI/EDI there. */
    {
        static const uint8_t dword[] = {0x89, 0x28}; /* mov %ebp,(%rax) */
        CHECK_HEX("dword reg 5 still fine", 0u, (unsigned)hype_mmio_decode(dword, 2u, &d));
        CHECK_HEX("reg", 5u, d.reg);
    }
}

/*
 * #575: `TEST r/m, imm` -- group 3 /0, opcodes F6 and F7. A guest driver testing a bit in a
 * device register can legitimately emit it, and hype STOPPED the VM for it.
 */
static void test_immediate_test_decodes_as_a_flags_only_read(void) {
    /*
     * THE EXACT INSTRUCTION FROM THE TICKET, byte for byte:
     *   41 f7 84 19 04 00 00 20 00 00 00 02
     * = testl $0x02000000, 0x20000004(%r9,%rbx,1)
     * REX.B, F7 /0, ModRM 0x84 (mod=10 disp32, reg=000, rm=100 -> SIB), SIB 0x19, disp32, imm32.
     * clang folded a volatile readl() plus a shift-and-mask plus a compare into it at -O2.
     */
    {
        static const uint8_t insn[] = {0x41, 0xF7, 0x84, 0x19, 0x04, 0x00, 0x00, 0x20,
                                       0x00, 0x00, 0x00, 0x02};
        hype_mmio_decode_t d;
        CHECK_HEX("the ticket's testl decodes", 0u, (unsigned)hype_mmio_decode(insn, 12u, &d));
        CHECK_HEX("it is a READ", 0u, (unsigned)d.is_write);
        CHECK_HEX("memory is not its destination", 0u, (unsigned)d.mem_is_dst);
        CHECK_HEX("it carries an immediate", 1u, (unsigned)d.has_imm);
        CHECK_HEX("the op is TEST", (unsigned)HYPE_MMIO_ALU_TEST, (unsigned)d.op);
        CHECK_HEX("32-bit operand", 4u, (unsigned)d.size_bytes);
        CHECK_HEX("imm32 read whole", 0x02000000u, d.imm_value);
        /* The length is the thing that must not be guessed: resuming the guest anywhere but
         * exactly past the imm32 lands it inside its own instruction stream. */
        CHECK_HEX("instr_len covers REX+opcode+ModRM+SIB+disp32+imm32", 12u, (unsigned)d.instr_len);
    }
    /* 8-bit form: testb $0x20, 0x10(%rax). imm is ONE byte for F6. */
    {
        static const uint8_t insn[] = {0xF6, 0x40, 0x10, 0x20};
        hype_mmio_decode_t d;
        CHECK_HEX("testb decodes", 0u, (unsigned)hype_mmio_decode(insn, 4u, &d));
        CHECK_HEX("testb width", 1u, (unsigned)d.size_bytes);
        CHECK_HEX("testb is a READ", 0u, (unsigned)d.is_write);
        CHECK_HEX("testb immediate", 0x20u, d.imm_value);
        CHECK_HEX("testb length", 4u, (unsigned)d.instr_len);
    }
    /* 16-bit form via the 0x66 prefix: imm is TWO bytes, not four. */
    {
        static const uint8_t insn[] = {0x66, 0xF7, 0x00, 0x34, 0x12};
        hype_mmio_decode_t d;
        CHECK_HEX("testw decodes", 0u, (unsigned)hype_mmio_decode(insn, 5u, &d));
        CHECK_HEX("testw width", 2u, (unsigned)d.size_bytes);
        CHECK_HEX("testw immediate", 0x1234u, d.imm_value);
        CHECK_HEX("testw length", 5u, (unsigned)d.instr_len);
    }
    /*
     * There is NO sign-extended-imm8 short form in group 3, unlike 0x81/0x83. A plain
     * `testl $imm32, (%rax)` is 6 bytes; reading the immediate as one byte would resume the
     * guest three bytes into its own immediate.
     */
    {
        static const uint8_t insn[] = {0xF7, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
        hype_mmio_decode_t d;
        CHECK_HEX("testl imm32 decodes", 0u, (unsigned)hype_mmio_decode(insn, 6u, &d));
        CHECK_HEX("testl imm32 is not truncated", 0xFFFFFFFFu, d.imm_value);
        CHECK_HEX("testl imm32 length is 6, not 3", 6u, (unsigned)d.instr_len);
    }
    /* A truncated fetch must be refused, not decoded with a half-read immediate. */
    {
        static const uint8_t insn[] = {0xF7, 0x00, 0x01, 0x00};
        hype_mmio_decode_t d;
        CHECK_HEX("imm32 not fully present is refused", 1u,
                  (unsigned)(hype_mmio_decode(insn, 4u, &d) != 0));
    }
    /*
     * Intel SDM Table A-6, group 3: /0 TEST, /1 blank, /2 NOT, /3 NEG, /4 MUL, /5 IMUL,
     * /6 DIV, /7 IDIV. Only /0 may decode. NOT and NEG are memory-destination RMWs and
     * MUL..IDIV write rDX:rAX implicitly, so decoding any of them as a TEST would silently
     * corrupt a device register or a register pair.
     */
    {
        unsigned int ext;
        for (ext = 1u; ext < 8u; ext++) {
            uint8_t insn[6];
            hype_mmio_decode_t d;
            insn[0] = 0xF7u;
            insn[1] = (uint8_t)(0x00u | (ext << 3)); /* mod=00, reg=ext, rm=000 -> [rax] */
            insn[2] = 0x01u;
            insn[3] = 0x00u;
            insn[4] = 0x00u;
            insn[5] = 0x00u;
            CHECK_HEX("group 3 non-/0 extension refused", 1u,
                      (unsigned)(hype_mmio_decode(insn, 6u, &d) != 0));
        }
    }
}

/* The read-completion tail must treat an immediate TEST as flags-only: no register is written,
 * and the flags come from memory AND immediate. */
static void test_immediate_test_writes_flags_and_no_register(void) {
    static const uint8_t insn[] = {0xF7, 0x00, 0x04, 0x00, 0x00, 0x00}; /* testl $4, (%rax) */
    hype_mmio_decode_t d;
    uint64_t reg = 0xDEADBEEFCAFEBABEULL;
    uint64_t rflags = 0;

    CHECK_HEX("decodes", 0u, (unsigned)hype_mmio_decode(insn, 6u, &d));

    /* Bit 2 SET in the device register -> ZF clear. */
    rflags = 0;
    hype_mmio_complete_read(&d, &reg, 0x00000004u, &rflags);
    CHECK_HEX("register untouched by TEST", 0xDEADBEEFCAFEBABEULL, reg);
    CHECK_HEX("ZF clear when the bit is set", 0u, (unsigned)(rflags & (1u << 6)));

    /* Bit 2 CLEAR -> ZF set. This is the branch the guest driver actually takes. */
    rflags = 0;
    hype_mmio_complete_read(&d, &reg, 0x00000002u, &rflags);
    CHECK_HEX("register still untouched", 0xDEADBEEFCAFEBABEULL, reg);
    CHECK_HEX("ZF set when the bit is clear", (1u << 6), (unsigned)(rflags & (1u << 6)));
}

int main(void) {
    test_decodes_mov_m32_imm32();
    test_decodes_mov_m8_imm8();
    test_imm_length_is_added_for_each_addressing_form();
    test_imm_store_rejects_a_non_zero_opcode_extension();
    test_imm_store_rejects_a_truncated_immediate();
    test_store_value_falls_back_to_the_register();
    test_decodes_and_r32_m32();
    test_decodes_the_rest_of_the_group();
    test_mov_is_still_a_mov();
    test_alu_bitwise_flags();
    test_alu_test_and_cmp_do_not_produce_a_register_value_by_accident();
    test_immediate_test_decodes_as_a_flags_only_read();  /* #575 */
    test_immediate_test_writes_flags_and_no_register();  /* #575 */
    test_alu_arithmetic_flags();
    test_alu_respects_operand_width();
    test_alu_null_rflags_is_safe();
    test_mov_store_byte_no_rex();
    test_mov_store_byte_rex_r();
    test_mov_load_byte();
    test_mov_store_dword();
    test_mov_load_dword_zero_extends();
    test_mov_store_word_prefix();
    test_mov_load_word_prefix_no_zero_extend();
    test_movzx_byte();
    test_movzx_byte_with_rex();
    test_movzx_word();
    test_rejects_unrecognized_opcode();
    test_rejects_unrecognized_0f_opcode();
    test_rejects_zero_length();
    test_rejects_truncated_after_prefix();
    test_rejects_truncated_after_rex();
    test_rejects_truncated_after_opcode();
    test_rejects_truncated_after_0f();
    test_rejects_truncated_0f_before_modrm();
    test_merge_read_value_zero_extend();
    test_merge_read_value_no_zero_extend_byte();
    test_merge_read_value_no_zero_extend_word();
    test_merge_read_value_dword_zero_extend();
    test_extract_write_value_byte();
    test_extract_write_value_word();
    test_extract_write_value_dword();
    test_disp8_addressing();
    test_disp32_addressing();
    test_sib_with_base_no_disp();
    test_sib_no_base_disp32();
    test_rip_relative_addressing();
    test_rejects_register_direct();
    test_rejects_truncated_sib();
    test_rejects_truncated_disp8();
    test_rejects_truncated_disp32();

    test_decodes_the_freebsd_ahci_ghc_write();
    test_group1_ops_and_widths();
    test_group1_imm32_and_word_widths();
    test_group1_imm8_is_sign_extended();
    test_reg_to_mem_ops();
    test_rmw_source_is_the_register_when_there_is_no_immediate();
    test_rmw_direction_matters_for_subtraction();
    test_rmw_sets_flags_from_the_result();
    test_rmw_refuses_adc_sbb_and_cmp();
    test_rmw_rejects_a_truncated_immediate();
    test_rmw_length_across_addressing_forms();
    test_rmw_value_is_a_no_op_for_a_plain_store();
    test_rejects_unaddressable_byte_registers();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
