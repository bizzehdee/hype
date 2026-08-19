/*
 * #535: the smallest possible kernel-boot guest -- proof that a `[vm.*]` section with
 * `boot = kernel` reaches a 64-bit entry point with a working device model behind it.
 *
 * It is not a microtest port (that is #536). What it validates is the LOAD PATH: config parse,
 * admission, the RAM carve, the image read through hype's own FS stack, the guest-resident page
 * tables, the long-mode entry, and the serial relay into this VM's log. Every one of those has to
 * work before a single in-binary test can move out, so it is worth having its own artifact that
 * fails for exactly one reason.
 */
#include "micro.h"

#define NAME "hello"

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    /*
     * The zero page is the one thing the boot protocol promises in a register, so check it before
     * anything else: a kernel that cannot trust RSI cannot trust its memory map either. hype puts
     * it at HYPE_KBOOT_ZERO_PAGE_GPA (0x7000) and the e820 entry it builds says how much RAM this
     * VM really has -- which is a configured value (mem_mb), so reporting it proves the config
     * reached the guest and not just the log.
     */
    const unsigned char *zp = (const unsigned char *)(uintptr_t)zero_page_gpa;
    unsigned int e820_entries;
    uint64_t ram_bytes;

    micro_puts("\n");
    micro_puts("micro/" NAME ": entered long mode, rsi=");
    micro_put_hex(zero_page_gpa);
    micro_puts("\n");

    if (zero_page_gpa == 0ull) {
        micro_fail(NAME, "RSI was zero -- no zero page was passed");
        micro_halt();
    }

    /* boot_params.e820_entries is at offset 0x1E8, and the table at 0x2D0 in 20-byte entries of
     * {addr, size, type} -- the layout core/linux_boot.h transcribes from the kernel's own
     * bootparam.h. Read here rather than trusted, because a wrong offset on either side is a
     * silent disagreement about memory. */
    e820_entries = zp[0x1E8];
    if (e820_entries == 0u) {
        micro_fail(NAME, "the zero page carries no e820 entries");
        micro_halt();
    }
    ram_bytes = *(const uint64_t *)(uintptr_t)(zero_page_gpa + 0x2D0 + 8);

    /* #546: say what command line, if any, the config asked for -- and distinguish "none" from
     * "empty", because they are different configs and only one of them is a mistake. */
    {
        const char *cl = micro_cmdline(zero_page_gpa);
        micro_puts("micro/" NAME ": cmdline ");
        if (cl == 0) {
            micro_puts("(none -- cmd_line_ptr was 0)\n");
        } else if (cl[0] == '\0') {
            micro_puts("(empty -- a valid pointer to an empty string)\n");
        } else {
            micro_puts("'");
            micro_puts(cl);
            micro_puts("'\n");
        }
    }

    micro_puts("micro/" NAME ": e820 entries=");
    micro_put_uint(e820_entries);
    micro_puts(", usable RAM=");
    micro_put_uint(ram_bytes / (1024ull * 1024ull));
    micro_puts(" MiB\n");

    /*
     * Touch the far end of the RAM the map claims. It is the cheapest check that the identity map
     * and the nested tables agree with the e820: if guest RAM is short of what the map says, this
     * faults instead of returning a wrong answer, and a fault here is a located one.
     */
    if (ram_bytes > (2ull * 1024ull * 1024ull)) {
        volatile uint64_t *tail = (volatile uint64_t *)(uintptr_t)(ram_bytes - 4096ull);
        *tail = 0x5555AAAA5555AAAAull;
        if (*tail != 0x5555AAAA5555AAAAull) {
            micro_fail(NAME, "the last page of reported RAM did not read back what was written");
            micro_halt();
        }
    }

    micro_pass(NAME);
    micro_halt();
}
