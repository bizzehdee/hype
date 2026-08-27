/*
 * #751/#749/#735: run test code on a guest AP, and prove the AP absorbs an unclaimed
 * MMIO access instead of spinning on it forever.
 *
 * WHY THIS EXISTS. Every other microtest runs on the guest BSP, so hype's AP dispatch loop
 * -- several hundred lines of exit handling in fw_1_run_ap_vcpu() -- had no unit-level
 * coverage at all. Its bugs were found on hardware, one boot at a time: #749 (an unclaimed
 * NPF re-executed forever, which is #735's root cause), #748, #484, #520, #641. Every one
 * of them was a path the BSP loop had and the AP loop did not -- #576's rule, and nothing
 * could catch a violation of it.
 *
 * WHAT IT DOES. Brings up vCPU 1 with INIT/SIPI through the guest LAPIC, into 32-bit
 * protected mode with a flat 4 GiB data segment -- not long mode, because the access only
 * needs a 32-bit linear address and every instruction of paging setup is another way for a
 * trampoline to triple-fault silently. The AP then reads an address no device claims,
 * twice, and leaves both the values and two progress markers in low memory.
 *
 * THE ASSERTION IS FORWARD PROGRESS. Pre-#749 the AP re-executes its first read forever:
 * the first marker never appears and this test times out. That is the pre-fix symptom.
 */
#include "micro.h"

#define NAME "apunclaimed"

/* Low-memory layout the trampoline agrees with. Below 0x10000, clear of the zero page
 * (0x7000) and of anything the kernel-boot path puts in guest RAM. */
#define TRAMP_GPA   0x8000u          /* SIPI vector 0x08 */
#define GDTR_GPA    0x9000u          /* 6-byte pseudo-descriptor */
#define GDT_GPA     0x9010u          /* null + 32-bit code + 32-bit data */
#define RESULT_GPA  0x9500u          /* [0]=read1 [1]=marker1 [2]=read2 [3]=marker2 */

#define MARK1 0x0000C0DEu
#define MARK2 0x0000600Du

#define LAPIC_BASE  0xFEE00000u
#define LAPIC_ICR_LO 0x300u
#define LAPIC_ICR_HI 0x310u

/*
 * The trampoline, assembled from the source below and pasted as bytes because the
 * microtest Makefile builds ONE .c plus the shared crt0.S per test, and a per-test .S
 * would be a build-system change for 78 bytes. `llvm-objdump -b binary -m i386` reproduces
 * it exactly from this array.
 *
 *      .code16
 *      cli; xor %ax,%ax; mov %ax,%ds
 *      lgdt 0x9000
 *      mov %cr0,%eax; or $1,%eax; mov %eax,%cr0
 *      ljmpl $0x08,$0x8020
 *      .org 0x20
 *      .code32
 *      mov $0x10,%ax; mov %ax,%ds; mov %ax,%es; mov %ax,%ss
 *      mov $0xD0000004,%ebx ; mov $0x9500,%edi
 *      mov (%ebx),%eax ; mov %eax,(%edi) ; movl $0xC0DE,4(%edi)
 *      mov (%ebx),%eax ; mov %eax,8(%edi) ; movl $0x600D,12(%edi)
 *   1: hlt ; jmp 1b
 *
 * TWO traps, both paid for with a run each:
 *
 * 1. `ljmpl $0x08,$0x8020` is ABSOLUTE. Writing `$pm32` assembles to linear 0x20, not
 *    0x8020 -- the AP left real mode, executed whatever was at 0x20, and triple-faulted at
 *    rip 0x303f. The trampoline is position-dependent and every absolute in it is linear.
 *
 * 2. The access uses ModRM addressing (`mov (%ebx),%eax`), not the moffs form
 *    (`mov 0xD0000004,%eax`, opcode A1). hype_mmio_decode() does not decode moffs, so the
 *    absorber could not retire it and the AP span 11,625,190 times -- the very failure
 *    this test exists to catch, reproduced by the test's own instruction encoding. ModRM
 *    is what compiled code emits and so is what this should exercise; the moffs gap is
 *    real and is filed separately.
 */
static const unsigned char g_tramp[] = {
    0xfa, 0x31, 0xc0, 0x8e, 0xd8, 0x0f, 0x01, 0x16, 0x00, 0x90, 0x0f, 0x20, 0xc0, 0x66, 0x83, 0xc8,
    0x01, 0x0f, 0x22, 0xc0, 0x66, 0xea, 0x20, 0x80, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x66, 0xb8, 0x10, 0x00, 0x8e, 0xd8, 0x8e, 0xc0, 0x8e, 0xd0, 0xbb, 0x04, 0x00, 0x00, 0xd0, 0xbf,
    0x00, 0x95, 0x00, 0x00, 0x8b, 0x03, 0x89, 0x07, 0xc7, 0x47, 0x04, 0xde, 0xc0, 0x00, 0x00, 0x8b,
    0x03, 0x89, 0x47, 0x08, 0xc7, 0x47, 0x0c, 0x0d, 0x60, 0x00, 0x00, 0xf4, 0xeb, 0xfd
};

static void put32(unsigned int gpa, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)gpa = v;
}
static uint32_t get32(unsigned int gpa) {
    return *(volatile uint32_t *)(uintptr_t)gpa;
}
static void lapic_write(unsigned int off, uint32_t v) {
    *(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + off) = v;
}
static void spin(unsigned long n) {
    volatile unsigned long i;
    for (i = 0; i < n; i++) { }
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    unsigned int i;
    uint32_t r1, r2, m1, m2;

    (void)zero_page_gpa;
    micro_puts("\nmicro/" NAME ": bringing up vCPU 1 to touch an unclaimed address\n");

    /* Results cleared first, so a stale value cannot be mistaken for a result. */
    put32(RESULT_GPA + 0, 0);
    put32(RESULT_GPA + 4, 0);
    put32(RESULT_GPA + 8, 0);
    put32(RESULT_GPA + 12, 0);

    for (i = 0; i < (unsigned int)sizeof(g_tramp); i++) {
        *(volatile unsigned char *)(uintptr_t)(TRAMP_GPA + i) = g_tramp[i];
    }

    /* Flat 32-bit code and data, base 0 limit 4 GiB. Selector 0x08 = code, 0x10 = data. */
    put32(GDT_GPA + 0, 0); put32(GDT_GPA + 4, 0);                 /* null */
    put32(GDT_GPA + 8, 0x0000FFFFu); put32(GDT_GPA + 12, 0x00CF9A00u);  /* code */
    put32(GDT_GPA + 16, 0x0000FFFFu); put32(GDT_GPA + 20, 0x00CF9200u); /* data */
    *(volatile uint16_t *)(uintptr_t)GDTR_GPA = (uint16_t)(24u - 1u);
    put32(GDTR_GPA + 2, GDT_GPA);

    /*
     * INIT then STARTUP to APIC ID 1. hype models both (SMP-4/#188) and holds an AP in
     * WAIT_SIPI until the STARTUP arrives -- so an AP that never runs means the IPI path is
     * broken, which is a different and equally worth-knowing failure.
     */
    lapic_write(LAPIC_ICR_HI, 1u << 24);
    lapic_write(LAPIC_ICR_LO, 0x00004500u);         /* INIT, assert */
    spin(200000);
    lapic_write(LAPIC_ICR_HI, 1u << 24);
    lapic_write(LAPIC_ICR_LO, 0x00004600u | (TRAMP_GPA >> 12)); /* STARTUP, vector 0x08 */

    /*
     * Bounded wait. Pre-#749 the AP spins on its first read and the markers never appear,
     * so this expires -- which is the reproduction, and is reported as such rather than as
     * a generic timeout.
     */
    for (i = 0; i < 200u; i++) {
        if (get32(RESULT_GPA + 12) == MARK2) {
            break;
        }
        spin(100000);
    }
    micro_puts("micro/" NAME ": waited "); micro_put_hex(i); micro_puts(" rounds\n");

    r1 = get32(RESULT_GPA + 0);
    m1 = get32(RESULT_GPA + 4);
    r2 = get32(RESULT_GPA + 8);
    m2 = get32(RESULT_GPA + 12);

    micro_puts("micro/" NAME ": read1=0x"); micro_put_hex(r1);
    micro_puts(" mark1=0x"); micro_put_hex(m1);
    micro_puts(" read2=0x"); micro_put_hex(r2);
    micro_puts(" mark2=0x"); micro_put_hex(m2);
    micro_puts("\n");

    if (m1 != MARK1) {
        /* The AP never got past its FIRST unclaimed read. That is #735 exactly. */
        micro_fail(NAME, "the AP never completed one unclaimed access -- it is spinning on it");
        micro_halt();
    }
    if (r1 != 0xFFFFFFFFu) {
        micro_fail(NAME, "the AP's unclaimed read did not return all-ones");
        micro_halt();
    }
    if (m2 != MARK2 || r2 != 0xFFFFFFFFu) {
        micro_fail(NAME, "the AP completed one unclaimed access but not a second");
        micro_halt();
    }

    micro_puts("micro/" NAME ": vCPU 1 absorbed two unclaimed accesses and kept running\n");
    micro_pass(NAME);
    micro_halt();
}
