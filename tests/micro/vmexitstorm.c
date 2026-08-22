/*
 * #603 item 4: the unhandled-exit path itself -- an exit hype does not claim to handle must stop
 * that VM alone, per plan.md Sec 6g and #538's isolation rule, with a full diagnostic.
 *
 * A SEPARATE FILE, not another probe inside vmexit.c, for the same reason faulter.c stands alone:
 * once this guest's watchdog trips, hype force-powers THIS VM off (core/vm_watchdog.c's
 * HYPE_VM_HEALTH_FAULTED_STORM -> boot/main.c's fw_1_watchdog_observe() ->
 * hype_vm_lifecycle_next(..., HYPE_VM_EV_FORCE_OFF)) and this guest's own code never runs again.
 * vmexit.c's own finale (a triple fault) already claims the "last thing that runs" slot for a
 * different mechanism; a single guest cannot deliberately trigger two distinct VM-ending
 * mechanisms and have both be "last".
 *
 * HOW THIS WAS FOUND, not guessed. core/tests/vm_watchdog.c's own header lays out the exact
 * liveness rule: an UNHANDLED exit repeating at the SAME guest RIP, HYPE_VM_WATCHDOG_STORM_THRESHOLD
 * (4096) times in a row, is the one fault signature besides shutdown/triple-fault that the watchdog
 * treats as "this guest cannot advance" rather than "this guest is idle" or "this guest is making
 * slow legitimate progress". boot/main.c calls fw_1_watchdog_observe() from exactly ONE call site:
 * the GLADDER-1 unmodelled-MMIO absorb path (grep for "M8-8 (#171): this is the one place in the
 * loop where an exit is known to have hit NO modelled device -- exactly the 'unrecognized VM-exit'
 * the watchdog is about"). That is the mechanism this file drives: a tight loop touching an
 * unclaimed MMIO address from the SAME instruction (a backward jump re-executes the identical
 * `mov` at the identical RIP every time), so hype sees the identical (reason, rip) pair repeat past
 * the threshold.
 *
 * A GENUINE "hype's dispatch table has literally no case for this exit REASON CODE" probe (as
 * opposed to "reached a known, modelled catch-all path repeatedly") was considered and NOT used.
 * The two candidates found while researching #603 were: (a) VMX exit reasons for VMPTRLD/VMREAD/
 * VMWRITE/VMXOFF/VMCLEAR/VMRESUME/INVEPT/INVVPID, none of which boot/main.c defines a
 * HYPE_VMX_EXIT_REASON_* constant for at all -- but those instructions are ARCHITECTURALLY
 * intercepted unconditionally in VMX non-root operation regardless of any control bit, and their
 * exact resulting dispatch-table behaviour (which "else" branch catches an exit reason with no
 * named constant) was not something this ticket's author could verify without running the actual
 * VMX path under QEMU, which this worktree is forbidden to do; guessing at undefined-dispatch
 * behaviour and shipping it as a "safe" probe is exactly the "probably-unsafe instruction sequence"
 * #603 says to skip rather than construct blind. (b) An SVM EXITCODE hype's if-chain does not
 * classify at all (there is no default/else that PANICs on an utterly unknown code -- everything
 * ends up in ex_other and continues) -- also unverifiable without a run. The watchdog-storm path
 * above is the one #603 sub-probe for "an exit hype does not handle" that could be grounded
 * entirely in code already read, with a mechanism (core/vm_watchdog.c) that is itself unit-tested
 * (core/tests/test_vm_watchdog.c) and therefore known to behave exactly as described.
 *
 * WHAT PROVES THIS WORKED. This guest prints its intent and then goes silent -- there is no
 * verdict line to grep for from vmexitstorm ITSELF, because a VM that hype has force-powered off
 * cannot print one. What the coordinator checks after the run:
 *
 *   1. The host debug log (fw-1 WATCHDOG line) for THIS VM's index: "fw-1 WATCHDOG vmN: faulted:
 *      unhandled-exit storm at one RIP (reason=... rip=... repeats>=4096) -- forcing THIS vm off;
 *      others keep running".
 *   2. suite-603.cfg's paired healthy VM (hello) still reaches its own "MICRO PASS: hello" --
 *      proving the force-off stayed confined to this VM alone, which is the Sec 6g claim under
 *      test, not merely that the watchdog fired at all.
 *
 * A run that shows the WATCHDOG line but NOT (2) -- the paired VM also stopped, or never reached
 * its own PASS -- is the finding #603 asks to be surfaced, not silently treated as a pass: it
 * would mean the force-off did not stay confined to this VM alone, which is the whole Sec 6g claim.
 */
