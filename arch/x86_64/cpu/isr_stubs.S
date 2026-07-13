# Per-vector ISR entry stubs + common trampoline (M1-2, extended M1-8).
#
# Hand-written asm with nothing to unit test -- see idt.h for the split
# between this and the tested encode/decode logic. Generated via
# .rept/.altmacro rather than 256 hand-written blocks so the vector
# list (which of 0-31 push a hardware error code) is the only thing a
# human has to get right and verify against the SDM, not 256 repeats.
#
# This target (x86_64-unknown-uefi) uses the Microsoft x64 calling
# convention for *every* C function, not just EFIAPI ones -- verified
# empirically (see docs/toolchain.md), not assumed. That means the
# frame pointer goes in %rcx (not %rdi), and the call needs 32 bytes of
# shadow space plus 16-byte stack alignment, unlike a SysV target.
#
# hype_isr_dispatch() (isr_decode.c) can now return normally -- M1-8
# needs the timer IRQ to resume the interrupted code, not halt -- so
# unlike M1-2's original version, the pre-call %rsp (before the
# alignment below discards it) has to survive the call so it can be
# restored afterward for a clean register-pop + `iretq` sequence. %rbp
# is callee-saved under the calling convention above, so C code called
# from here is required to preserve whatever we stash there across the
# call -- exactly what a plain scratch stash needs.

.altmacro

.macro isr_noerr num
.globl isr_stub_\num
isr_stub_\num:
    pushq $0
    pushq $\num
    jmp isr_common_entry
.endm

.macro isr_err num
.globl isr_stub_\num
isr_stub_\num:
    pushq $\num
    jmp isr_common_entry
.endm

# Vectors 8, 10-14, 17, 21, 29, 30 push a hardware error code (Intel SDM
# Vol 3, 6.13 "Error Code" table); every other vector does not, so the
# stub pushes a dummy 0 to keep the frame uniform.
.macro gen_stub num
    .if \num==8 || \num==10 || \num==11 || \num==12 || \num==13 || \num==14 || \num==17 || \num==21 || \num==29 || \num==30
        isr_err %num
    .else
        isr_noerr %num
    .endif
.endm

.text

isr_common_entry:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rcx      # MS x64 ABI arg0 = pointer to hype_isr_frame_t
    mov %rsp, %rbp      # stash pre-alignment rsp (callee-saved -- survives the call)
    and $-16, %rsp      # force 16-byte alignment
    sub $0x20, %rsp     # MS x64 ABI shadow space
    call hype_isr_dispatch
    mov %rbp, %rsp      # undo the alignment; rsp is back at the top of our pushes

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp       # discard the vector + error_code the stub pushed
    iretq

.set i, 0
.rept 256
    gen_stub %i
    .set i, i+1
.endr

.macro emit_stub_addr num
.quad isr_stub_\num
.endm

.section .rodata
.globl hype_isr_stub_table
hype_isr_stub_table:
.set i, 0
.rept 256
    emit_stub_addr %i
    .set i, i+1
.endr
