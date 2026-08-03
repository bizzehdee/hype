#ifndef HYPE_ARCH_VMM_OPS_H
#define HYPE_ARCH_VMM_OPS_H

#include <stdint.h>

#include "cpu_features.h"

/*
 * Vendor-agnostic VM-exit info, filled in by whichever backend actually
 * ran the vCPU. `reason` and `qualification` carry each backend's own
 * native exit-reason encoding for now (VMX's 32-bit exit reason vs
 * SVM's #VMEXIT code are different numbering spaces) -- M2-5's dispatch
 * loop is what gives them a shared meaning; M2 alone doesn't need more
 * than "did we get an exit, and does the hlt-loop guest's known exit
 * reason show up."
 */
typedef struct {
    uint64_t reason;
    uint64_t qualification;
    uint64_t guest_rip;
} hype_vmexit_info_t;

/* Opaque per-vCPU context. Each backend (arch/x86_64/vmx,
 * arch/x86_64/svm) defines its own concrete struct; the VM-exit
 * dispatch loop (M2-5) and device model only ever see this pointer,
 * which is what keeps them vendor-agnostic (plan.md §4). */
typedef struct hype_vcpu_ctx hype_vcpu_ctx_t;

/*
 * Snapshot of the guest's interrupt-acceptance state (M4-6d2), filled by
 * whichever backend ran the vCPU. Says whether the guest is blocked with
 * interrupts disabled (`rflags` IF=0 / `interrupt_shadow` set / !can_accept)
 * versus ready-but-not-delivered, and whether an event is staged for entry /
 * an interrupt window is armed / a vector is still pending.
 *
 * Vendor-neutral (VMX-4, #236) because the FW-1 loop uses it to DECIDE, not
 * only to log -- it gates `pic_ready` and the HLT idle-wait path. The two
 * fields whose names come from SVM mean the architectural equivalent on VMX:
 *   eventinj -- SVM's VMCB EVENTINJ / VMX's VM_ENTRY_INTR_INFO_FIELD
 *   vintr    -- whether an interrupt window is armed: SVM's V_IRQ in the VINTR
 *               control / VMX's interrupt-window-exiting proc-based control
 * `interrupt_shadow` is SVM's INTERRUPT_SHADOW / VMX's
 * GUEST_INTERRUPTIBILITY_STATE (blocking by STI or by MOV SS).
 */
typedef struct {
    uint64_t rflags;
    uint64_t interrupt_shadow;
    uint64_t eventinj;
    uint64_t vintr;
    int can_accept;
    int pending_valid;
    uint8_t pending_vector;
} hype_vmm_intr_state_t;

/*
 * Decoded nested-paging fault (SVM NPF / VMX EPT violation). Vendor-neutral
 * because the *content* always was: which guest-physical address faulted, and
 * whether it was a write. Only where the two fields come from differs -- SVM's
 * EXITINFO1/EXITINFO2 versus VMX's EXIT_QUALIFICATION bit 1 and
 * GUEST_PHYSICAL_ADDRESS.
 */
typedef struct {
    int is_write;
    uint64_t guest_phys_addr;
} hype_vmm_npf_t;

/*
 * Decoded port-I/O intercept. Again the content is vendor-neutral; the source
 * is SVM's EXITINFO1 versus VMX's I/O EXIT_QUALIFICATION.
 *
 * is_string/is_rep/addr_size_bytes exist for INS/OUTS emulation (SVM-STRIO),
 * where the data moves to/from memory rather than a GPR and (E/R)SI/(E/R)DI/
 * (E/R)CX are indexed at the guest's current address size.
 */
typedef struct {
    int is_in;
    uint16_t port;
    uint8_t size_bytes;      /* operand size: 1, 2, or 4 */
    int is_string;           /* STR: INS/OUTS (data moves to/from memory, not a GPR) */
    int is_rep;              /* REP-prefixed (transfer (E)CX units, else exactly 1) */
    uint8_t addr_size_bytes; /* address size: 2, 4, or 8 (indexes/masks (E/R)SI/(E/R)DI/(E/R)CX) */
} hype_vmm_ioio_t;

typedef struct {
    const char *name;

    /* Enables VMX/SVM operation on the calling physical CPU (VMXON /
     * setting EFER.SVME). Returns 0 on success, non-zero on failure.
     * Uses the backend's own single static per-CPU page, so it is the
     * BSP's entry point; a second core must use enable_on() below. */
    int (*enable)(void);

    /*
     * Same, but on the CALLING core using a caller-owned 4KB-aligned page
     * (SVM: the VM_HSAVE_PA host-save area; VMX: the VMXON region). Both are
     * per-logical-processor and stay in use for as long as that core is in
     * VMX/SVM operation, so every core that will VMRUN/VMLAUNCH needs its own
     * -- which is all the page means here; what goes in it is the backend's
     * business, and that is exactly why this is in the vtable.
     *
     * #242: it is in the vtable because the AP landing previously called
     * hype_svm_enable_on() directly. That was correct when SVM was the only
     * backend and silently wrong afterwards -- on Intel the AP set EFER.SVME,
     * a reserved bit there, and #GP'd on every boot. Going through the ops
     * table means an AP has no backend left to pick wrongly.
     */
    int (*enable_on)(void *percore_page);

    /*
     * Allocates and minimally initializes a vCPU context: the guest
     * starts executing at guest_rip with guest_rsp, using
     * ept_or_npt_root as its EPT/NPT table root (physical address; 0 is
     * a valid "not set up yet" value pre-M3). Returns an opaque
     * context, or 0 (NULL) on failure.
     */
    hype_vcpu_ctx_t *(*vcpu_create)(uint64_t guest_rip, uint64_t guest_rsp,
                                     uint64_t ept_or_npt_root);

    /* Enables APICv (Intel)/AVIC (AMD) for this vCPU context (M2-4). */
    void (*vcpu_enable_apic_accel)(hype_vcpu_ctx_t *ctx);

    /*
     * Runs the vCPU until the next VM-exit, filling *info. Returns 0 on
     * a normal VM-exit; non-zero if VM-entry itself failed (a VMCS/VMCB
     * misconfiguration bug, not a guest action -- VMLAUNCH/VMRUN report
     * this distinctly from a real exit).
     */
    int (*vcpu_run)(hype_vcpu_ctx_t *ctx, hype_vmexit_info_t *info);

    /*
     * This vCPU's hardware TLB tag: the SVM ASID (#244) or the VMX VPID (#273).
     * 0 means "none assigned", which for SVM is the dangerous case -- a guest on
     * ASID 0 shares the host's TLB tag.
     *
     * In the vtable rather than as an SVM entry point because both vendors tag
     * guest TLB entries, just under different names, and the question asked of
     * it is the same on both: do two concurrent guests have DIFFERENT tags?
     *
     * It exists because that question could not be answered from a log. The
     * assignment is announced by a hype_debug_print inside the SVM create path,
     * but in a two-VM AMD run two cores contend for one UART and whole lines are
     * lost -- a run where both guests provably reached a login prompt contained
     * not one fragment of either ASID line, nor of the "launching real OVMF"
     * line printed immediately before it. So per #288's own conclusion, anything
     * that needs measuring has to be self-checking inside hype rather than
     * grepped: the owner core reads BOTH VMs' tags and prints them in ONE line,
     * exactly as the #274 isolation probe does for the NPT/EPT roots.
     */
    uint32_t (*vcpu_tlb_tag)(hype_vcpu_ctx_t *ctx);
} hype_vmm_ops_t;

#endif /* HYPE_ARCH_VMM_OPS_H */
