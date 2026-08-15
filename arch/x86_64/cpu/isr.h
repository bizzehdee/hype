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
 * vectors; vectors 32-255 are user-defined/IRQ. */
const char *hype_isr_vector_name(uint64_t vector);

/*
 * Formats the fatal-fault message hype_isr_dispatch() panics with when no handler is registered
 * for a vector. Pure/testable -- see isr_decode.c for why dispatch itself only partly is, which
 * is also why `apic_id` is a parameter rather than an MMIO read in here.
 *
 * #461: the message used to omit both the core and the stack. A stray GP fault with
 * `rip=0x9f01f cs=0x8` was unattributable -- there was no way to tell which core died, and
 * therefore no way to tell whether a VM had just lost its vCPU. It also has to say that the
 * halt is CORE-LOCAL: hype_fatal() masks interrupts and halts the calling core only, so the
 * other cores carry on and the log keeps flowing, which reads as "hype survived a panic" when
 * what actually happened is that one core is gone and nothing noticed.
 */
void hype_isr_format_message(char *buf, unsigned long long bufsz, const hype_isr_frame_t *frame,
                             unsigned int apic_id);

typedef void (*hype_isr_handler_fn)(const hype_isr_frame_t *frame);

/*
 * Registers a handler for a hardware IRQ vector (32-255) that returns
 * normally instead of being fatal -- isr_stubs.S's trampoline restores
 * all registers and `iretq`s when hype_isr_dispatch() returns. Vectors
 * 0-31 (the SDM's architectural exceptions) can't be registered this
 * way and are always fatal; M1 has no recovery story for a genuine CPU
 * fault, only for expected hardware IRQs (the timer, M1-8; more devices
 * later). Returns 1 on success, 0 if vector is out of the registerable
 * range. Last registration for a given vector wins.
 */
int hype_isr_register(uint8_t vector, hype_isr_handler_fn handler);

/*
 * Called by isr_stubs.S's common trampoline for every one of the 256
 * vectors. If a handler is registered for this vector (only possible
 * for 32-255), calls it and returns -- the testable, common case now
 * that M1-8 needs the timer IRQ to resume normally rather than panic.
 * Otherwise formats a message and calls the noreturn hype_fatal():
 * that branch isn't unit tested (calling it in a test would hang the
 * test binary rather than verify anything), same reasoning as
 * hype_fatal() itself (halt.h).
 */
void hype_isr_dispatch(const hype_isr_frame_t *frame);

/*
 * #248: run the handler for `vector` without a trampoline frame, reporting
 * whether one existed instead of panicking when it did not.
 *
 * Needed because VMX's "acknowledge interrupt on exit" has the CPU perform the
 * interrupt-acknowledge cycle during the VM exit: the interrupt is consumed, so
 * the host IDT never fires for it, and hype must invoke the handler itself using
 * the vector reported in VM_EXIT_INTR_INFO.
 *
 * Deliberately NOT hype_isr_dispatch() with a hand-built frame: that panics via
 * hype_fatal() on an unregistered vector, which is wrong for an interrupt the
 * hardware acknowledged and hype merely has no interest in -- and it would mean
 * synthesising 21 register fields we do not have, inviting exactly the
 * uninitialised-struct class of bug this codebase has been bitten by. Returns 1
 * if a handler ran, 0 if none was registered (caller decides what that means).
 * The frame passed to the handler is fully zeroed except `vector`; handlers that
 * need real register state must not be dispatched this way.
 */
int hype_isr_dispatch_vector(uint8_t vector);

#endif /* HYPE_ARCH_ISR_H */
