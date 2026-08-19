/*
 * #539: CPUMSR, ported out of boot/main.c.
 *
 * The in-binary test's CPUID oracle was host-side and cannot move: hype re-ran the real `cpuid`
 * instruction, fed it through hype_cpuid_emulate_ex(), and compared byte-for-byte with what the
 * guest saw. A guest cannot know the host's raw CPUID, and faking that oracle would be worse than
 * not having it -- so what moves here is the set of guest-visible INVARIANTS, which is what a real
 * guest actually depends on and where every bug in this area has actually bitten.
 *
 * Each one below exists because getting it wrong broke a real guest:
 *
 *  - X2APIC must read as ABSENT. Advertised, Linux switches APIC access to the x2APIC MSR
 *    interface, which hype does not model (it is MMIO-LAPIC-only), the timer calibration reads
 *    garbage, and the guest idle-hangs.
 *  - MONITOR must read as ABSENT (#256). Advertised, Linux picks MWAIT for its idle loop, hype
 *    intercepts neither instruction, and the guest NEVER executes HLT -- so hype loses its only
 *    idle signal and the dashboard shows 100% CPU for an idle guest.
 *  - TSC_DEADLINE must read as ABSENT. hype has no MSR-armed LAPIC timer.
 *  - SVM/VMX must read as ABSENT, and EFER.SVME must read as CLEAR (#316). hype does not expose
 *    virtualization to guests, and a guest seeing SVME set while its CPUID reports no SVM is
 *    self-contradictory -- OpenBSD reads EFER back and checks it.
 *  - HYPERVISOR-PRESENT must read as SET, which is how a guest knows to look for the paravirtual
 *    leaves at all.
 *
 * Asserting these from the guest is the point: they are all statements about what a guest sees, and
 * the host asserting them was checking its own arithmetic rather than the guest's view.
 *
 * cmdline (#546): `hv=require` demands the Hyper-V hypercall page work, for a VM configured with
 * os_hint = windows. Without it the Hyper-V arm is skipped and SAID to be skipped -- a silently
 * skipped arm is a test that passes for the wrong reason.
 */
#include "micro.h"

#define NAME "cpumsr"

#define MSR_IA32_APIC_BASE 0x1Bu
#define MSR_EFER 0xC0000080u
#define MSR_HV_GUEST_OS_ID 0x40000000u
#define MSR_HV_HYPERCALL 0x40000001u

#define LEAF1_ECX_MONITOR (1u << 3)
#define LEAF1_ECX_VMX (1u << 5)
#define LEAF1_ECX_X2APIC (1u << 21)
#define LEAF1_ECX_TSC_DEADLINE (1u << 24)
#define LEAF1_ECX_HYPERVISOR (1u << 31)
#define LEAF8000_0001_ECX_SVM (1u << 2)

#define EFER_SVME (1ull << 12)
#define EFER_LME (1ull << 8)
#define EFER_LMA (1ull << 10)

#define APIC_BASE_ADDR 0xFEE00000ull
#define APIC_BASE_ENABLE (1ull << 11)
#define APIC_BASE_BSP (1ull << 8)

#define HV_STATUS_INVALID_HYPERCALL_CODE 0x0002ull
#define HV_HYPERCALL_ENABLE 0x1ull

typedef struct {
    uint32_t eax, ebx, ecx, edx;
} cpuid_t;

static inline cpuid_t do_cpuid(uint32_t leaf, uint32_t sub) {
    cpuid_t r;
    __asm__ volatile("cpuid"
                     : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(sub));
    return r;
}

static inline uint64_t do_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void do_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void put_bit(const char *what, int set) {
    micro_puts("micro/" NAME ": ");
    micro_puts(what);
    micro_puts(set ? " = present\n" : " = absent\n");
}

/* One place that reports a bit that is the wrong way round, with the reason it matters. */
static int require_bit(const char *what, int actual, int wanted, const char *why) {
    if (actual == wanted) {
        return 1;
    }
    micro_puts("micro/" NAME ": ");
    micro_puts(what);
    micro_puts(wanted ? " must be PRESENT and is absent -- " : " must be ABSENT and is present -- ");
    micro_puts(why);
    micro_puts("\n");
    return 0;
}

