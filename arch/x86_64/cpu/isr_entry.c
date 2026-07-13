#include "isr.h"
#include "../../../core/panic.h"
#include "../../../core/sys_table.h"

/*
 * Called by isr_stubs.S for every one of the 256 vectors. Not unit
 * tested: it ends in the noreturn hype_panic(), so calling it in a test
 * would hang the test binary rather than verify anything -- same
 * reasoning as hype_panic() itself (see halt.c). All the actual
 * decision logic (what to say) lives in the tested
 * hype_isr_format_message() in isr_decode.c; this is deliberately just
 * the wiring.
 */
void hype_isr_dispatch(const hype_isr_frame_t *frame) {
    char msg[192];

    hype_isr_format_message(msg, sizeof(msg), frame);
    hype_panic(hype_sys_table_get(), msg);
}
