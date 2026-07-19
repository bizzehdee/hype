# M8-0b: application-processor (AP) startup trampoline.
#
# Firmware MP services don't survive ExitBootServices, so hype brings up a
# second core itself: it copies this blob to a page below 1 MiB, fills the
# parameter block at the end, and sends INIT-SIPI-SIPI (see ap_boot.c /
# lapic.c). A Startup IPI lands the AP here in 16-bit real mode with CS =
# (page >> 4) and IP = 0 -- so CS<<4 is exactly this blob's physical base.
#
# The blob is fully relocatable: every internal reference is computed as
# (label - hype_ap_tramp_start)(%ebx), where %ebx = CS<<4 = the runtime base.
# Nothing in the *code* is patched -- only the data parameter block (cr3,
# entry, stack) is written by the BSP before the SIPI, and the AP writes the
# `alive` flag the BSP polls.
#
# Real -> protected -> long, the standard two-far-jump path. Hand asm; the
# only unit-testable logic (the ICR encoders) lives in lapic.c, tested there.

.section .rodata
.code16
.globl hype_ap_tramp_start
hype_ap_tramp_start:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds                 # DS = CS so (label-start)(%ebx) reads resolve
    movzwl %cs, %ebx
    shll $4, %ebx                 # %ebx = physical base of this copy (CS << 4)

    # GDTR.base = base + offset(gdt); limit is preset in the data below.
    leal (gdt - hype_ap_tramp_start)(%ebx), %eax
    movl %eax, (gdtr_base - hype_ap_tramp_start)(%ebx)
    lgdtl (gdtr - hype_ap_tramp_start)(%ebx)

    # Far-pointer offset for the jump into 32-bit code (selector preset).
    leal (prot_entry - hype_ap_tramp_start)(%ebx), %eax
    movl %eax, (farptr32 - hype_ap_tramp_start)(%ebx)

    movl %cr0, %eax
    orl $0x1, %eax                # CR0.PE
    movl %eax, %cr0
    ljmpl *(farptr32 - hype_ap_tramp_start)(%ebx)

.code32
prot_entry:
    movw $0x10, %ax              # 32-bit data selector
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movl %cr4, %eax
    orl $0x20, %eax              # CR4.PAE
    movl %eax, %cr4

    movl (tramp_cr3 - hype_ap_tramp_start)(%ebx), %eax
    movl %eax, %cr3              # low 32 bits (BSP guarantees CR3 < 4 GiB)

    movl $0xC0000080, %ecx      # IA32_EFER
    rdmsr
    orl $0x100, %eax            # EFER.LME
    wrmsr

    movl %cr0, %eax
    orl $0x80000001, %eax       # CR0.PG | CR0.PE  -> activates long mode
    movl %eax, %cr0

    leal (long_entry - hype_ap_tramp_start)(%ebx), %eax
    movl %eax, (farptr64 - hype_ap_tramp_start)(%ebx)
    ljmpl *(farptr64 - hype_ap_tramp_start)(%ebx)

.code64
long_entry:
    movw $0x20, %ax             # 64-bit data selector
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    # %rbx == base (upper 32 bits zero-extended from the 32-bit writes; base < 1 MiB).
    movq (tramp_stack - hype_ap_tramp_start)(%rbx), %rsp
    movl $1, (tramp_alive - hype_ap_tramp_start)(%rbx)   # signal the BSP: mode switch OK

    movq (tramp_entry - hype_ap_tramp_start)(%rbx), %rax
    xorq %rcx, %rcx             # MS x64 ABI arg0 = 0 (unused for now)
    callq *%rax                 # into hype_ap_entry(); it never returns
1:  cli
    hlt
    jmp 1b

# ---- data: GDT + far pointers + parameter block (all within the blob) ----
.align 8
gdt:
    .quad 0x0000000000000000    # 0x00 null
    .quad 0x00CF9A000000FFFF    # 0x08 32-bit code (G=1,D=1)
    .quad 0x00CF92000000FFFF    # 0x10 32-bit data
    .quad 0x00AF9A000000FFFF    # 0x18 64-bit code (G=1,L=1)
    .quad 0x00CF92000000FFFF    # 0x20 64-bit data
gdt_end:

gdtr:
    .word gdt_end - gdt - 1     # limit
gdtr_base:
    .long 0                     # base (filled at runtime by the trampoline)

farptr32:
    .long 0                     # offset (filled at runtime)
    .word 0x08                  # 32-bit code selector
farptr64:
    .long 0                     # offset (filled at runtime)
    .word 0x18                  # 64-bit code selector

.align 8
.globl hype_ap_tramp_cr3
hype_ap_tramp_cr3:
tramp_cr3:
    .quad 0
.globl hype_ap_tramp_entry
hype_ap_tramp_entry:
tramp_entry:
    .quad 0
.globl hype_ap_tramp_stack
hype_ap_tramp_stack:
tramp_stack:
    .quad 0
.globl hype_ap_tramp_alive
hype_ap_tramp_alive:
tramp_alive:
    .long 0

.globl hype_ap_tramp_end
hype_ap_tramp_end:
