# SMP-13 (#469): host-side stack switch between shared-tier vCPU loops on one core.
#
# Each shared vCPU keeps its own host stack and its own dispatch-loop invocation, so every
# function-local variable of that loop stays per-vCPU by construction (the #237/#413 class of
# bug cannot arise from locals). A switch saves the callee-saved state of the running loop on its
# own stack and resumes another one.
#
# ABI: x86_64-unknown-uefi = Microsoft x64.
#   hype_coro_switch(uint64_t *save_sp /* rcx */, uint64_t load_sp /* rdx */)
# Callee-saved under MS x64: RBX RBP RDI RSI R12-R15, XMM6-XMM15 (+RSP). RFLAGS is saved too, so
# a loop resumes with the interrupt flag it yielded with. Frame, from the saved RSP upward:
#   [0..159]   XMM6..XMM15 (movdqu: SSE only, no VEX -- tools/check-no-vex.sh)
#   [160..223] R15 R14 R13 R12 RSI RDI RBP RBX
#   [224]      RFLAGS
#   [232]      return address
.set XMM_BYTES, 160

.text
.globl hype_coro_switch
hype_coro_switch:
    pushfq
    pushq %rbx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $XMM_BYTES, %rsp
    movdqu %xmm6, 0(%rsp)
    movdqu %xmm7, 16(%rsp)
    movdqu %xmm8, 32(%rsp)
    movdqu %xmm9, 48(%rsp)
    movdqu %xmm10, 64(%rsp)
    movdqu %xmm11, 80(%rsp)
    movdqu %xmm12, 96(%rsp)
    movdqu %xmm13, 112(%rsp)
    movdqu %xmm14, 128(%rsp)
    movdqu %xmm15, 144(%rsp)
    movq %rsp, (%rcx)
    movq %rdx, %rsp
    movdqu 0(%rsp), %xmm6
    movdqu 16(%rsp), %xmm7
    movdqu 32(%rsp), %xmm8
    movdqu 48(%rsp), %xmm9
    movdqu 64(%rsp), %xmm10
    movdqu 80(%rsp), %xmm11
    movdqu 96(%rsp), %xmm12
    movdqu 112(%rsp), %xmm13
    movdqu 128(%rsp), %xmm14
    movdqu 144(%rsp), %xmm15
    addq $XMM_BYTES, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rbx
    popfq
    ret

# First entry of a new loop: hype_coro_init() left entry in R12 and its argument in R13, and a
# RSP that is 8 mod 16 here, as at any function entry. Reserve the 32-byte shadow space (+8 to
# realign), call entry(arg). A loop must never return; if one does, halt this core's copy of it.
.globl hype_coro_trampoline
hype_coro_trampoline:
    subq $40, %rsp
    movq %r13, %rcx
    callq *%r12
1:
    cli
    hlt
    jmp 1b
