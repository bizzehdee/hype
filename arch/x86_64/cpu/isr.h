#ifndef HYPE_ARCH_ISR_H
#define HYPE_ARCH_ISR_H

#include <stdint.h>

/*
 * Register/vector state as laid out on the stack by isr_stubs.S's
 * common trampoline before it calls hype_isr_dispatch(). Field order
 * matches ascending stack address (i.e. push order in reverse) exactly
 * -- see isr_stubs.S's comment for the full derivation. Getting this
 * order wrong silently reads the wrong register as e.g. `vector`,
 * which is the kind of bug that's easy to miss by inspection, which is
 * why hype_isr_format_message() below is unit tested against a frame
 * built the same way a human would hand-verify it.
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} hype_isr_frame_t;

/* Human-readable name for one of the SDM's 32 architectural exception
 * vectors; vectors 32-255 are user-defined/IRQ, unused before M1-8. */
const char *hype_isr_vector_name(uint64_t vector);

/* Formats the fatal-fault message hype_isr_dispatch() prints. Pure/
 * testable -- see isr_decode.c for why dispatch itself isn't. */
void hype_isr_format_message(char *buf, unsigned long long bufsz, const hype_isr_frame_t *frame);

/*
 * Called by isr_stubs.S's common trampoline for every one of the 256
 * vectors. For M1, every fault is fatal: format a message, print it via
 * serial (never ConOut -- see isr_entry.c for why that's not optional),
 * halt. Never returns. Not unit tested -- it's a thin wrapper around
 * the tested hype_isr_format_message() plus the noreturn
 * hype_halt_forever(), same reasoning as hype_panic() itself (halt.h).
 */
void hype_isr_dispatch(const hype_isr_frame_t *frame);

#endif /* HYPE_ARCH_ISR_H */
