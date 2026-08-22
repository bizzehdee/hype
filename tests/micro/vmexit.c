/*
 * #603: systematic VM-exit coverage -- one guest that deliberately triggers every exit reason
 * hype's dispatch table (arch/x86_64/svm/vmcb.c + svm_vcpu.c, arch/x86_64/vmx/vmcs_hw.c,
 * boot/main.c's vmm_reason_is_*() classifiers) actually intercepts, and checks the architectural
 * result from inside the guest.
 *
 * WHY A NEW FILE RATHER THAN EXTENDING AN EXISTING ONE. Several exit reasons already have a
 * dedicated, deeper microtest (HLT in intdeliver.c, CPUID/MSR invariants in cpumsr.c, PAUSE in
 * pausespin.c). Duplicating them here would be the same coverage twice with two chances to drift
 * apart. This file re-proves each of those ONLY far enough to confirm the exit is still taken (a
 * couple of lines), and gives its own real weight to the reasons NOTHING else exercises: the
 * AMD-privileged-instruction #UD group, the unmodelled-MMIO absorb path, MONITOR/MWAIT, and
 * INIT/SIPI from the guest's own instruction stream. The coverage table below says which is which.
 *
 * WHY THIS IS NOT A SUITE MEMBER (tests/micro/suite.c). Like faulter.c, this guest ends itself on
 * purpose -- first via the watchdog-storm probe's sibling file (vmexitstorm.c) forcing a VM off,
 * and this file's own last probe, a deliberate triple fault. A suite member that dies takes every
 * test scheduled after it down too, so this stays a standalone artifact with its own config
 * (tests/micro/suite-603.cfg), exactly like tests/micro/suite-538.cfg pairs faulter with hello.
 *
 * HARD SAFETY RULE THIS FILE OBEYS. VMRUN/VMLOAD/VMSAVE/STGI/CLGI/SKINIT/INVLPGA are guest
 * ring-0 instructions that, if hype's interception were ever missing, would actually run --
 * VMRUN into a nested context, STGI/CLGI toggling GIF, SKINIT resetting the platform. Every one of
 * these is intercepted UNCONDITIONALLY on SVM (HYPE_SVM_INTERCEPT_SVM_INSNS, set for every guest
 * with no opt-out -- see hype_vmcb_build_long_mode_guest()) and is architecturally undefined on
 * Intel silicon (AMD's SVM opcode space, 0F 01 D8-DF, names nothing on a CPU with no SVM unit), so
 * on EITHER backend the CPU takes #UD before any real side effect. try_dangerous_insn() below
 * checks that the fault actually happened, immediately, per instruction -- and if one ever comes
 * back having NOT faulted, the test stops trying the rest of the group and fails loudly rather
 * than executing whatever comes next in a now-untrustworthy state. That check is the whole point
 * of this file existing before it is anything else.
 *
 * COVERAGE TABLE -- exit reason -> probe -> EXHIST counter (boot/main.c's per-VM stat_ex_* /
 * the "fw-1 EXHIST:" debug line). EXHIST is dumped at most once per ~30s of wall-clock per VM
 * (fw_1_publish_and_render()'s cadence), so a run must last >= 35s before the coordinator greps
 * the LAST "fw-1 EXHIST:" line for this VM's index and checks each named bucket is nonzero. A
 * bucket that stays zero after a probe reported PASS means the intercept was never taken -- that
 * is the finding #603 exists to catch, not a pass.
 *
 *   exit reason                          probe (this file unless noted)         EXHIST counter
 *   ------------------------------------ --------------------------------------- ----------------
 *   HLT                                   probe_hlt() [depth: intdeliver.c]      hlt
 *   CPUID                                 probe_cpuid() [depth: cpumsr.c]        cpuid
 *   RDMSR/WRMSR, known MSR round-trip     probe_msr() [depth: cpumsr.c]          msr
 *   RDMSR/WRMSR, genuinely UNKNOWN MSR    probe_msr()                            msr
 *     FINDING: hype does NOT #GP an unknown MSR (svm_vcpu.c / vmcs_hw.c's
 *     HYPE_MSR_ACTION_REJECT/default case absorbs it -- WRMSR ignored, RDMSR
 *     returns 0, comment: "Absorbing is BEHAVIOUR ... the same absorb-rather-
 *     than-die posture GLADDER-1 established for unmodelled MMIO"). The ticket
 *     anticipated a #GP; the actual, deliberate behaviour is absorb. This probe
 *     asserts the REAL behaviour and prints the divergence rather than forcing
 *     a #GP that would not happen.
 *   IN/OUT, register form                 probe_ioio() [depth: ps2.c/pci.c/...]  ioio
 *   IN/OUT, REP string form (outsb/insb)  probe_ioio()                           ioio
 *   NPF/EPT-violation, unmodelled MMIO    probe_mmio_absorb()                    npf
 *     (single touch, GLADDER-1's absorb path: reads all-ones, write dropped,
 *     RIP advances -- see boot/main.c's own "e.g. ICH9 RCBA" comment, the
 *     address this probe uses.)
 *   PAUSE filter / spin-loop preemption   [already covered: pausespin.c]        pause
 *   VMRUN                                 probe_dangerous_group()                other
 *   VMLOAD                                probe_dangerous_group()                other
 *   VMSAVE                                probe_dangerous_group()                other
 *   STGI                                  probe_dangerous_group()                other
 *   CLGI                                  probe_dangerous_group()                other
 *   SKINIT                                probe_dangerous_group()                other
 *   INVLPGA ("INVLPG-class", privileged)  probe_dangerous_group()                other
 *   INVLPG (plain, unprivileged)          probe_dangerous_group()                (none -- see below)
 *     FINDING: neither backend intercepts plain INVLPG at all (no
 *     HYPE_SVM_INTERCEPT_INVLPG-equivalent bit is ever set; VMX's proc-based
 *     controls never set an INVLPG-exiting bit either). It executes natively
 *     against the guest's own ASID/VPID-tagged TLB, which nested paging makes
 *     safe without interception. This probe's PASS condition is that NO fault
 *     and NO exit-count movement is observable -- a documented negative, not
 *     an omission.
 *   MONITOR                               probe_monitor_mwait()                  other, or none
 *   MWAIT                                 probe_monitor_mwait()                  other, or none
 *     FINDING (cpuid_emulate.c #256): hype intercepts NEITHER instruction. It
 *     only masks CPUID.01H:ECX.MONITOR so a well-behaved guest never picks
 *     MWAIT for its idle loop -- CPUID is advisory, not enforcement. Whether
 *     this probe sees a #UD (host CPU's own IA32_MISC_ENABLE has the Monitor
 *     FSM disabled) or a clean execute-and-resume (real MONITOR/MWAIT ran) is
 *     HOST-CPU-DEPENDENT, and either is treated as a pass -- the guest reports
 *     which one it saw, which is the finding worth having either way. A hang
 *     is the only failure: guarded by the same periodic-tick safety net as
 *     probe_hlt(), so a live MWAIT is woken by the next external interrupt
 *     exit (unconditionally intercepted) within one tick period.
 *   task switch (JMP/CALL to a TSS)       SKIPPED -- see probe_note_task_switch()
 *     Not reachable: this guest, like every guest this project targets
 *     (.claude memory: "min guest target ... all 64-bit only"), runs entirely
 *     in 64-bit long mode. Hardware task-switching (JMP/CALL/IRET to a TSS
 *     descriptor) is not available in 64-bit mode at all (Intel SDM Vol. 3A
 *     Sec 7.2.5) -- there is no instruction sequence this guest's own code could
 *     execute to reach it, so constructing one would mean dropping to legacy
 *     mode first, which is a different, much larger guest than this ticket
 *     asks for. Recorded here rather than silently omitted.
 *   INIT                                  probe_init_sipi()                      npf
 *   SIPI (Startup IPI)                    probe_init_sipi()                      npf
 *     via an ICR_LOW MMIO write at the guest's own LAPIC (0xFEE00300) -- an
 *     ordinary NPF-trapped MMIO access (devices/guest_lapic.c). EXHIST has no
 *     dedicated SIPI bucket; it folds into `npf` like every other LAPIC
 *     register write. This is the only probe needing a second vCPU (vcpus=2
 *     in suite-603.cfg): AP bring-up is meaningless to a 1-vCPU guest.
 *   unhandled/unrecognized exit reason     tests/micro/vmexitstorm.c (separate  (VM force-off; no
 *     (the section 6g/#538 isolation rule, M8-8's per-vCPU                      bucket -- the watch-
 *     watchdog)                            file -- see its own header)         dog fires from the
 *                                                                                same absorb path,
 *                                                                                so also folds into
 *                                                                                `npf` up to the
 *                                                                                force-off itself)
 *   triple fault / SHUTDOWN                probe_triple_fault() (last, by       other
 *     (this file's own finale)             design -- ends the VM)               [depth: faulter.c
 *                                                                                 is the original,
 *                                                                                 authoritative
 *                                                                                 triple-fault test]
 *
 * cmdline: none. This test takes no parameters -- every probe's behaviour is fixed.
 */
