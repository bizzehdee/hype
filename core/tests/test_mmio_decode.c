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
    uint8_t bytes[] = {0x01u, 0x03u}; /* ADD, not a MOV form we support */
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

int main(void) {
    test_decodes_and_r32_m32();
    test_decodes_the_rest_of_the_group();
    test_mov_is_still_a_mov();
    test_alu_bitwise_flags();
    test_alu_test_and_cmp_do_not_produce_a_register_value_by_accident();
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

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
