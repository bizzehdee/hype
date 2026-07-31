#include "fpu_state.h"

/*
 * #260: the two instructions that actually move the guest's x87/SSE state.
 *
 * Split out from fpu_state.c so the pure image logic stays unit-testable on the
 * host, matching the _hw split used elsewhere in this tree.
 *
 * CR4.OSFXSR is already set on every core that reaches here: the BSP inherits it
 * from UEFI (which itself uses SSE), and ap_trampoline.S sets it explicitly before
 * entering C. So neither of these can #UD.
 */

void hype_fpu_save(hype_fpu_area_t *area) {
    __asm__ volatile("fxsave64 %0" : "=m"(*area) : : "memory");
}

void hype_fpu_restore(const hype_fpu_area_t *area) {
    __asm__ volatile("fxrstor64 %0" : : "m"(*area) : "memory");
}
