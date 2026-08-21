/*
 * #596: a guest that does nothing but produce a LOT of serial output, so hype's real log writer
 * (core/log_sink.c via split_log_setup) has a large per-VM log to grow. Run several of these at
 * once and hype grows \HYPE.LOG + one VMn.LOG per guest CONCURRENTLY on the boot volume's shared
 * fs -- the exact path #596 appeared on, which a single quiet guest never exercises.
 *
 * Prints `lines=<N>` (cmdline; default 2000) records to ttyS0, then MICRO PASS and halts. Each
 * record is ~70 bytes, so N=2000 grows this VM's log by ~140 KiB of real, classified guest output.
 */
#include "micro.h"

#define NAME "logchatter"

void micro_main(uint64_t zero_page_gpa);

static unsigned int parse_uint(const char *s) {
    unsigned int v = 0u;
    if (s == 0) return 0u;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned int)(*s - '0'); s++; }
    return v;
}

void micro_main(uint64_t zero_page_gpa) {
    const char *cl = micro_cmdline(zero_page_gpa);
    unsigned int lines = cl ? parse_uint(micro_cmdline_value(cl, "lines")) : 0u;
    unsigned int i;

    if (lines == 0u) lines = 2000u;

    for (i = 0; i < lines; i++) {
        /* A distinct, non-trivial line each time -- real guest chatter, not a repeated constant,
         * so the per-VM log grows with varied content the log writer must place on the medium. */
        micro_puts("micro/" NAME ": chatter line ");
        micro_put_uint(i);
        micro_puts("/");
        micro_put_uint(lines);
        micro_puts(" payload ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 tail ");
        micro_put_hex((uint64_t)i * 2654435761ull);
        micro_puts("\n");
    }

    micro_puts("micro/" NAME ": emitted ");
    micro_put_uint(lines);
    micro_puts(" lines\n");
    micro_pass(NAME);
    micro_halt();
}