#include "micro_idt.h"

#define NAME "vmexit"

#define PIC_BASE 0x20u
#define IRQ0_VECTOR (PIC_BASE + 0u)
#define PIT_DIVISOR (MICRO_PIT_HZ / 100u) /* 100 Hz -- same as intdeliver.c/pausespin.c */

/* ---- shared tick source: armed once by probe_hlt(), kept running for every later probe that
 * needs a live interrupt source (MONITOR/MWAIT's safety net). ---- */
static volatile unsigned long long g_ticks;

MICRO_ISR(irq0_isr,
          "incq g_ticks(%rip)\n\t"
          "movb $0x20, %al\n\t"
          "outb %al, $0x20\n\t")

/* ---- #UD catcher, shared by every "this must fault" probe. g_ud_skip is the length in bytes of
 * the instruction that was expected to fault -- always 3 for the 0F 01 xx group this file uses,
 * spelled out anyway so a future probe with a different length cannot silently reuse the wrong
 * one. ---- */
static volatile unsigned long long g_ud_count;
static volatile unsigned long long g_ud_skip;

MICRO_ISR(ud_isr,
          "pushq %rax\n\t"
          "movq g_ud_skip(%rip), %rax\n\t"
          "addq %rax, 8(%rsp)\n\t"
          "incq g_ud_count(%rip)\n\t"
          "popq %rax\n\t")

