---
title: 'VMX'
source_page_id: '2598'
source_revision_id: '30309'
source_timestamp: '2026-03-17T18:52:08Z'
source_format: mediawiki
---

# VMX

> This page or section is a stub.

**Intel's Virtual-Machine Extensions** 

## Programming
### Discovering support
After enumerating the features from [CPUID](CPUID), bit 5 in ECX will tell you if VMX is supported or not.  This is the first thing that should be checked!

### Basic environment
Software should enable bit 13 of CR4 (CR4.VMXE) first.  Here are a few definitions of some used MSR's that we'll be dealing with: 
```
 #define IA32_VMX_BASIC          0x480
 #define IA32_VMX_CR0_FIXED0     0x486
 #define IA32_VMX_CR0_FIXED1     0x487
 #define IA32_VMX_CR4_FIXED0     0x488
 #define IA32_VMX_CR4_FIXED1     0x489
```

In order to see what your OS needs to support, you need to read the lower 32-bits of IA32_VMX_CR0_FIXED0 and IA32_VMX_CR4_FIXED0.  The bits in those MSR's are what need to be set (and supported) in their corresponding control registers (CR0/CR4).  If any of those bits are not enabled, a GPF will occur.  Also, to see what extra bits can be set, check the IA32_VMX_CR0_FIXED1 and IA32_VMX_CR4_FIXED1.  It's basically a mask of bits, if a bit is set to 1, then it "can" be enabled, but if it's a 0, you may generate an exception if you enable the corresponding bit in the control registers. 

From my own hardware and emulator (bochs), I've found that you will most likely need to enable these bits: CR0.NE, CR4.VMXE, CR0.PG and CR0.PE. For 64-bit kernels, you need to have IA32_EFER.LMA set.

### Executing VMXON
The main entry point for using VMX is through the VMXON instruction.  The instruction requires a single operand of a m64 region called the VMXON region.  The memory region needs to be 4096-byte aligned (bits 0-11 must be 0) and the only VMCS field that should be modified is the VMCS revision identification field.  This ID field should contain the value in bits 0-31 of MSR IA32_VMX_BASIC.  In order to prepare a memory address in 32-bit PMode for use as an m64, some modifications need to be made.  The upper 32-bits of the m64 on non long mode capable processors have to be 0 or an "invalid memory address" error will occur and a VMEXIT will be called.  
```
 uint32_t * region = (uint32_t *)allocate_4k_aligned(4096);
 uint64_t region64 = (uint64_t)((size_t)(region) & 0xFFFFFFFF);
 asm volatile("  vmxon %0; "::"m" (region64));
```

This general process of taking a 32-bit memory address and turning it into a psuedo-64bit int (unsigned long long) will be used for all m64 operands later.  VMCLEAR is another example instruction that requires the upper 32-bits of a memory address to be 0.  

Long mode capable processors simply requires a 64-bit pointer to the region.  

Note: **The VMXON, VMCLEAR and VMPTRLD instruction must point to the physical address of their respective regions.**

## VMX instruction error checking
VMX instructions have their own error reporting mechanism to indicate the success or failure of a given operation.

There are two flags used to signify the success or failure of a VM instruction.  The carry flag(CF) and the zero flag(ZF).

If both of these flags are clear after a VM instruction was executed then it succeeded.

If the carry flag is set then current VMCS pointer is invalid.

If the zero flag is set, it indicates that the VMCS pointer is valid but there is some other error specified in the VM-instruction error field (encoding 4400h).  Error numbers are listed in section 5.4 of the Intel SDM Volume 2B.

The following table represents the error numbers in the VM-instruction error field:

{| 
|-
! Error Number !! Description
|-
| 0x01 || VMCALL executed in VMX root operation
|-
| 0x02 || VMCLEAR with invalid physical address.
|-
| 0x03 || VMCLEAR with VMXON pointer.
|-
| 0x04 || VMLAUNCH with non-clear VMCS.
|-
| 0x05 || VMRESUME with non-launched VMCS.
|-
| 0x06 || VMRESUME with a corrupted VMCS. Indicates corruption of the current VMCS
|-
| 0x07 || VM entry with invalid VMX-control field(s).
|-
| 0x08 || VM entry with invalid host-state field(s).
|-
| 0x09 || VMPTRLD with invalid physical address.
|-
| 0x0A || VMPTRLD with VMXON pointer.
|-
| 0x0B || VMPTRLD with incorrect VMCS revision identifier.
|-
| 0x0C || VMREAD/VMWRITE from/to unsupported VMCS component.
|-
| 0x0D || VMWRITE to read-only VMCS component.
|-
| 0x0F || VMXON executed in VMX root operation.
|-
| 0x1A || VM entry with events blocked by MOV SS.
|}

