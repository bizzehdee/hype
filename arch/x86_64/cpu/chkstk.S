/*
 * #436: __chkstk -- the Microsoft x64 stack-probe helper.
 *
 * hype is built for x86_64-unknown-uefi, so it uses the MS ABI, and that ABI
 * requires a function whose frame exceeds one page to probe every page it is
 * about to claim before moving RSP past them. The compiler emits a call here
 * automatically; there is no way to opt out short of keeping every frame under
 * 4 KB, which the dispatch loop no longer is.
 *
 * Probing matters even though hype's stacks are pre-allocated: touching each
 * page in descending order is what guarantees a fault lands on the guard page
 * adjacent to the stack rather than skipping over it into whatever lies below,
 * which would corrupt memory silently instead of failing loudly.
 *
 * Contract (per the MSVC documentation): RAX holds the number of bytes about to
 * be allocated. Every register except the flags is preserved, RSP is left
 * unchanged -- the caller performs its own `sub rsp, rax` after this returns.
 */

.text
.globl __chkstk
__chkstk:
    pushq %rcx
    pushq %rax
    /* RCX = the first address below the caller's return address: the two
     * pushes above plus the return address itself are 24 bytes. */
    leaq  24(%rsp), %rcx
    cmpq  $0x1000, %rax
    jb    2f

1:  /* Walk down a page at a time, touching each one. */
    subq  $0x1000, %rcx
    orq   $0, (%rcx)            /* the probe: a read-modify-write that changes nothing */
    subq  $0x1000, %rax
    cmpq  $0x1000, %rax
    ja    1b

2:  /* Probe the final partial page. */
    subq  %rax, %rcx
    orq   $0, (%rcx)

    popq  %rax
    popq  %rcx
    ret