/* The 4-byte page stub, which differs by vendor: VMCALL on Intel, VMMCALL on AMD. The guest works
 * out which to expect from CPUID leaf 0's vendor string rather than being told, so this covers
 * hype's vendor dispatch as well as the page write. */
static int vendor_is_amd(void) {
    cpuid_t v = do_cpuid(0u, 0u);
    /* "AuthenticAMD" -> ebx "Auth", edx "enti", ecx "cAMD" */
    return (v.ebx == 0x68747541u && v.edx == 0x69746E65u && v.ecx == 0x444D4163u) ? 1 : 0;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cl = micro_cmdline(zero_page_gpa);
    int hv_required = (cl != 0 && micro_cmdline_value(cl, "hv") != 0);
    cpuid_t l0, l1, l8;
    uint64_t apic_base, efer;
    int ok = 1;

    micro_puts("\n");

    l0 = do_cpuid(0u, 0u);
    l1 = do_cpuid(1u, 0u);
    l8 = do_cpuid(0x80000001u, 0u);

    micro_puts("micro/" NAME ": vendor ");
    {
        char v[13];
        unsigned i;
        for (i = 0; i < 4; i++) v[i] = (char)((l0.ebx >> (i * 8)) & 0xFFu);
        for (i = 0; i < 4; i++) v[4 + i] = (char)((l0.edx >> (i * 8)) & 0xFFu);
        for (i = 0; i < 4; i++) v[8 + i] = (char)((l0.ecx >> (i * 8)) & 0xFFu);
        v[12] = '\0';
        micro_puts(v);
        micro_puts(", max basic leaf ");
        micro_put_hex(l0.eax);
        micro_puts("\n");
    }

    put_bit("hypervisor-present", (l1.ecx & LEAF1_ECX_HYPERVISOR) ? 1 : 0);
    put_bit("x2apic", (l1.ecx & LEAF1_ECX_X2APIC) ? 1 : 0);
    put_bit("monitor/mwait", (l1.ecx & LEAF1_ECX_MONITOR) ? 1 : 0);
    put_bit("tsc-deadline", (l1.ecx & LEAF1_ECX_TSC_DEADLINE) ? 1 : 0);
    put_bit("vmx", (l1.ecx & LEAF1_ECX_VMX) ? 1 : 0);
    put_bit("svm", (l8.ecx & LEAF8000_0001_ECX_SVM) ? 1 : 0);

    ok &= require_bit("hypervisor-present", (l1.ecx & LEAF1_ECX_HYPERVISOR) ? 1 : 0, 1,
                      "a guest cannot find the paravirtual leaves without it");
    ok &= require_bit("x2apic", (l1.ecx & LEAF1_ECX_X2APIC) ? 1 : 0, 0,
                      "hype models only the MMIO xAPIC; in x2APIC mode the timer reads garbage");
    ok &= require_bit("monitor/mwait", (l1.ecx & LEAF1_ECX_MONITOR) ? 1 : 0, 0,
                      "#256: a guest that idles on MWAIT never HLTs, so hype loses its idle signal");
    ok &= require_bit("tsc-deadline", (l1.ecx & LEAF1_ECX_TSC_DEADLINE) ? 1 : 0, 0,
                      "hype has no MSR-armed LAPIC timer");
    ok &= require_bit("vmx", (l1.ecx & LEAF1_ECX_VMX) ? 1 : 0, 0,
                      "hype does not expose virtualization to guests");
    ok &= require_bit("svm", (l8.ecx & LEAF8000_0001_ECX_SVM) ? 1 : 0, 0,
                      "hype does not expose virtualization to guests");

    /* The paravirtual leaf block. hype places KVM at 0x40000000, or at 0x40000100 when Hyper-V
     * holds the lower block -- the same relocation QEMU performs. Either is correct; nothing at
     * all is not, because hypervisor-present said to look. */
    {
        cpuid_t pv = do_cpuid(0x40000000u, 0u);
        cpuid_t pv2 = do_cpuid(0x40000100u, 0u);
        int have = (pv.ebx != 0u || pv2.ebx != 0u) ? 1 : 0;

        micro_puts("micro/" NAME ": paravirtual leaf 0x40000000 ebx=");
        micro_put_hex(pv.ebx);
        micro_puts(" 0x40000100 ebx=");
        micro_put_hex(pv2.ebx);
        micro_puts("\n");
        ok &= require_bit("a paravirtual signature", have, 1,
                          "hypervisor-present is set, so a guest looks here and must find one");
    }

    apic_base = do_rdmsr(MSR_IA32_APIC_BASE);
    micro_puts("micro/" NAME ": APIC_BASE=");
    micro_put_hex(apic_base);
    micro_puts("\n");
    if ((apic_base & 0xFFFFFF000ull) != APIC_BASE_ADDR) {
        micro_fail(NAME, "APIC_BASE does not name 0xFEE00000, which is the only LAPIC hype models");
        micro_halt();
    }
    if ((apic_base & APIC_BASE_ENABLE) == 0ull) {
        micro_fail(NAME, "APIC_BASE has the global-enable bit clear");
        micro_halt();
    }
    /* vCPU 0 is this VM's BSP, and this kernel runs on it. */
    if ((apic_base & APIC_BASE_BSP) == 0ull) {
        micro_fail(NAME, "APIC_BASE does not mark this vCPU as the BSP");
        micro_halt();
    }

    efer = do_rdmsr(MSR_EFER);
    micro_puts("micro/" NAME ": EFER=");
    micro_put_hex(efer);
    micro_puts("\n");
    if ((efer & EFER_LMA) == 0ull || (efer & EFER_LME) == 0ull) {
        micro_fail(NAME, "EFER does not report long mode, in a guest that is executing 64-bit code");
        micro_halt();
    }
    if ((efer & EFER_SVME) != 0ull) {
        /* #316. The in-binary test asserted the OPPOSITE until #316 inverted it: hype must force
         * SVME on in the VMCB (a VMRUN consistency check) and MASK IT OUT of what the guest reads.
         * Keeping the inverted assertion makes this a standing guard, so a regression that leaks
         * SVME back fails here rather than in a BSD install months later. */
        micro_fail(NAME, "EFER leaks SVME to the guest -- #316 requires it be masked on read");
        micro_halt();
    }

    /*
     * The Hyper-V hypercall page. Only meaningful when this VM was configured os_hint = windows,
     * which is what turns the leaves on -- so the arm is conditional, and says which way it went.
     * A skipped arm that did not announce itself is a test passing for the wrong reason.
     */
    {
        /*
         * Detect Hyper-V by ITS OWN signature -- ebx/ecx/edx spelling "Microsoft Hv" -- not by
         * "leaf 0x40000000 has a nonzero ebx". KVM puts its own signature at the same leaf when
         * Hyper-V is off, so the looser test matched KVM and then failed looking for a Hyper-V page
         * that was never meant to exist. Caught by running it.
         */
        cpuid_t hv = do_cpuid(0x40000000u, 0u);
        int hv_present = (hv.ebx == 0x7263694Du && hv.ecx == 0x666F736Fu && hv.edx == 0x76482074u)
                             ? 1
                             : 0;

        if (!hv_present) {
            if (hv_required) {
                micro_fail(NAME, "cmdline said hv=require but no Hyper-V leaves are present -- "
                                 "this VM needs os_hint = windows");
                micro_halt();
            }
            micro_puts("micro/" NAME ": Hyper-V arm SKIPPED -- no Hyper-V leaves (this VM is not "
                       "os_hint = windows). Pass hv=require on the cmdline to demand it.\n");
        } else {
            /*
             * Point the hypercall MSR at a page of our own RAM and check hype wrote the right
             * 4-byte stub into it, then make an unknown hypercall and check the status.
             */
            uint64_t page = 0x200000ull; /* 2 MB: inside RAM, clear of the payload at 16 MB */
            const volatile uint8_t *stub = (const volatile uint8_t *)(uintptr_t)page;
            uint8_t want2 = vendor_is_amd() ? 0xD9u : 0xC1u;
            uint64_t status;

            /*
             * A NONZERO GUEST OS ID FIRST. The TLFS requires it before Enable may be set, and hype
             * implements that rule -- so a test that skips this reads a page of zeros and blames
             * the hypervisor. That is what the first run of this port did, which is worth keeping
             * as a comment: the requirement is real, easy to miss, and its symptom (an all-zero
             * stub) looks exactly like hype having failed to write one.
             */
            do_wrmsr(MSR_HV_GUEST_OS_ID, 0x1ull << 48); /* any nonzero value identifies "an OS" */
            do_wrmsr(MSR_HV_HYPERCALL, page | HV_HYPERCALL_ENABLE);

            /* Read the MSR back: Enable must have stuck, or the page write did not happen and the
             * stub check below would report the wrong cause. */
            {
                uint64_t back = do_rdmsr(MSR_HV_HYPERCALL);
                micro_puts("micro/" NAME ": hypercall MSR reads back ");
                micro_put_hex(back);
                micro_puts("\n");
                if ((back & HV_HYPERCALL_ENABLE) == 0ull) {
                    micro_fail(NAME, "the hypercall page did not enable -- Guest OS ID was set, so "
                                     "Enable should have stuck");
                    micro_halt();
                }
            }

            micro_puts("micro/" NAME ": hypercall stub ");
            micro_put_hex(((uint64_t)stub[0] << 24) | ((uint64_t)stub[1] << 16) |
                          ((uint64_t)stub[2] << 8) | (uint64_t)stub[3]);
            micro_puts(vendor_is_amd() ? " (expecting VMMCALL)\n" : " (expecting VMCALL)\n");

            if (stub[0] != 0x0Fu || stub[1] != 0x01u || stub[2] != want2 || stub[3] != 0xC3u) {
                micro_fail(NAME, "the Hyper-V page does not hold this vendor's hypercall stub");
                micro_halt();
            }

            /*
             * Call it with a code nothing implements. RCX is the input value: bits 15:0 the call
             * code, 16 the fast flag, 26:17 the rep count. RDX and R8 are the operand GPAs.
             *
             * The RETURN is not a bare status: the TLFS defines bits 15:0 as the result and 31:16
             * as reps completed, so a caller masks. hype returns 0x0002 with reps 0 -- measured,
             * after two wrong readings that were entirely this test's fault (see the register note
             * below). Nothing above bit 31 is defined, so a bit set there would mean hype is
             * returning a field that does not exist.
             */
            /*
             * The call target goes in RBX explicitly, and RAX is ZEROED before the call.
             *
             * Both matter. The first version wrote `callq *%1` with an "r" input holding &page and
             * "=a" as the output, and the compiler chose RAX for that input -- so the value read
             * back was the pointer, not the status, and it CHANGED between builds (0x00010002, then
             * 0x0007ff90, which is a stack address). Two different wrong answers from the same
             * hypervisor is the signature of the test reading the wrong register, not of the
             * hypervisor being inconsistent -- and zeroing RAX first makes "hype wrote nothing"
             * report as 0 instead of as noise.
             */
            __asm__ volatile("movq $0xBEEF, %%rcx\n\t"
                             "xorq %%rdx, %%rdx\n\t"
                             "xorq %%r8, %%r8\n\t"
                             "xorq %%rax, %%rax\n\t"
                             "callq *%%rbx\n\t"
                             : "=a"(status)
                             : "b"(page)
                             : "rcx", "rdx", "r8", "r9", "r10", "r11", "memory");
            micro_puts("micro/" NAME ": unknown hypercall returned ");
            micro_put_hex(status);
            micro_puts(" (result=");
            micro_put_hex(status & 0xFFFFull);
            micro_puts(" reps=");
            micro_put_uint((status >> 16) & 0xFFFFull);
            micro_puts(")\n");
            if ((status & 0xFFFFull) != HV_STATUS_INVALID_HYPERCALL_CODE) {
                micro_fail(NAME, "an unknown hypercall did not return HV_STATUS_INVALID_HYPERCALL_CODE");
                micro_halt();
            }
            /* Nothing above bit 31 is defined for this output; a set bit there would mean hype is
             * returning a field the TLFS does not define. */
            if ((status >> 32) != 0ull) {
                micro_fail(NAME, "the hypercall result has bits set above 31, which are undefined");
                micro_halt();
            }
        }
    }

    if (!ok) {
        micro_fail(NAME, "one or more CPUID feature bits are the wrong way round -- see the lines above");
        micro_halt();
    }

    micro_pass(NAME);
    micro_halt();
}