static int g_fail_count;

static void note_fail(const char *what) {
    micro_puts("micro/" NAME ": PROBE FAIL -- ");
    micro_puts(what);
    micro_puts("\n");
    g_fail_count++;
}

static void note_pass(const char *what) {
    micro_puts("micro/" NAME ": probe ok -- ");
    micro_puts(what);
    micro_puts("\n");
}

/* ================= HLT (depth: intdeliver.c) ================= */
static void probe_hlt(void) {
    unsigned long long guard = 0ull;
    micro_puts("micro/" NAME ": HLT -- arming IRQ0 at 100 Hz and halting until one tick lands\n");
    while (g_ticks == 0ull) {
        __asm__ volatile("hlt" ::: "memory");
        if (++guard > 200000ull) {
            note_fail("HLT never woke -- no tick arrived (EXHIST hlt should still be nonzero; see "
                      "intdeliver.c for the deeper #580 wake-point assertion)");
            return;
        }
    }
    note_pass("HLT took at least one exit and resumed (EXHIST: hlt)");
}

/* ================= CPUID (depth: cpumsr.c) ================= */
#define LEAF1_ECX_MONITOR (1u << 3)
#define LEAF1_ECX_HYPERVISOR (1u << 31)

static void probe_cpuid(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1u), "c"(0u));
    (void)eax;
    (void)ebx;
    (void)edx;
    micro_puts("micro/" NAME ": CPUID leaf 1 ECX=");
    micro_put_hex(ecx);
    micro_puts("\n");
    if ((ecx & LEAF1_ECX_HYPERVISOR) == 0u) {
        note_fail("CPUID leaf 1 did not go through hype's emulation (hypervisor-present bit "
                  "absent) -- see cpumsr.c for the full invariant set");
        return;
    }
    if ((ecx & LEAF1_ECX_MONITOR) != 0u) {
        note_fail("CPUID leaf 1 reports MONITOR present -- #256 requires it masked");
        return;
    }
    note_pass("CPUID was intercepted and answered by hype's emulation (EXHIST: cpuid)");
}