#include "micro.h"

#define NAME "vmexitstorm"

/*
 * Same address vmexit.c's probe_mmio_absorb() uses, for the same reasons (see that file's own
 * comment): unclaimed by any device model, identity-mapped by core/kboot.h's 4 GB map, clear of
 * the I/O APIC and LAPIC windows either side of it.
 */
#define UNMODELLED_MMIO_GPA 0xFED1C000ull

/*
 * core/vm_watchdog.h's HYPE_VM_WATCHDOG_STORM_THRESHOLD is 4096 consecutive unhandled exits at
 * one (reason, rip) pair. 8192 clears that with margin without this microtest needing to include
 * the host header (micro.h's own rule: a microtest may not include a hype header).
 */
#define STORM_ITERATIONS 8192ull

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    (void)zero_page_gpa;

    micro_puts("\n");
    micro_puts("micro/" NAME ": alive. About to read guest-physical 0x");
    micro_put_hex(UNMODELLED_MMIO_GPA);
    micro_puts(" ");
    micro_put_uint(STORM_ITERATIONS);
    micro_puts(" times in a row from ONE instruction, to trip hype's #171 per-vCPU watchdog "
               "(plan.md Sec 6g, #538, #603 item 4) and force THIS VM off alone.\n");
    micro_puts("micro/" NAME ": if this is the last line this VM's log ever shows, the watchdog "
               "worked -- check the host log for 'fw-1 WATCHDOG vm<N>: faulted: unhandled-exit "
               "storm at one RIP' naming this VM, and confirm the paired healthy VM in "
               "suite-603.cfg still reached its own PASS.\n");

    /*
     * The loop. A backward jump re-executes the SAME `movl` instruction at the SAME address every
     * time -- the watchdog keys on (reason, rip) both matching, not merely on the exit reason
     * repeating. `ecx` counts down in a register so the loop body touches nothing this microtest
     * would need to fault out of separately.
     */
    __asm__ volatile("movq %0, %%rbx\n\t"
                     "movq %1, %%rcx\n\t"
                     "1:\n\t"
                     "movl (%%rbx), %%eax\n\t"
                     "decq %%rcx\n\t"
                     "jnz 1b\n\t"
                     :
                     : "r"(UNMODELLED_MMIO_GPA), "r"(STORM_ITERATIONS)
                     : "rax", "rbx", "rcx", "memory");

    /*
     * UNREACHABLE if the watchdog fired: hype stops scheduling this vCPU once the VM is forced
     * off, and control never returns here. Reaching this line and printing a verdict is itself
     * the #603 finding this file exists to be able to report -- the mechanism did NOT trip within
     * the iteration count given, which is worth knowing exactly because it looks like nothing
     * happened rather than like a failure.
     */
    micro_puts("micro/" NAME ": UNREACHABLE (or so it should be) -- the loop completed and this "
               "VM is still running. FINDING: the #171 watchdog did NOT force this VM off within ");
    micro_put_uint(STORM_ITERATIONS);
    micro_puts(" identical unhandled exits at one RIP. Either the threshold moved, the address no "
               "longer misses every device model, or the mechanism itself regressed -- this is not "
               "a pass for #603 item 4.\n");
    micro_fail(NAME, "completed the storm loop without being force-powered off -- the #171 "
                     "watchdog was expected to end this VM before reaching this line");
    micro_halt();
}
