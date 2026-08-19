/*
 * #535: entry stub for a hype micro-kernel.
 *
 * hype's `boot = kernel` loader (plan.md §10 decision 45) enters here in 64-bit long mode with
 * the guest's own identity page tables already in CR3, RSP at the top of the low stack, and RSI
 * holding the zero page's guest-physical address -- the Linux/x86_64 boot protocol's one register
 * contract. Nothing else is assumed: no firmware has run, no GDT of our own is loaded, and the
 * segment registers are the flat ones the loader built.
 *
 * The 0x200-byte .head section exists because the 64-bit entry point is defined as the payload's
 * load address PLUS 0x200. It is not padding for its own sake: it is the offset that makes
 * hype_linux_64bit_entry() land exactly on _start.
 */
    .section .head, "a"
    .ascii "HYPEMICRO"
    /*
     * INT3, not zero padding. Zero padding is not inert: 0x00 0x00 decodes as `add [rax], al`, so
     * a guest entered ANYWHERE below the payload slides through zeroed RAM, through this region,
     * and into _start -- and then produces a completely correct-looking PASS from an entry point
     * that was never used. That happened on this loader's first run (#535), and the log line
     * reporting the intended RIP made it look right. 0xCC stops the slide with a #BP the guest's
     * empty IDT cannot handle, which hype reports as a shutdown: loud, and impossible to mistake
     * for a pass.
     */
    .fill 0x200 - 9, 1, 0xCC

    .section .text
    .globl _start
_start:
    cld
    /* RSI = zero page -> micro_main's first argument. */
    movq %rsi, %rdi
    /* A 16-byte-aligned stack, as the SysV ABI requires of a call site. The loader set RSP to a
     * page-aligned top, so this only ever removes the return address's own skew. */
    andq $-16, %rsp
    call micro_main
    /* micro_main returning is not expected -- it is a test kernel that reports and stops. Halt
     * rather than fall into whatever follows: an unexpected return that executed on is how a
     * clean FAIL turns into an unexplained exception. */
.Lhalt:
    hlt
    jmp .Lhalt