/* ================= RDMSR/WRMSR (depth: cpumsr.c) ================= */
#define MSR_MTRR_VAR0_BASE 0x200u
#define MSR_GENUINELY_UNKNOWN 0x1FFFFFFFu /* not MTRR, not HV, not APIC_BASE/EFER/TSC -- see below */

static inline uint64_t do_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void do_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static void probe_msr(void) {
    uint64_t back;

    /* Known, modelled MSR: a variable MTRR base register round-trips (svm_vcpu.c / vmcs_hw.c
     * both store and return it). Proves the exit is taken AND serviced, not merely absorbed. */
    do_wrmsr(MSR_MTRR_VAR0_BASE, 0x123456000ull);
    back = do_rdmsr(MSR_MTRR_VAR0_BASE);
    micro_puts("micro/" NAME ": MSR round-trip (MTRR var0 base) wrote 0x123456000, read back ");
    micro_put_hex(back);
    micro_puts("\n");
    if (back != 0x123456000ull) {
        note_fail("a modelled MSR did not round-trip -- RDMSR/WRMSR exits are not being serviced "
                  "correctly");
        return;
    }

    /* Genuinely unknown MSR. FINDING: hype absorbs this (WRMSR ignored, RDMSR returns 0) rather
     * than injecting #GP -- see this file's header coverage table. Assert the REAL behaviour. */
    do_wrmsr(MSR_GENUINELY_UNKNOWN, 0xDEADBEEFull);
    back = do_rdmsr(MSR_GENUINELY_UNKNOWN);
    micro_puts("micro/" NAME ": unknown MSR 0x");
    micro_put_hex(MSR_GENUINELY_UNKNOWN);
    micro_puts(" read back ");
    micro_put_hex(back);
    micro_puts(" after this guest survived both the WRMSR and the RDMSR with no exception -- "
               "FINDING: absorbed, not #GP'd (see this file's header)\n");
    if (back != 0ull) {
        note_fail("an unknown MSR read back nonzero -- the absorb-to-zero contract changed");
        return;
    }
    note_pass("RDMSR/WRMSR were intercepted and serviced for both a known and an unknown MSR "
              "(EXHIST: msr)");
}

/* ================= IN/OUT, register and REP-string forms ================= */
#define PIC_MASTER_DATA 0x21u

static void probe_ioio(void) {
    uint8_t back;
    static const uint8_t out_buf[4] = {'v', 'm', 'x', '!'};
    static uint8_t in_buf[4];
    const uint8_t *src;
    uint8_t *dst;
    unsigned i;

    /* Register-form round trip on a modelled, read/write port. */
    micro_outb(PIC_MASTER_DATA, 0xA5u);
    back = micro_inb(PIC_MASTER_DATA);
    micro_puts("micro/" NAME ": OUT 0xA5 to the PIC mask register, read back 0x");
    micro_put_hex(back);
    micro_puts("\n");
    if (back != 0xA5u) {
        note_fail("IN did not read back what OUT wrote to a read/write port");
        return;
    }

    /* REP OUTSB: a whole string in one exit (boot/main.c's UART handler names this exact case,
     * "#286: one exit can carry a whole string"). This also lands as readable text in the log.
     * The "+S" constraint binds the POINTER VALUE (not the array) to RSI, which REP OUTSB then
     * advances -- src must be a plain pointer, not the const array itself. */
    src = out_buf;
    __asm__ volatile("cld\n\t"
                     "rep outsb"
                     : "+S"(src)
                     : "d"((uint16_t)MICRO_COM1), "c"(4u)
                     : "memory");
    micro_puts("\nmicro/" NAME ": (the 4 bytes 'vmx!' above this line came from a single REP "
               "OUTSB)\n");

    /* REP INSB: read the same byte back from the PIC mask register 4 times over. Every byte
     * should match what was written above -- proves the REP form is decoded per-iteration, not
     * just once and replicated. */
    micro_outb(PIC_MASTER_DATA, 0x5Au);
    dst = in_buf;
    __asm__ volatile("cld\n\t"
                     "rep insb"
                     : "+D"(dst)
                     : "d"((uint16_t)PIC_MASTER_DATA), "c"(4u)
                     : "memory");
    for (i = 0; i < 4u; i++) {
        if (in_buf[i] != 0x5Au) {
            note_fail("REP INSB did not deliver the same byte on every iteration");
            return;
        }
    }
    note_pass("register IN/OUT and REP OUTSB/INSB (string I/O) were all intercepted and serviced "
              "(EXHIST: ioio)");
}

