/*
 * SMP-14 (#470): a guest that tries to keep its core forever.
 *
 * It reports its verdict FIRST, then masks interrupts and spins in `cli; 1: jmp 1b` for the rest
 * of the run -- no exit-causing instruction, no interrupt window, no PAUSE, no HLT. Its PASS only
 * says "the hog is now spinning"; the real assertion is on its co-tenant (tests/micro/sharemem.c
 * on the same shared core), which must still finish all of its rounds, and on hype's own SCHED
 * lines, which must show the hog being preempted every slice with a bounded overrun.
 *
 * A guest able to defer preemption this way would void plan.md §6g on the shared tier.
 *
 *   cmdline: [mode=cli|sti|pause|movss|rep|hlt|mwait]
 *     cli    interrupts masked, tight loop (the default)
 *     sti    interrupts unmasked, tight loop
 *     pause  a PAUSE loop, which SVM's pause filter may see
 *     movss  back-to-back MOV SS, each opening an interrupt shadow for the next instruction
 *     rep    one long `rep movsb` (the first 32 MiB onto itself), a single long-running instruction
 *     hlt    `cli; hlt` forever -- a halt nothing can wake
 *     mwait  MONITOR/MWAIT with interrupts masked (not advertised in CPUID; tried anyway)
 */
#include "micro.h"

#define NAME "cpuhog"

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *mode = micro_cmdline_value(micro_cmdline(zero_page_gpa), "mode");
    char m = (mode == 0) ? 'c' : mode[0];
    /* Two modes share a first letter with another: tell them apart by the second. */
    if (mode != 0 && mode[0] == 'm') m = (mode[1] == 'o') ? 'o' : 'w';
    if (mode != 0 && mode[0] == 'r') m = 'r';
    if (mode != 0 && mode[0] == 'h') m = 'h';

    micro_puts("\n");
    if (m != 'c' && m != 's' && m != 'p' && m != 'o' && m != 'r' && m != 'h' && m != 'w') {
        micro_fail(NAME, "mode= must be cli, sti, pause, movss, rep, hlt or mwait");
        micro_halt();
    }
    micro_puts("micro/" NAME ": spinning forever, mode ");
    micro_puts(m == 'c' ? "cli" : m == 's' ? "sti" : m == 'p' ? "pause" : m == 'o' ? "movss"
               : m == 'r' ? "rep" : m == 'h' ? "hlt" : "mwait");
    micro_puts(" -- the co-tenant must still make progress\n");
    micro_pass(NAME);
    if (m == 'c') {
        __asm__ volatile("cli\n1: jmp 1b" ::: "memory");
    } else if (m == 's') {
        __asm__ volatile("sti\n1: jmp 1b" ::: "memory");
    } else if (m == 'p') {
        __asm__ volatile("cli\n1: pause\n jmp 1b" ::: "memory");
    } else if (m == 'o') {
        __asm__ volatile("cli\n mov %%ss, %%ax\n1: mov %%ax, %%ss\n mov %%ax, %%ss\n jmp 1b"
                         ::: "rax", "memory");
    } else if (m == 'r') {
        /* The first 32 MiB of RAM onto itself (give the VM >= 64 mem_mb): same bytes, one
         * instruction that runs for milliseconds. */
        __asm__ volatile("cli\n1: xor %%esi, %%esi\n xor %%edi, %%edi\n mov $0x2000000, %%ecx\n"
                         " rep movsb\n jmp 1b" ::: "rsi", "rdi", "rcx", "memory");
    } else if (m == 'h') {
        __asm__ volatile("cli\n1: hlt\n jmp 1b" ::: "memory");
    } else {
        __asm__ volatile("cli\n1: xor %%ecx, %%ecx\n xor %%edx, %%edx\n mov $0x100000, %%eax\n"
                         " monitor\n xor %%eax, %%eax\n mwait\n jmp 1b" ::: "rax", "rcx", "rdx",
                         "memory");
    }
    micro_halt();
}
