/*
 * #604: the two freestanding symbols clang's -fstack-protector-strong codegen expects to find
 * itself (no libc/CRT here to supply them). __stack_chk_guard starts at a fixed link-time constant
 * so every stack-protected function has something to compare against from the very first
 * instruction executed, before hype_stack_protector_init() (below) reseeds it from the TSC.
 *
 * #711 spent real effort suspecting this exact mechanism was broken for hype's clang+ld.lld
 * x86_64-unknown-uefi target -- it compiled and linked clean but the resulting hype.efi never
 * booted. A from-scratch minimal repro (same target, same flags, same ld.lld invocation, run under
 * this project's own vendored OVMF) booted and ran the stack-protected code correctly, and
 * re-attempting the exact change described there against the current toolchain (clang/lld 22.1.8)
 * now also boots clean, repeatedly. Whatever combination produced #711's original failure -- most
 * likely a since-fixed clang/lld codegen bug for this target, given nothing here or in
 * boot/main.c's own huge efi_main() changed -- no longer reproduces.
 */
#include <stdint.h>

#include "fatal.h"
#include "stack_protector.h"

uintptr_t __stack_chk_guard = 0x1122334455667788ULL;

__attribute__((noreturn)) void __stack_chk_fail(void) {
    hype_fatal("stack smashing detected [#604]\n");
}

static inline uint64_t sp_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void hype_stack_protector_init(void) {
    __stack_chk_guard = sp_rdtsc() ^ 0xDEADBEEFCAFEBABEULL;
}
