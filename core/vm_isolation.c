#include "vm_isolation.h"

unsigned hype_vm_isolation_check(uint64_t a_ram_base, uint64_t a_ram_size, uint64_t a_root,
                                 uint64_t b_ram_base, uint64_t b_ram_size, uint64_t b_root) {
    unsigned flags = HYPE_VM_ISOLATION_OK;
    uint64_t a_end, b_end;

    if (a_ram_size == 0 || b_ram_size == 0 || a_ram_base == 0 || b_ram_base == 0 || a_root == 0 ||
        b_root == 0) {
        /* Report and keep going: an unconfigured VM would otherwise "pass" the
         * overlap test trivially, which is the wrong answer to give. */
        flags |= HYPE_VM_ISOLATION_UNCONFIGURED;
    }

    if (a_root == b_root) {
        flags |= HYPE_VM_ISOLATION_SAME_ROOT;
    }

    /*
     * Half-open ranges: [base, base+size).
     *
     * A range whose base+size carries past the top of the address space is
     * reported UNCONFIGURED rather than quietly clamped. Clamping to ~0 answers
     * correctly for the part above `base` but silently drops the part that wrapped
     * round to low addresses, so a guest sitting down there would be declared
     * isolated from a range that in fact reaches it. Guest RAM never legitimately
     * wraps, so the honest answer to an impossible input is "do not trust this",
     * not a confident half-right verdict. The clamp still happens afterwards so
     * the overlap test below stays well-defined.
     *
     * A range ending EXACTLY at 2^64 is a legal boundary, not an overflow: the
     * half-open end is simply 0. Clamp it without complaint, and reserve the
     * flag for an end that genuinely landed back inside the address space.
     */
    a_end = a_ram_base + a_ram_size;
    b_end = b_ram_base + b_ram_size;
    if (a_end < a_ram_base || a_end == 0) {
        if (a_end != 0) {
            flags |= HYPE_VM_ISOLATION_UNCONFIGURED;
        }
        a_end = ~0ull;
    }
    if (b_end < b_ram_base || b_end == 0) {
        if (b_end != 0) {
            flags |= HYPE_VM_ISOLATION_UNCONFIGURED;
        }
        b_end = ~0ull;
    }
    if (a_ram_base < b_end && b_ram_base < a_end) {
        flags |= HYPE_VM_ISOLATION_RAM_OVERLAP;
    }

    return flags;
}

const char *hype_vm_isolation_describe(unsigned flags) {
    if (flags == HYPE_VM_ISOLATION_OK) {
        return "isolated";
    }
    /* Most severe first: a shared root means one address space regardless of
     * where the RAM sits, so it subsumes the overlap answer. */
    if (flags & HYPE_VM_ISOLATION_SAME_ROOT) {
        return "SHARED TRANSLATION ROOT -- guests share one address space";
    }
    if (flags & HYPE_VM_ISOLATION_RAM_OVERLAP) {
        return "RAM RANGES OVERLAP -- each guest can reach the other's memory";
    }
    return "UNCONFIGURED -- a base/size/root was zero, result not trustworthy";
}
