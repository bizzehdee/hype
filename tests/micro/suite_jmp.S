/*
 * #554: minimal setjmp/longjmp for the suite kernel.
 *
 * Every microtest ends by calling micro_halt(), which in a standalone artifact is an infinite
 * `hlt` -- correct there, and fatal to a suite that needs to run the next test. Rather than edit
 * ten tests to return instead of halting (and lose the property that a standalone artifact and a
 * suite member are the SAME code), micro_halt() longjmps back to the dispatcher when built with
 * MICRO_SUITE.
 *
 * Freestanding, so there is no libc setjmp to use. This is the x86-64 SysV callee-saved set plus
 * the stack pointer and return address -- the complete set a non-local jump must restore. There is
 * no signal mask and no FP state to save: these guests have neither.
 *
 * Layout: [0]=rbx [8]=rbp [16]=r12 [24]=r13 [32]=r14 [40]=r15 [48]=rsp [56]=return address.
 */
    .section .text
    .globl micro_setjmp
    .globl micro_longjmp

micro_setjmp:                       /* int micro_setjmp(micro_jmp_buf b) -- b in %rdi */
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax              /* the RSP the caller will see after our ret */
    movq %rax, 48(%rdi)
    movq (%rsp), %rax               /* our return address */
    movq %rax, 56(%rdi)
    xorl %eax, %eax                 /* first return: 0 */
    ret

micro_longjmp:                      /* void micro_longjmp(micro_jmp_buf b, int v) -- b/%rdi v/%esi */
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movl %esi, %eax
    testl %eax, %eax                /* longjmp(b, 0) must still return non-zero */
    jnz 1f
    movl $1, %eax
1:  jmp *56(%rdi)
