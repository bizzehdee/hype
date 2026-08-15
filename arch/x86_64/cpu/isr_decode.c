#include "isr.h"
#include "../../../core/fatal.h"
#include "../../../core/format.h"

const char *hype_isr_vector_name(uint64_t vector) {
    switch (vector) {
    case 0: return "Divide Error";
    case 1: return "Debug";
    case 2: return "NMI";
    case 3: return "Breakpoint";
    case 4: return "Overflow";
    case 5: return "BOUND Range Exceeded";
    case 6: return "Invalid Opcode";
    case 7: return "Device Not Available";
    case 8: return "Double Fault";
    case 9: return "Coprocessor Segment Overrun";
    case 10: return "Invalid TSS";
    case 11: return "Segment Not Present";
    case 12: return "Stack-Segment Fault";
    case 13: return "General Protection Fault";
    case 14: return "Page Fault";
    case 15: return "Reserved";
    case 16: return "x87 Floating-Point Exception";
    case 17: return "Alignment Check";
    case 18: return "Machine Check";
    case 19: return "SIMD Floating-Point Exception";
    case 20: return "Virtualization Exception";
    case 21: return "Control Protection Exception";
    case 28: return "Hypervisor Injection Exception";
    case 29: return "VMM Communication Exception";
    case 30: return "Security Exception";
    default:
        if (vector <= 31) {
            return "Reserved";
        }
        return "IRQ/User-Defined";
    }
}

void hype_isr_format_message(char *buf, unsigned long long bufsz, const hype_isr_frame_t *frame,
                             unsigned int apic_id) {
    hype_snprintf(buf, bufsz,
                  "unhandled interrupt on apic=%u: vector=%llu (%s) error_code=0x%llx "
                  "rip=0x%llx cs=0x%llx rflags=0x%llx rsp=0x%llx ss=0x%llx rbp=0x%llx "
                  "-- apic=%u HALTS, other cores keep running [#461]",
                  apic_id, frame->vector, hype_isr_vector_name(frame->vector), frame->error_code,
                  frame->rip, frame->cs, frame->rflags, frame->rsp, frame->ss, frame->rbp,
                  apic_id);
}

/* Only vectors 32-255 are ever populated -- see hype_isr_register(). */
static hype_isr_handler_fn g_handlers[256];

int hype_isr_register(uint8_t vector, hype_isr_handler_fn handler) {
    if (vector < 32) {
        return 0;
    }
    g_handlers[vector] = handler;
    return 1;
}

/*
 * The handler-found branch is fully testable (register a handler,
 * dispatch a frame for its vector, confirm it was called and dispatch
 * returned). The no-handler branch calls the noreturn hype_fatal() and
 * is deliberately not exercised in tests -- doing so would hang the
 * test binary rather than verify anything, same reasoning as
 * hype_fatal() itself (halt.h).
 */
int hype_isr_dispatch_vector(uint8_t vector) {
    hype_isr_frame_t frame;
    hype_isr_handler_fn handler = g_handlers[vector];

    if (handler == 0) {
        return 0;
    }
    /*
     * Zeroed field-by-field on purpose. Aggregate initialisation of a struct
     * this size emits a memset call, which does not link on the freestanding
     * UEFI target (no libc) -- and leaving the fields uninitialised would hand a
     * handler stack garbage as register state.
     */
    frame.r15 = 0; frame.r14 = 0; frame.r13 = 0; frame.r12 = 0;
    frame.r11 = 0; frame.r10 = 0; frame.r9 = 0; frame.r8 = 0;
    frame.rbp = 0; frame.rdi = 0; frame.rsi = 0; frame.rdx = 0;
    frame.rcx = 0; frame.rbx = 0; frame.rax = 0;
    frame.vector = (uint64_t)vector;
    frame.error_code = 0;
    frame.rip = 0; frame.cs = 0; frame.rflags = 0; frame.rsp = 0; frame.ss = 0;
    handler(&frame);
    return 1;
}

/*
 * #461: read only on the panic path, never on the handler-found path the unit tests exercise --
 * which is why this is a local read rather than a parameter threaded through every ISR entry.
 */
static unsigned int isr_this_apic(void) {
    return (unsigned int)((*(volatile uint32_t *)(uintptr_t)0xFEE00020u) >> 24);
}

void hype_isr_dispatch(const hype_isr_frame_t *frame) {
    char msg[320]; /* #461: the message now carries the core, the stack and the halt note. */
    hype_isr_handler_fn handler;
    unsigned int apic;

    handler = (frame->vector < 256) ? g_handlers[frame->vector] : 0;
    if (handler != 0) {
        handler(frame);
        return;
    }

    apic = isr_this_apic();
    /* Recorded BEFORE panicking, so the surviving cores can report the loss even if the panic
     * path itself dies painting the framebuffer. */
    hype_fatal_note_core_panic(apic);
    hype_isr_format_message(msg, sizeof(msg), frame, apic);
    hype_fatal("%s", msg);
}
