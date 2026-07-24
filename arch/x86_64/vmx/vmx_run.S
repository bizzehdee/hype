# VMX VM-entry/exit trampoline (VMX-1, #35). Hand-written asm -- nothing to unit
# test (see vmx.h's UNVALIDATED note); the field values + control flow are
# grounded in Intel SDM Vol 3C (VMLAUNCH/VMRESUME, HOST_RSP/HOST_RIP behaviour).
#
# Unlike SVM's VMRUN (which returns to the instruction after it), a VMX VM-exit
# jumps the CPU to VMCS HOST_RIP with HOST_RSP -- so this trampoline VMWRITEs
# both to point back into itself (the .Lvmexit label + the current stack), loads
# the guest GPRs, and VMLAUNCH/VMRESUMEs. On the VM-exit the CPU lands at
# .Lvmexit where the guest GPRs are saved back and the host GPRs restored.
#
# ABI: x86_64-unknown-uefi = Microsoft x64 for every C function (see isr_stubs.S):
#   arg0 (ctx)      -> %rcx
#   arg1 (launched) -> %rdx   (0 = first entry -> VMLAUNCH; 1 = VMRESUME)
#   return          -> %rax   (0 = clean VM-exit; 1 = VM-entry failure)
# Callee-saved under MS x64: RBX RBP RDI RSI R12-R15 (+ RSP). We also stash the
# ctx pointer + launched flag on the stack so they survive the guest, which
# clobbers every GPR.
#
# ctx layout: struct starts with uint64_t gprs[16], x86 reg encoding order
#   0=RAX 1=RCX 2=RDX 3=RBX 4=RSP(unused;in VMCS) 5=RBP 6=RSI 7=RDI 8..15=R8..R15

.set VMCS_HOST_RSP, 0x6C14
.set VMCS_HOST_RIP, 0x6C16

.text
.globl hype_vmx_launch
hype_vmx_launch:
    # save host callee-saved regs
    pushq %rbp
    pushq %rbx
    pushq %rsi
    pushq %rdi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rcx              # [ctx]      (survives the guest, restored on exit)
    pushq %rdx              # [launched] (top of stack)

    # HOST_RSP = current %rsp; HOST_RIP = .Lvmexit
    movq $VMCS_HOST_RSP, %rax
    vmwrite %rsp, %rax
    leaq .Lvmexit(%rip), %rax
    movq $VMCS_HOST_RIP, %rdx
    vmwrite %rax, %rdx

    # load guest GPRs from ctx (%rcx still = ctx here)
    movq 8*2(%rcx), %rdx
    movq 8*3(%rcx), %rbx
    movq 8*5(%rcx), %rbp
    movq 8*6(%rcx), %rsi
    movq 8*7(%rcx), %rdi
    movq 8*8(%rcx), %r8
    movq 8*9(%rcx), %r9
    movq 8*10(%rcx), %r10
    movq 8*11(%rcx), %r11
    movq 8*12(%rcx), %r12
    movq 8*13(%rcx), %r13
    movq 8*14(%rcx), %r14
    movq 8*15(%rcx), %r15
    movq 8*0(%rcx), %rax    # guest RAX
    movq 8*1(%rcx), %rcx    # guest RCX last (ctx ptr now only on the stack)

    cmpq $0, (%rsp)         # launched?
    jne .Lresume
    vmlaunch
    jmp .Lentryfail
.Lresume:
    vmresume
    # fall through on failure

.Lentryfail:
    # VMLAUNCH/VMRESUME fell through -> VM-entry failed (guest never ran).
    # Host regs are still on the stack; guest GPRs were loaded but unused.
    addq $8, %rsp          # discard [launched]
    popq %rcx              # discard [ctx]
    movq $1, %rax          # return 1 = entry failure
    jmp .Lrestore

.Lvmexit:
    # VM-exit: CPU restored HOST_RSP, so %rsp points at [launched] again.
    # Save the guest GPRs back into ctx. Need ctx in a reg without losing a
    # guest value: stash guest RAX, load ctx into RAX, store the rest.
    pushq %rax             # save guest RAX (rsp now -8)
    movq 16(%rsp), %rax    # ctx = [ctx] (was 8(%rsp); +8 for the RAX push)
    movq %rcx, 8*1(%rax)
    movq %rdx, 8*2(%rax)
    movq %rbx, 8*3(%rax)
    movq %rbp, 8*5(%rax)
    movq %rsi, 8*6(%rax)
    movq %rdi, 8*7(%rax)
    movq %r8,  8*8(%rax)
    movq %r9,  8*9(%rax)
    movq %r10, 8*10(%rax)
    movq %r11, 8*11(%rax)
    movq %r12, 8*12(%rax)
    movq %r13, 8*13(%rax)
    movq %r14, 8*14(%rax)
    movq %r15, 8*15(%rax)
    popq %rdx              # guest RAX (rsp back to [launched])
    movq %rdx, 8*0(%rax)
    addq $8, %rsp          # discard [launched]
    popq %rcx             # discard [ctx]
    movq $0, %rax          # return 0 = clean VM-exit

.Lrestore:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbx
    popq %rbp
    ret
