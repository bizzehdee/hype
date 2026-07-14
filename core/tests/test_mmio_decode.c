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

int main(void) {
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
