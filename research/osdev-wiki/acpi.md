---
title: 'ACPI'
source_page_id: '1438'
source_revision_id: '30223'
source_timestamp: '2026-03-16T14:50:17Z'
source_format: mediawiki
---

# ACPI

> **ACPI tables:** [RSDP](RSDP), [FADT](FADT), [MADT](MADT), [MCFG](MCFG), [RSDT](RSDT), [XSDT](XSDT), [DSDT](DSDT), [SSDT](SSDT).

ACPI (Advanced Configuration and Power Interface) is a [management](power)(:Category:Power management) and configuration standard for the PC, developed by Intel, Microsoft and Toshiba. ACPI allows the operating system to discover the hardware configuration of the computer and control the amount of power each device is given (allowing it to put certain devices on standby or power-off for example). It is also used to control and/or check thermal zones (temperature sensors, fan speeds, etc), battery levels, PCI IRQ routing, CPUs, NUMA domains and many other things.

## Implementing ACPI
Information about ACPI is stored in the [BIOS](BIOS) memory (for those systems that support ACPI of course).

There are 2 main parts to ACPI. The first part is the tables used by the OS for configuration during boot (these include things like how many CPUs, APIC details, NUMA memory ranges, etc). The second part is the run time ACPI environment, which consists of [AML](AML) code (a platform independent OOP language that comes from the BIOS and devices) and the ACPI SMM (System Management Mode) code.

To begin using ACPI, the operating system must look for the RSDP (Root System Description Pointer). This is covered in [RSDP](RSDP) because it is too verbose to put here.

If the RSDP is found and the verification is valid, it contains a pointer to the [RSDT](RSDT) (Root System Description Table) and for newer versions of ACPI (ACPI 2.0 and later) there is an additional XSDT (eXtended System Description Table). Both the RSDT and the XSDT contain pointers to other tables. The only real difference between the RSDT and the XSDT is that the XSDT contains 64 bit pointer instead of 32 bit pointers.

For the run time part of ACPI the main table to detect is the FADT (Fixed ACPI Description Table) as this contains information needed to enable ACPI.

You have two possibilities of using the ACPI. You can write your own ACPI table reader and AML interpreter. Or you can integrate [ACPICA](ACPICA) in your OS.

## Switching to ACPI Mode
On some PCs, this is already done for you if...

- the SMI command field in the FADT is 0
- the ACPI enable and ACPI disable fields in the FADT are both 0
- bit 0 (value 1) of the PM1a control block I/O port is set

Otherwise, write the value of the ACPI Enable field into the register number pointed to by the smi command field, like so:

```
outb(fadt->smi_command,fadt->acpi_enable);
```
Linux waits 3 seconds for the hardware to change modes.
Then poll the PM1a control block until bit 0 (value 1) sets. When this bit is set, it means power management events are generating SCIs and not SMIs, which means your OS has to handle the events and the System Management BIOS isn't doing anything for you. The SCI is an IRQ that the FADT tells you about.
```
while (inw(fadt->pm1a_control_block) & 1 == 0);
```

## See Also
### Articles
- [Shutdown](Shutdown)
- [ACPICA](ACPICA)
- [UACPI](UACPI)
- [RSDP](RSDP)
- [RSDT](RSDT)

### Forum Links
- [Shutdown code with good explanation in C](ACPI)(Topic:16990)

### External Links
- [ACPI Specification](https://uefi.org/sites/default/files/resources/ACPI_6_3_final_Jan30.pdf)

[Category:ACPI](Category:ACPI)
[Configuration and Power Interface](de:Advanced)(de:Advanced Configuration and Power Interface)