/* ================= NPF/EPT-violation, unmodelled MMIO (GLADDER-1 absorb) ================= */
/*
 * boot/main.c's own comment names this exact address as the kind of chipset region a fuller guest
 * probes and finds unmodelled (ICH9's RCBA). It sits between the I/O APIC window (0xFEC00000,
 * modelled) and the LAPIC window (0xFEE00000, modelled), so it cannot collide with either, and
 * this guest never touches PCI config space, so no BAR is ever latched over it. HYPE_KBOOT_PD_PAGES
 * (core/kboot.h) identity-maps the low 4 GB, so this address is reachable without this guest
 * building any page tables of its own.
 */
#define UNMODELLED_MMIO_GPA 0xFED1C000ull

static void probe_mmio_absorb(void) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)UNMODELLED_MMIO_GPA;
    uint32_t v = *p;
    micro_puts("micro/" NAME ": read of unmodelled MMIO 0x");
    micro_put_hex(UNMODELLED_MMIO_GPA);
    micro_puts(" = 0x");
    micro_put_hex(v);
    micro_puts(" (GLADDER-1 absorbs an unclaimed region as all-ones)\n");
    if (v != 0xFFFFFFFFu) {
        note_fail("an unmodelled MMIO read did not come back all-ones -- either something now "
                  "claims this address, or the absorb path changed");
        return;
    }
    *p = 0x11223344u; /* dropped, per the same absorb contract */
    v = *p;
    if (v != 0xFFFFFFFFu) {
        note_fail("a write to unmodelled MMIO was not dropped -- readback changed");
        return;
    }
    note_pass("an unmodelled MMIO read/write round-tripped through the GLADDER-1 absorb path "
              "without faulting (EXHIST: npf)");
}

/* ================= the dangerous group: SVM privileged instructions + plain INVLPG ================= */
/*
 * Every trigger_*() below is exactly the 3-byte 0F 01 xx encoding named in this file's header --
 * nothing more, so there is nothing for the CPU to act on even in the (never-taken, on hype)
 * branch where interception is somehow missing. No operand is meaningful to any of them until
 * AFTER the point where hype's interception (or, on VMX, the CPU's own lack of an SVM unit) takes
 * over -- see the header's "HARD SAFETY RULE" paragraph.
 */
static void trigger_vmrun(void) { __asm__ volatile(".byte 0x0f,0x01,0xd8" ::: "memory"); }
static void trigger_vmload(void) { __asm__ volatile(".byte 0x0f,0x01,0xda" ::: "memory"); }
static void trigger_vmsave(void) { __asm__ volatile(".byte 0x0f,0x01,0xdb" ::: "memory"); }
static void trigger_stgi(void) { __asm__ volatile(".byte 0x0f,0x01,0xdc" ::: "memory"); }
static void trigger_clgi(void) { __asm__ volatile(".byte 0x0f,0x01,0xdd" ::: "memory"); }
static void trigger_skinit(void) { __asm__ volatile(".byte 0x0f,0x01,0xde" ::: "memory"); }
static void trigger_invlpga(void) { __asm__ volatile(".byte 0x0f,0x01,0xdf" ::: "memory"); }

/*
 * Returns 1 if the instruction faulted (#UD, caught and skipped by ud_isr) exactly once, 0
 * otherwise. A 0 here means the instruction did NOT fault -- on hype that should never happen,
 * and the caller must stop trying the rest of the group rather than push forward into a state
 * that might no longer be trustworthy.
 */
static int try_dangerous_insn(const char *name, void (*trigger)(void)) {
    unsigned long long before = g_ud_count;
    g_ud_skip = 3ull;
    trigger();
    if (g_ud_count != before + 1ull) {
        micro_puts("micro/" NAME ": ");
        micro_puts(name);
        micro_puts(" did NOT fault -- g_ud_count before=");
        micro_put_uint(before);
        micro_puts(" after=");
        micro_put_uint(g_ud_count);
        micro_puts("\n");
        return 0;
    }
    micro_puts("micro/" NAME ": ");
    micro_puts(name);
    micro_puts(" faulted with #UD as required, guest resumed normally\n");
    return 1;
}

