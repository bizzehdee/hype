/*
 * #749/#735: an access to an address NO device claims must complete, not wedge the vCPU.
 *
 * This is the microtest for the fault that stopped #735's reboot. On the 5950X, boot 6,
 * vCPU 1 took 37,009,095 nested page faults at one RIP -- about 350,000 a second, from
 * t=170s to the end of the run -- because hype counted an unmatched NPF and re-entered the
 * guest with RIP unchanged. The guest re-executed the same instruction forever. The
 * operator's `reboot`, pinned to that very vCPU, could never run.
 *
 * Only eight of those 37 million were ever printed: the log line was capped per vCPU, so
 * the log showed a handful and nothing after, and the fault read as a transient that had
 * stopped. That is why this test asserts on a COUNT and on forward progress rather than on
 * the absence of a message.
 *
 * What a real machine does with an unclaimed access is not "hang": the read returns
 * all-ones and the write is dropped, because nothing on the bus drove the data lines low
 * and nothing latched the write. That is the behaviour asserted here.
 */
#include "micro.h"

#define NAME "unclaimed"

/*
 * 0xD0000004 -- above this guest's RAM, below the ECAM window at 0xE0000000, below the
 * IO-APIC at 0xFEC00000 and below the flash at the top of the 32-bit range. Nothing hype
 * models claims it.
 *
 * NOT 0x100000004, which is the address the 5950X's guest actually wedged on. A microtest
 * kernel identity-maps the first 4 GiB and no more, so 4 GiB + 4 faults in the GUEST's own
 * page tables and triple-faults before hype ever sees a nested page fault -- measured, on
 * the first attempt at this test. The real guest maps all of physical memory and had no
 * such limit. What matters here is "an address inside the guest's own map that no device
 * claims", and this is one.
 */
#define UNCLAIMED_GPA 0xD0000004ull

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)UNCLAIMED_GPA;
    unsigned int i;
    uint32_t v;

    (void)zero_page_gpa;
    micro_puts("\nmicro/" NAME ": reading an address no device claims\n");

    /*
     * ONE read first. If hype has not been fixed this never returns -- the vCPU spins on
     * this instruction and the harness reports a timeout, which is the pre-fix symptom and
     * is a perfectly good failure.
     */
    v = *p;
    micro_puts("micro/" NAME ": first read returned 0x");
    micro_put_hex(v);
    micro_puts("\n");
    if (v != 0xFFFFFFFFu) {
        micro_fail(NAME, "an unclaimed read should return all-ones");
        micro_halt();
    }

    /* A write must be dropped rather than faulting or wedging. */
    *p = 0x5A5A5A5Au;
    v = *p;
    if (v != 0xFFFFFFFFu) {
        micro_fail(NAME, "a dropped write should not change what the address reads back");
        micro_halt();
    }

    /*
     * And FORWARD PROGRESS, which is the whole point: a thousand of them in a row. The
     * pre-fix behaviour would not reach the second iteration, let alone the thousandth.
     * A thousand is enough to distinguish "completes" from "completes sometimes" while
     * staying instant on a working build.
     */
    for (i = 0; i < 1000u; i++) {
        v = *p;
        if (v != 0xFFFFFFFFu) {
            micro_fail(NAME, "an unclaimed read stopped returning all-ones partway through");
            micro_halt();
        }
        *p = i;
    }
    micro_puts("micro/" NAME ": 1000 unclaimed accesses completed, execution continued\n");

    /* Byte and word widths take different decoder paths; an all-ones read must truncate. */
    {
        volatile uint8_t *b = (volatile uint8_t *)(uintptr_t)UNCLAIMED_GPA;
        volatile uint16_t *w = (volatile uint16_t *)(uintptr_t)UNCLAIMED_GPA;
        if (*b != 0xFFu) {
            micro_fail(NAME, "an unclaimed byte read should return 0xFF");
            micro_halt();
        }
        if (*w != 0xFFFFu) {
            micro_fail(NAME, "an unclaimed word read should return 0xFFFF");
            micro_halt();
        }
    }

    micro_pass(NAME);
    micro_halt();
}