## VMCS
The VMCS only has two relevant fields that can be accessed at this time, a 4-byte VMCS revision ID located at byte offset 0 and a 4-byte abort indicator field located at byte offset 4.  The rest of the VMCS is reserved for field data.

The revision field must be filled with the 32-bit revision Id stored in bits 0-31 of the IA32_VMX_BASIC MSR similar to the VMX region.

The abort field will simply contain a non-zero value if the VM abort occurs during a VMX exit, see section 23.7 in Intel's SDM 3B for error values.

A VMCS is loaded with the VMPTRLD instruction, which loads and activates a VMCS, and requires a 64-bit memory address as it's operand in the same format as VMXON/VMCLEAR.

```
 asm volatile ("vmptrld %0; ":: "m" (vmcsRegion64));
```

The structure of the VMCS is covered in detail in Chapter 20 of the Intel SDM volume 3B (see link below).  Field encodings for VMWRITE and VMREAD are covered in Appendix H of the same manual.
## Peripheral Emulation
### IO framework emulation
In x86, there are two kinds of IO channels: Port-Based IO(aka **PIO** ) and Memory-Mapped IO(aka **MMIO**). PIO has separate address space and special instructions to do IO jobs. while with MMIO, the device IO space is backed with the memory address space, you can use memory data move instructions to do IO jobs.
#### PIO emulation
with Intel VT-x, the hypervisor is able to determine whether the guest's IO instructions trap into vmx root mode by setting the primary processor based control bit 24. if this bit is set, all the guest's IO instructions will causes vm exits. otherwise, you have to setup the two IO bitmap regions to capture the vm exits you are interested in.

the IO causes vm exit with basic reason number as 30, you can retrieve the IO operation size, direction, port id and etc. for more please refer to the vmx_pio.c in reference pages.
#### MMIO emulation
MMIO emulation in x86 is a bit different: we are going to exploit EPT in order to capture MMIO events.
VMX provides two kinds of EPT involved vm exits: EPT violation and EPT misconfiguration.  In general, when guest is accessing memory which is not backed correctly(e.g. the memory is **writable** but **not readable**!), VMX results in vm exits with EPT misconfiguration. 
the hypervisor must do the following steps to do MMIO operation:
<pre>
 1). decode the memory move instruction to determine the memory involved instruction length, access size, direction, operations, registers index/immediates  and memory address,
 2). search the MMIO devices regions to see whether the address is backed with a DEVICE.
 3). store the result in destination  register if necessary.
 4). advance to next instruction by adding guest RIP with instruction length resolved in step 1.
</pre>

### Devices emulation examples
{| 
|-
! Device !! IO type !! Refference
|-
| Intel 8259 PIC || PIO ||https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_8259pic.c
|-
| Intel 8253 PIT || PIO ||https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_8253pit.c
|-
| Intel 8042 keyboard || PIO ||https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_keyboard.c
|-
| serial port controller || PIO ||https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_serial.c
|-
| 16-colors video controller || MMIO ||https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/device_video.c
|-
|}

## References
Intel's SDM 3B: http://www.intel.com/Assets/PDF/manual/253669.pdf 

Intel's SDM 2B: http://www.intel.com/Assets/PDF/manual/253667.pdf 

KVM's VMX.c (GPLv2): http://lxr.free-electrons.com/source/arch/x86/kvm/vmx.c 

BOCHS's VMX.c (LGPLv2): http://bochs.cvs.sourceforge.net/viewvc/bochs/bochs/cpu/vmx.cc 

PIO sub handler: https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/vmx_pio.c

Memory move instruction decode: https://github.com/chillancezen/ZeldaOS.x86_64/blob/master/vm_monitor/vmx_instruction_decoding.c



## Other examples
Vmx implementation in home made OS:
http://www.dumais.io/index.php?article=ac3267239dd3e34c061de6413203fb98

[Virtualization](Category:X86)(Category:X86 Virtualization)