static void probe_dangerous_group(void) {
    int ok = 1;
    long dummy = 0;

    ok = ok && try_dangerous_insn("VMRUN", trigger_vmrun);
    ok = ok && try_dangerous_insn("VMLOAD", trigger_vmload);
    ok = ok && try_dangerous_insn("VMSAVE", trigger_vmsave);
    ok = ok && try_dangerous_insn("STGI", trigger_stgi);
    ok = ok && try_dangerous_insn("CLGI", trigger_clgi);
    ok = ok && try_dangerous_insn("SKINIT", trigger_skinit);
    ok = ok && try_dangerous_insn("INVLPGA", trigger_invlpga);

    if (!ok) {
        /*
         * HARD CONSTRAINT: one of the SVM-privileged instructions did not fault. Whatever ran
         * for real, this guest's state is no longer trustworthy for the instructions still in
         * this group (GIF/nested state may have changed), so stop here rather than try more of
         * them, and fail loudly -- this is exactly the outcome the whole file exists to catch.
         */
        note_fail("a guest-ring-0 SVM instruction executed instead of faulting -- STOPPING the "
                  "dangerous-instruction group here rather than trying the rest; hype's "
                  "guest-isolation guarantee for VMRUN/VMLOAD/VMSAVE/STGI/CLGI/SKINIT/INVLPGA is "
                  "VIOLATED if this is ever seen on a real run");
        return;
    }
    note_pass("VMRUN/VMLOAD/VMSAVE/STGI/CLGI/SKINIT/INVLPGA all faulted with #UD, none executed "
              "for real (EXHIST: other)");

    /*
     * Plain INVLPG. FINDING: neither backend intercepts it (see this file's header), so it is
     * expected to execute NATIVELY -- no fault, no change to g_ud_count. `dummy`'s address is
     * arbitrary and safe: INVLPG only ever invalidates a TLB entry for this guest's own
     * ASID/VPID-tagged mapping, which is architecturally harmless regardless of which address is
     * named.
     */
    {
        unsigned long long before = g_ud_count;
        __asm__ volatile("invlpg (%0)" : : "r"(&dummy) : "memory");
        if (g_ud_count != before) {
            note_fail("plain INVLPG faulted -- the documented 'not intercepted, executes natively' "
                      "finding no longer holds (this would actually be an improvement, but the "
                      "coverage table's row needs updating to match)");
            return;
        }
    }
    note_pass("plain INVLPG executed natively with no fault and no VM-exit, as documented "
              "(no HYPE_SVM_INTERCEPT_INVLPG-equivalent bit exists on either backend)");
}

/* ================= MONITOR/MWAIT ================= */
static void probe_monitor_mwait(void) {
    static volatile unsigned char monitor_line[64];
    void *addr = (void *)(uintptr_t)&monitor_line[0];
    unsigned long long before = g_ud_count;

    g_ud_skip = 3ull;
    micro_sti(); /* MWAIT must be able to be woken by the tick armed in probe_hlt() */
    /* addr is loaded into some general register by the "r" input, then moved into RAX
     * explicitly -- MONITOR's address operand -- rather than binding a C variable directly to
     * RAX, since EAX is reused (zeroed) for MWAIT's hint bits a few lines later and reusing an
     * input-constrained register for something else mid-block is not something to rely on. */
    __asm__ volatile("movq %0, %%rax\n\t"
                     "xorl %%ecx, %%ecx\n\t"
                     "xorl %%edx, %%edx\n\t"
                     ".byte 0x0f,0x01,0xc8\n\t" /* monitor */
                     "xorl %%ecx, %%ecx\n\t"
                     "xorl %%eax, %%eax\n\t"
                     ".byte 0x0f,0x01,0xc9\n\t" /* mwait */
                     :
                     : "r"(addr)
                     : "rax", "rcx", "rdx", "memory");
    micro_cli();

    if (g_ud_count != before) {
        note_pass("MONITOR/MWAIT faulted with #UD (this host CPU's IA32_MISC_ENABLE has the "
                  "Monitor FSM disabled, or hype's masked CPUID bit is enforced some other way -- "
                  "EXHIST: other)");
        return;
    }
    /*
     * FINDING (cpuid_emulate.c #256): neither instruction is intercepted by hype on either
     * backend. If this host's real CPU supports MONITOR/MWAIT, they just ran -- no VM-exit, no
     * EXHIST bucket at all for this specific probe. That the guest is back here at all (rather
     * than hung) is itself the interesting fact: the periodic tick's unconditional
     * external-interrupt exit woke it and hype resumed the guest right after MWAIT.
     */
    note_pass("MONITOR/MWAIT executed WITHOUT faulting and WITHOUT any VM-exit for the instruction "
              "itself -- KNOWN, DOCUMENTED GAP (#256), not a new regression; the guest was woken "
              "by the next external-interrupt exit, same as an unintercepted HLT would be");
}

