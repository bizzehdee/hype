#include "isr.h"
#include "../../../core/format.h"

/* hype_isr_dispatch() itself lives in isr_entry.c, alongside nothing
 * else -- it calls the noreturn hype_panic(), so like hype_panic()
 * itself (see halt.c), calling it would hang a test rather than verify
 * anything. Keeping it in its own file keeps this file's coverage
 * numbers meaningful instead of diluted by an untestable wrapper. */

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
