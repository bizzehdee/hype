#ifndef HYPE_FPU_STATE_H
#define HYPE_FPU_STATE_H

#include <stdint.h>

/*
 * #260: per-vCPU x87/SSE register state, saved and restored across VMRUN.
 *
 * WHY THIS EXISTS. AMD SVM does not save or restore FPU/SSE state in the VMCB --
 * that is the hypervisor's job, and hype was not doing it. Meanwhile hype's own
 * compiled handlers DO use XMM registers: clang's baseline x86-64 target emits
 * movups/movaps for struct copies (172 such instructions in one build), and
 * ap_trampoline.S already sets CR4.OSFXSR precisely because "Compiled C uses XMM
 * registers". So every #VMEXIT could clobber the guest's vector registers.
 *
 * The corruption is probabilistic per exit -- it needs the exit to land inside a
 * guest vector loop AND hype's handler to touch the same register -- so guests
 * boot fine and only LONG vector computations come out wrong. That is exactly how
 * it was found: in-guest openssl produced a wrong SHA-256 for a 121 MiB input
 * while busybox's plain-C sha256sum over the same bytes matched the host exactly.
 *
 * WHY FXSAVE AND NOT XSAVE. XSAVE's save mask is XCR0, and on SVM XCR0 is *shared*
 * with the guest -- XSETBV is not intercepted, so the guest owns that register.
 * Basing hype's save on it would mean depending on state hype does not control.
 * FXSAVE64 needs no mask and no CR4.OSXSAVE: it covers x87, MXCSR and XMM0-15,
 * which is precisely what legacy-SSE code destroys.
 *
 * THE INVARIANT THIS RELIES ON. Legacy SSE writes to XMMn leave YMMn[255:128]
 * unmodified (that is why the AVX/SSE transition penalty and VZEROUPPER exist), so
 * a guest's AVX upper halves survive hype's XMM use without being saved. That holds
 * only while hype itself emits no VEX/EVEX instructions. It does not today (zero
 * %ymm/%zmm, zero VEX in the binary; the build passes no -march/-mavx, so clang's
 * baseline is SSE2), and tools/check-no-vex.sh enforces it so a future -march or an
 * AVX-emitting builtin cannot silently invalidate the reasoning here.
 */

/* FXSAVE64's architectural image size. */
#define HYPE_FPU_AREA_BYTES 512u

/* Field offsets within an FXSAVE image (Intel SDM Vol.1 "FXSAVE Area Layout"). */
#define HYPE_FPU_OFF_FCW 0u
#define HYPE_FPU_OFF_MXCSR 24u
#define HYPE_FPU_OFF_MXCSR_MASK 28u
#define HYPE_FPU_OFF_XMM0 160u

/* Reset values. A zeroed image is NOT a safe starting point: MXCSR would be 0,
 * which unmasks every SIMD exception and would fault the guest on its first
 * denormal. These are the architectural post-reset values. */
#define HYPE_FPU_FCW_RESET 0x037Fu
#define HYPE_FPU_MXCSR_RESET 0x1F80u

/* 16-byte alignment is an FXSAVE/FXRSTOR requirement; 64 costs nothing and keeps
 * the image off a shared cache line with adjacent per-vCPU fields. */
typedef struct {
    uint8_t bytes[HYPE_FPU_AREA_BYTES];
} __attribute__((aligned(64))) hype_fpu_area_t;

/*
 * Fill `area` with the architectural reset image, so the first FXRSTOR before a
 * vCPU has ever run loads a sane state rather than whatever the buffer held.
 * Pure: no hardware access, unit-tested.
 */
void hype_fpu_area_reset(hype_fpu_area_t *area);

/*
 * 1 if `area` is a plausible FXSAVE image to hand to FXRSTOR -- specifically that
 * MXCSR has no bits set outside the architectural mask, which is the one field
 * FXRSTOR itself will #GP on. Pure: unit-tested. Used to assert the reset image
 * and any restored image are well-formed before trusting them.
 */
int hype_fpu_area_mxcsr_valid(const hype_fpu_area_t *area);

/* Read/write helpers so tests can inspect an image without duplicating offsets. */
uint32_t hype_fpu_area_mxcsr(const hype_fpu_area_t *area);
uint16_t hype_fpu_area_fcw(const hype_fpu_area_t *area);

/*
 * Hardware ops (fpu_state_hw.c). Must bracket VMRUN with NOTHING in between that
 * could touch XMM -- in particular no hype_debug_print, which does.
 */
void hype_fpu_save(hype_fpu_area_t *area);
void hype_fpu_restore(const hype_fpu_area_t *area);

#endif /* HYPE_FPU_STATE_H */