/* ================= task switch: not reachable, recorded rather than silently skipped ================= */
static void probe_note_task_switch(void) {
    micro_puts("micro/" NAME ": task-switch (JMP/CALL to a TSS) -- SKIPPED, not reachable from a "
               "64-bit long-mode guest's own instruction stream (Intel SDM Vol. 3A Sec 7.2.5: "
               "hardware task-switching does not exist in 64-bit mode). See this file's header.\n");
}

/* ================= INIT/SIPI: wake vCPU1 from vCPU0's own instruction stream ================= */
/*
 * A tiny 16-bit real-mode stub, hand-assembled rather than compiled: the AP starts executing at
 * (SIPI vector << 12) in real mode, and nothing in this project's toolchain emits 16-bit code.
 *
 *   B8 00 00          mov ax, 0
 *   8E D8             mov ds, ax
 *   FF 06 10 90       inc word [0x9010]      ; the flag vCPU0 polls, absolute (DS=0)
 *   FA                cli
 *   F4                hlt                     ; parks here; nothing else needs it to do anything
 *
 * Placed at GPA 0x9000 -- the first free page after core/kboot.h's own layout (cmdline ends at
 * 0x8FFF; the guest's own stack/GDT/IDT all sit at 0x80000 and above). SIPI vector 0x09 makes the
 * AP's starting CS:IP = 0x0900:0x0000, physical 0x9000 -- matching devices/guest_lapic.h's own
 * documented STARTUP semantics ("STARTUP hands it a real-mode entry at (vector << 12)").
 */
#define SIPI_TRAMPOLINE_GPA 0x9000ull
#define SIPI_FLAG_GPA 0x9010ull
#define SIPI_VECTOR 0x09u
#define SIPI_TARGET_APIC_ID 1u

#define LAPIC_BASE 0xFEE00000ull
#define LAPIC_ICR_LOW (LAPIC_BASE + 0x300ull)
#define LAPIC_ICR_HIGH (LAPIC_BASE + 0x310ull)

/* ICR_LOW delivery-mode field (bits 10:8) and shorthand (bits 19:18) -- devices/guest_lapic.h's
 * own HYPE_GUEST_LAPIC_ICR_DELMODE_ and SHORTHAND_NONE values, reproduced here because a microtest
 * may not include a hype header (micro.h's own rule). */
#define ICR_DELMODE_INIT (5u << 8)
#define ICR_DELMODE_STARTUP (6u << 8)
#define ICR_LEVEL_ASSERT (1u << 14)
#define ICR_SHORTHAND_NONE 0u
#define ICR_DESTMODE_PHYSICAL 0u

static void write_trampoline(void) {
    volatile uint8_t *t = (volatile uint8_t *)(uintptr_t)SIPI_TRAMPOLINE_GPA;
    static const uint8_t code[] = {0xB8, 0x00, 0x00, 0x8E, 0xD8, 0xFF, 0x06,
                                   0x10, 0x90, 0xFA, 0xF4};
    unsigned i;
    for (i = 0; i < sizeof(code); i++) {
        t[i] = code[i];
    }
    *(volatile uint16_t *)(uintptr_t)SIPI_FLAG_GPA = 0u;
}

static void send_icr(uint32_t icr_low) {
    *(volatile uint32_t *)(uintptr_t)LAPIC_ICR_HIGH = SIPI_TARGET_APIC_ID << 24;
    *(volatile uint32_t *)(uintptr_t)LAPIC_ICR_LOW = icr_low;
}

