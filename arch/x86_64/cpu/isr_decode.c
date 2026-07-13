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

void hype_isr_format_message(char *buf, unsigned long long bufsz, const hype_isr_frame_t *frame) {
    hype_snprintf(buf, bufsz,
                  "unhandled interrupt: vector=%llu (%s) error_code=0x%llx rip=0x%llx cs=0x%llx rflags=0x%llx",
                  frame->vector, hype_isr_vector_name(frame->vector), frame->error_code,
                  frame->rip, frame->cs, frame->rflags);
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
void hype_isr_dispatch(const hype_isr_frame_t *frame) {
    char msg[192];
    hype_isr_handler_fn handler;

    handler = (frame->vector < 256) ? g_handlers[frame->vector] : 0;
    if (handler != 0) {
        handler(frame);
        return;
    }

    hype_isr_format_message(msg, sizeof(msg), frame);
    hype_fatal("%s", msg);
}
