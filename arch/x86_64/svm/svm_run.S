# SVM VMRUN trampoline (#412). Hand-written asm -- nothing to unit test; the
# sequence is grounded in AMD APM Vol 2 (VMRUN saves/restores host RAX, and the
# guest RAX/RSP/RFLAGS live in the VMCB, so this code never touches them).
#
# Replaces the former HYPE_VMRUN_BODY inline-asm macro, which bound the guest
# GPR save/restore to STATIC pool-slot addresses (g_ctx_pool[0]/[1]) because the
# guest clobbers every register, leaving none free to hold a ctx pointer -- and
# so capped concurrent SVM vCPUs at compile time. This trampoline stashes the
# ctx pointer on the stack across the guest exactly as vmx_run.S does, so a
# runtime-allocated, arbitrarily-sized ctx pool works.
#
# ABI: x86_64-unknown-uefi = Microsoft x64 (see isr_stubs.S / vmx_run.S):
#   arg0 (ctx)       -> %rcx
#   arg1 (vmcb_phys) -> %rdx
# Callee-saved under MS x64: RBX RBP RDI RSI R12-R15 (+RSP), preserved here.
#
# VMRUN specifics vs VMX: VMRUN returns to the instruction after it (no
# HOST_RIP/HOST_RSP), there is no launch/resume distinction, and guest
# RAX/RSP/RFLAGS are saved/restored through the VMCB -- so this loads and saves
# only RCX RDX RBX RBP RSI RDI R8-R15, never RAX(gprs[0]) or RSP(gprs[4]),
# matching the operand set of the macro it replaces.
#
# ctx layout: SVM's struct hype_vcpu_ctx starts with `hype_vmcb_t *vmcb` (8
# bytes), THEN uint64_t gprs[16] -- so gprs is at offset G below, NOT 0 (unlike
# VMX's ctx, which vmx_run.S addresses at offset 0). Register encoding order:
#   0=RAX 1=RCX 2=RDX 3=RBX 4=RSP(in VMCB) 5=RBP 6=RSI 7=RDI 8..15=R8..R15
.set G, 8   # byte offset of gprs[] within struct hype_vcpu_ctx (past `vmcb *`)

.text
.globl hype_svm_vmrun
hype_svm_vmrun:
    # save host callee-saved regs
    pushq %rbp
    pushq %rbx
    pushq %rsi
    pushq %rdi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rcx              # [ctx] -- survives the guest, reloaded on exit

    movq %rdx, %rax         # RAX = vmcb_phys, the vmrun operand

    # load guest GPRs from ctx (%rcx still = ctx here)
    movq G+8*2(%rcx), %rdx
    movq G+8*3(%rcx), %rbx
    movq G+8*5(%rcx), %rbp
    movq G+8*6(%rcx), %rsi
    movq G+8*7(%rcx), %rdi
    movq G+8*8(%rcx), %r8
    movq G+8*9(%rcx), %r9
    movq G+8*10(%rcx), %r10
    movq G+8*11(%rcx), %r11
    movq G+8*12(%rcx), %r12
    movq G+8*13(%rcx), %r13
    movq G+8*14(%rcx), %r14
    movq G+8*15(%rcx), %r15
    movq G+8*1(%rcx), %rcx    # guest RCX last (ctx ptr now only on the stack)

    vmrun %rax             # guest RAX/RSP/RFLAGS come from the VMCB

    # VMRUN returned after #VMEXIT. Save guest GPRs back into ctx. Need ctx in a
    # reg without losing a guest value: stash guest RCX, load ctx from the stack.
    pushq %rcx             # save guest RCX (rsp now -8; [ctx] at 8(%rsp))
    movq 8(%rsp), %rcx     # rcx = [ctx]
    movq %rdx, G+8*2(%rcx)
    movq %rbx, G+8*3(%rcx)
    movq %rbp, G+8*5(%rcx)
    movq %rsi, G+8*6(%rcx)
    movq %rdi, G+8*7(%rcx)
    movq %r8,  G+8*8(%rcx)
    movq %r9,  G+8*9(%rcx)
    movq %r10, G+8*10(%rcx)
    movq %r11, G+8*11(%rcx)
    movq %r12, G+8*12(%rcx)
    movq %r13, G+8*13(%rcx)
    movq %r14, G+8*14(%rcx)
    movq %r15, G+8*15(%rcx)
    popq %rdx             # guest RCX (rsp back at [ctx])
    movq %rdx, G+8*1(%rcx)
    addq $8, %rsp         # discard [ctx]

    # restore host callee-saved regs
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbx
    popq %rbp
    ret