static void probe_init_sipi(void) {
    volatile uint16_t *flag = (volatile uint16_t *)(uintptr_t)SIPI_FLAG_GPA;
    unsigned long long guard = 0ull;

    write_trampoline();
    micro_puts("micro/" NAME ": INIT/SIPI -- trampoline written at 0x9000, sending INIT then "
               "STARTUP(vector=0x09) to APIC ID 1\n");

    send_icr(ICR_DELMODE_INIT | ICR_LEVEL_ASSERT | ICR_SHORTHAND_NONE | ICR_DESTMODE_PHYSICAL);
    send_icr(ICR_DELMODE_STARTUP | SIPI_VECTOR | ICR_SHORTHAND_NONE | ICR_DESTMODE_PHYSICAL);
    /* The legacy MP sequence sends STARTUP twice; the second is a no-op once the AP is already
     * away, and harmless if the first alone was enough. */
    send_icr(ICR_DELMODE_STARTUP | SIPI_VECTOR | ICR_SHORTHAND_NONE | ICR_DESTMODE_PHYSICAL);

    while (*flag == 0u) {
        __asm__ volatile("pause" ::: "memory");
        if (++guard > 50000000ull) {
            note_fail("vCPU1 never incremented the flag its SIPI trampoline writes -- either the "
                      "ICR write was not intercepted/routed, or the AP never started");
            return;
        }
    }
    note_pass("vCPU1 started from this guest's own INIT/SIPI and ran its trampoline (EXHIST: "
              "npf -- the ICR write is an ordinary LAPIC MMIO exit, no dedicated SIPI bucket "
              "exists)");
}

/* ================= triple fault: the deliberate finale ================= */
/*
 * Same technique as faulter.c (the original, authoritative triple-fault test), applied from a
 * guest that -- unlike faulter -- had a REAL IDT loaded for every probe above. `lidt` with a
 * zero-limit IDTR first, so the CPU cannot fetch ANY vector's descriptor: the first fault it takes
 * (from the ud2 below) cannot be delivered, the resulting double fault also cannot be delivered,
 * and the third failure is architectural SHUTDOWN -- a triple fault, ending this VM.
 */
static void probe_triple_fault(void) {
    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } null_idtr = {0, 0};

    micro_puts("\nmicro/" NAME ": triple fault -- this VM must stop now, and no other VM may "
               "notice (EXHIST: other)\n");
    micro_fail(NAME, "deliberately triple-faulting after every other probe ran (#603) -- this VM "
                     "ends here on purpose");

    __asm__ volatile("lidt %0" : : "m"(null_idtr));
    __asm__ volatile("ud2");

    micro_puts("micro/" NAME ": UNREACHABLE -- the triple fault did not happen\n");
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    (void)zero_page_gpa;

    micro_puts("\nmicro/" NAME ": systematic VM-exit coverage starting (#603) -- see this file's "
               "header for the full exit-reason -> probe -> EXHIST table\n");

    micro_cli();
    micro_gdt_load();
    micro_idt_load();
    micro_idt_set_gate(6u, ud_isr); /* #UD -- hype intercepts this vector unconditionally */
    micro_idt_set_gate(IRQ0_VECTOR, irq0_isr);
    micro_pic_remap((uint8_t)PIC_BASE, (uint8_t)(PIC_BASE + 8u));
    micro_pit_periodic((uint16_t)PIT_DIVISOR);
    micro_pic_unmask(0u);
    micro_sti();

    probe_hlt();
    micro_cli();
    probe_cpuid();
    probe_msr();
    probe_ioio();
    probe_mmio_absorb();
    probe_dangerous_group();
    probe_monitor_mwait();
    probe_note_task_switch();
    probe_init_sipi();

    micro_puts("\nmicro/" NAME ": ");
    micro_put_uint((unsigned long long)g_fail_count);
    micro_puts(" probe(s) failed out of the non-fatal set above\n");

    probe_triple_fault();

    /* UNREACHABLE if the triple fault worked. If it did not, say so honestly rather than let a
     * missing verdict above look like the whole run silently vanished. */
    micro_halt();
}
