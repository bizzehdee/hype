#include "isr.h"
#include "../../../core/fatal.h"

/*
 * Called by isr_stubs.S for every one of the 256 vectors. Not unit
 * tested: it ends in the noreturn hype_fatal(), so calling it in a test
 * would hang the test binary rather than verify anything -- same
 * reasoning as hype_fatal() itself (see halt.c). All the actual
 * decision logic (what to say) lives in the tested
 * hype_isr_format_message() in isr_decode.c; this is deliberately just
 * the wiring.
 *
 * Goes through hype_fatal() (M1-7's serial+GOP panic handler), never
 * ConOut: boot/main.c only installs this IDT after a successful
 * ExitBootServices(), so this function only ever runs with Boot
 * Services already gone. ConOut is unreliable at that point (its
 * implementation is commonly built on Boot-Services-dependent
 * machinery internally) -- calling it from here previously caused a
 * *second* fault inside this fault handler on a real crash, which is a
 * double fault, and a fault inside *that* handler is an unrecoverable
 * triple fault (confirmed via QEMU's -d int trace while validating
 * M1-5: `check_exception old: 0x8 new 0xd`, then a full VM reset
 * instead of a clean panic message). Serial has no such dependency, at
 * any point in the boot sequence.
 */
void hype_isr_dispatch(const hype_isr_frame_t *frame) {
    char msg[192];

    hype_isr_format_message(msg, sizeof(msg), frame);
    hype_fatal("%s", msg);
}
