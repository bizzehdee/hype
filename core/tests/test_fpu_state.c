#include <stdio.h>
#include "../../arch/x86_64/cpu/fpu_state.h"

/* #260: the per-vCPU FXSAVE image. The instructions themselves are hardware and
 * are verified by disassembly + the in-guest digest test; what is testable here is
 * the image the first FXRSTOR sees, which is where a plausible-looking mistake
 * (memset to zero) silently unmasks every SIMD exception in the guest. */

static int failures = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static void test_reset_sets_architectural_defaults(void) {
    hype_fpu_area_t a;
    unsigned int i;

    for (i = 0; i < HYPE_FPU_AREA_BYTES; i++) {
        a.bytes[i] = 0xAA; /* poison, so the reset has to do real work */
    }
    hype_fpu_area_reset(&a);

    expect(hype_fpu_area_fcw(&a) == HYPE_FPU_FCW_RESET, "reset leaves FCW at 0x037F");
    expect(hype_fpu_area_mxcsr(&a) == HYPE_FPU_MXCSR_RESET, "reset leaves MXCSR at 0x1F80");
    expect(hype_fpu_area_mxcsr(&a) != 0u,
           "reset does NOT leave MXCSR zero (that would unmask every SIMD exception)");
}

static void test_reset_clears_the_register_area(void) {
    hype_fpu_area_t a;
    unsigned int i;
    int nonzero = 0;

    for (i = 0; i < HYPE_FPU_AREA_BYTES; i++) {
        a.bytes[i] = 0xAA;
    }
    hype_fpu_area_reset(&a);
    /* Every XMM register slot must be zeroed: a fresh vCPU must not inherit
     * whatever a previously-run vCPU left in that pool slot. */
    for (i = HYPE_FPU_OFF_XMM0; i < HYPE_FPU_AREA_BYTES; i++) {
        if (a.bytes[i] != 0u) {
            nonzero++;
        }
    }
    expect(nonzero == 0, "reset zeroes the XMM0-15 register area");
}

static void test_mxcsr_validation(void) {
    hype_fpu_area_t a;

    hype_fpu_area_reset(&a);
    expect(hype_fpu_area_mxcsr_valid(&a) == 1, "the reset image passes MXCSR validation");

    /* Bit 16 is outside the architectural MXCSR mask; FXRSTOR would #GP on it. */
    a.bytes[HYPE_FPU_OFF_MXCSR + 2] = 0x01u;
    expect(hype_fpu_area_mxcsr_valid(&a) == 0,
           "an MXCSR with reserved bits set is rejected (FXRSTOR would #GP)");
}

static void test_null_is_tolerated(void) {
    hype_fpu_area_reset(0); /* must not crash */
    expect(hype_fpu_area_mxcsr_valid(0) == 0, "a NULL area is not reported valid");
    expect(hype_fpu_area_mxcsr(0) == 0u, "MXCSR of a NULL area reads 0");
    expect(hype_fpu_area_fcw(0) == 0u, "FCW of a NULL area reads 0");
}

static void test_area_is_fxsave_sized_and_aligned(void) {
    hype_fpu_area_t a;

    expect(sizeof(a) == 512u, "the area is FXSAVE64's architectural 512 bytes");
    expect((((unsigned long)(void *)&a) & 15u) == 0u,
           "the area is at least 16-byte aligned, as FXSAVE/FXRSTOR require");
}

int main(void) {
    test_reset_sets_architectural_defaults();
    test_reset_clears_the_register_area();
    test_mxcsr_validation();
    test_null_is_tolerated();
    test_area_is_fxsave_sized_and_aligned();

    if (failures == 0) {
        printf("test_fpu_state: all checks passed\n");
        return 0;
    }
    printf("test_fpu_state: %d failure(s)\n", failures);
    return 1;
}
