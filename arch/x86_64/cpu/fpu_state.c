#include "fpu_state.h"

/* #260: pure helpers for the per-vCPU FXSAVE image. See fpu_state.h for why this
 * uses FXSAVE rather than XSAVE, and what invariant that rests on. */

/* Bits FXRSTOR accepts in MXCSR. Anything outside this raises #GP, so it is the
 * one field worth validating before handing an image to the CPU. */
#define HYPE_FPU_MXCSR_LEGAL 0x0000FFBFu

static void store16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t load16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void hype_fpu_area_reset(hype_fpu_area_t *area) {
    unsigned int i;

    if (area == 0) {
        return;
    }
    /* Byte loop, not a struct assignment: there is no memcpy at EFI link time. */
    for (i = 0; i < HYPE_FPU_AREA_BYTES; i++) {
        area->bytes[i] = 0;
    }
    store16(&area->bytes[HYPE_FPU_OFF_FCW], (uint16_t)HYPE_FPU_FCW_RESET);
    store32(&area->bytes[HYPE_FPU_OFF_MXCSR], (uint32_t)HYPE_FPU_MXCSR_RESET);
    /* MXCSR_MASK 0 is architecturally read as 0xFFBF by FXRSTOR; write it
     * explicitly so an image dumped for debugging reads unambiguously. */
    store32(&area->bytes[HYPE_FPU_OFF_MXCSR_MASK], HYPE_FPU_MXCSR_LEGAL);
}

int hype_fpu_area_mxcsr_valid(const hype_fpu_area_t *area) {
    uint32_t mxcsr;

    if (area == 0) {
        return 0;
    }
    mxcsr = load32(&area->bytes[HYPE_FPU_OFF_MXCSR]);
    return (mxcsr & ~HYPE_FPU_MXCSR_LEGAL) == 0u;
}

uint32_t hype_fpu_area_mxcsr(const hype_fpu_area_t *area) {
    return (area == 0) ? 0u : load32(&area->bytes[HYPE_FPU_OFF_MXCSR]);
}

uint16_t hype_fpu_area_fcw(const hype_fpu_area_t *area) {
    return (area == 0) ? 0u : load16(&area->bytes[HYPE_FPU_OFF_FCW]);
}
