---
title: 'HPET'
source_page_id: '3378'
source_revision_id: '28151'
source_timestamp: '2023-07-10T14:17:48Z'
source_format: mediawiki
---

# HPET

:*This page is not meant as a full description of HPET, only as a lightweight introduction. If you need any information not covered by this article, consult the [HPET specification](http://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/software-developers-hpet-spec-1-0a.pdf).*

**HPET**, or High Precision Event Timer, is a piece of hardware designed by Intel and Microsoft to replace older [PIT](PIT) and [RTC](RTC). It consists of (usually 64-bit) main counter (which counts up), as well as from 3 to 32 32-bit or 64-bit wide comparators. HPET is programmed using memory mapped IO, and the base address of HPET can be found using [ACPI](ACPI).

## Detecting HPET using ACPI
The [HPET specification](http://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/software-developers-hpet-spec-1-0a.pdf) defines an ACPI 2.0 table that is to be used to detect presence, address and capabilities of HPET present in the system. If this table doesn't exist, you should assume there is no HPET and you should fall back to [PIT](PIT) or the [timer](APIC)(APIC timer).

```
struct address_structure
{
    uint8_t address_space_id;    // 0 - system memory, 1 - system I/O
    uint8_t register_bit_width;
    uint8_t register_bit_offset;
    uint8_t reserved;
    uint64_t address;
} __attribute__((packed));

struct description_table_header
{
    char signature[4];    // 'HPET' in case of HPET table
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    uint64_t oem_tableid;
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct hpet : public description_table_header
{
    uint8_t hardware_rev_id;
    uint8_t comparator_count:5;
    uint8_t counter_size:1;
    uint8_t reserved:1;
    uint8_t legacy_replacement:1;
    uint16_t pci_vendor_id;
    address_structure address;
    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} __attribute__((packed));
```

## HPET - timer vs comparators
There is just one up counting main counter in the timer, but interrupt generation is handled by *comparators*. There're from 3 to 32 comparators, and the exact amount is indicated by `comparator_count` field in the above `hpet` structure. Keep in mind you have to initialize both the main counter and all of the comparators. Also, the routing as well as **allowed** routing of comparator interrupts is independent, so you have to detect and set it up for each of them individually. More information on this procedure is provided further in the text.

## HPET operating modes
HPET offers two operating modes: one-shot (also called "non-periodic" by the specification) and periodic mode.

### One-shot mode
In non-periodic mode, the OS programs one of timer's comparator registers with value of main counter that is to trigger an interrupt. If the timer is set to 32 bit mode, it will also generate an interrupt when the counter wraps around. The comparator register's value is never written by the hardware, you are free to write to it and read from it at any time, therefore you can change at what value in the main counter the interrupt will be generated.

Every comparator in HPET **must support** non-periodic mode.

### Periodic mode
Periodic mode is more tricky than non-periodic mode. For periodic mode, similarly to one-shot mode, you write a value at which an interrupt shall be generated to the comparator register. When the interrupt is generated, however, the hardware will **increase the value in comparator register by the last value written to it**! This is a consequence of HPET's main counter being up-counting.

So, if the main counter's value is 12345 when we set the timer up, and we write 12456 to comparator (i.e. the interrupt should trigger 111 time units from now), when the interrupt triggers, 12456 will be *added* to the comparator register, so it will become 24912, which is 12456 time units from the first interrupt. There are two techniques to deal with this problem; they will both be described in later part of the article.

Comparators are **NOT required** to support this mode; you must detect this capability when initializing a comparator. More information on this is provided further in the article.

## Interrupt routing
HPET supports three interrupt mapping options: "legacy replacement" option, standard option, and FSB option. 

### "Legacy replacement" mapping
In this mapping, HPET's timer (comparator) #0 replaces PIT interrupts, whereas timer #1 replaces RTC's interrupts (in other words, PIC and RTC will no longer cause interrupts). While the HPET specification provides the following table describing the routing of timers #0 and #1 in this mapping, it is recommended to at least check the routing of IRQ0 and IRQ8 to I/O APIC in ACPI tables.

{| 
!Timer
!PIC mapping
!IOAPIC mapping
|-
|0
|IRQ0
|IRQ2
|-
|1
|IRQ8
|IRQ8
|-
|N
|As per IRQ Routing Field
|As per IRQ Routing Field
|}

### Standard mapping
In standard mapping, each timer has its own interrupt routing control. Allowed I/O APIC inputs are found by reading timer's capabilities register.

### FSB mapping
This mapping is almost identical to PCI's Message Signaled Interrupts. The "Timer N FSB Interrupt Route Register" which defines how FSB interrupts are configured can be found in the specification. FSB interrupts are enabled using "Tn_FSB_EN_CNF" field in timer's configuration register. This mapping mode will not be further discussed in this article.

## HPET registers
The following table and field descriptions can also be found in the specification. "Offset" means offset from the address defined in `address` field of `hpet` struct. The following table skips reserved registers defined in the specification.

{| 
!Offset
!Register
!Type
|-
|0x000 - 0x007
|General Capabilities and ID Register
|Read only
|-
|0x010 - 0x017
|General Configuration Register
|Read/write
|-
|0x020 - 0x027
|General Interrupt Status Register
|Read/write clear
|-
|0x0F0 - 0x0F7
|Main Counter Value Register
|Read/write
|-
|(0x100 + 0x20 * N) - (0x107 + 0x20 * N)
|Timer N Configuration and Capability Register
|Read/write
|-
|(0x108 + 0x20 * N) - (0x10F + 0x20 * N)
|Timer N Comparator Value Register
|Read/write
|-
|(0x110 + 0x20 * N) - (0x117 + 0x20 * N)
|Timer N FSB Interrupt Route Register
|Read/write
|}

### General Capabilities and ID Register
{| 
!Bits
!Name
!Description
|-
|63:32
|COUNTER_CLK_PERIOD
|Main counter tick period in femtoseconds (10^-15 seconds). Must not be zero, must be less or equal to 0x05F5E100, or 100 nanoseconds.
|-
|31-16
|VENDOR_ID
|This field should be interpreted similarly to PCI's vendor ID.
|-
|15
|LEG_RT_CAP
|If this bit is 1, HPET is capable of using "legacy replacement" mapping.
|-
|14
|Reserved
| -
|-
|13
|COUNT_SIZE_CAP
|If this bit is 1, HPET main counter is capable of operating in 64 bit mode.
|-
|12:8
|NUM_TIM_CAP
|The amount of timers - 1.
|-
|7:0
|REV_ID
|Indicates which revision of the function is implemented; must not be 0.
|}

### General Configuration Register
{| 
!Bits
!Name
!Description
|-
|63:2
|Reserved
| -
|-
|1
|LEG_RT_CNF
|0 - "legacy replacement" mapping is disabled
1 - "legacy replacement" mapping is enabled
|-
|0
|ENABLE_CNF
|Overall enable.
0 - main counter is halted, timer interrupts are disabled

1 - main counter is running, timer interrupts are allowed if enabled
|}

### General Interrupt Status Register
{| 
!Bits
!Name
!Description
|-
|63:32
|Reserved
| -
|-
|n
|Tn_INT_STS
|The functionality is dependent of whether edge or level-triggered mode is used for timer #n.

**For level-triggered:** the default value is 0. When a corresponding timer interrupt is active, this bit is set. If it is set, software can clear it by writing 1 to this bit. Writes of 0 have no effect.

**For edge-triggered:** this bit should be ignored. It is always set to 0.
|}

### Main Counter Value Register
Bits 63:0 of this register are called MAIN_COUNTER_VAL. Writes to this register should only be done when the counter is halted (ENABLE_CNF = 0). Reads will return current value of the main counter. 32-bit counters will always return 0 for the upper 32 bits. If 32 bit reads are performed on 64 bit counter, consult 2.4.7 in the specification for instructions how to do it safely. It is recommended to use 32 bit counter when on 32-bit only software.

### Timer N Configuration and Capability Register
{| 
!Bits
!Name
!Description
|-
|63:32
|Tn_INT_ROUTE_CAP
|Timer n Interrupt Routing Capability. If bit X is set in this field, it means that this timer can be mapped to IRQX line of I/O APIC.
|-
|31:16
|Reserved
| -
|-
|15
|Tn_FSB_INT_DEL_CAP
|If this read-only bit is 1, this timer supports FSB interrupt mapping.
|-
|14
|Tn_FSB_EN_CNF
|If this bit is set to 1, this timer will use FSB interrupt mapping.
|-
|13:9
|Tn_INT_ROUTE_CNF
|This field indicates I/O APIC routing. Allowed values can be determined using Tn_INT_ROUTE_CAP. If an illegal value is written, then value read back from this field will not match the written value.
|-
|8
|Tn_32MODE_CNF
|For 64-bit timer, if this field is set, the timer will be forced to work in 32-bit mode. Otherwise it has no effect.
|-
|7
|Reserved
| -
|-
|6
|Tn_VAL_SET_CNF
|This field is used to allow software to directly set periodic timer's accumulator. Detailed explanation is provided further in the article.
|-
|5
|Tn_SIZE_CAP
|If this read-only bit is set to 1, the size of the timer is 64-bit. Otherwise, it's 32-bit.
|-
|4
|Tn_PER_INT_CAP
|If this read-only bit is set to 1, this timer supports periodic mode.
|-
|3
|Tn_TYPE_CNF
|If Tn_PER_INT_CAP is 1, then writing 1 to this field enables periodic timer and writing 0 enables non-periodic mode. Otherwise, this bit will be ignored and reading it will always return 0.
|-
|2
|Tn_INT_ENB_CNF
|Setting this bit to 1 enables triggering of interrupts. Even if this bit is 0, this timer will still set Tn_INT_STS.
|-
|1
|Tn_INT_TYPE_CNF
|0 - this timer generates edge-triggered interrupts.
1 - this timer generates level-triggered interrupts. When the interrupt is generated, Tn_INT_STS is set. If another interrupt occurs before that bit is cleared, the interrupt will remain active.
|-
|0
|Reserved
| -
|}

### Timer N Comparator Value Register
Bits 63:0 (or 31:0, if the timer operates in 32 bit mode) are used to compare with main counter to check if an interrupt should be generated.

## Initialization
The following is the procedure you need to perform to initialize main counter and comparators in order to receive interrupts.

General initialization:

1. Find HPET base address in 'HPET' ACPI table.

2. Calculate HPET frequency (f = 10^15 / period).

3. Save minimal tick (either from ACPI table or configuration register).

4. Initialize comparators.

5. Set ENABLE_CNF bit.

Timer N initialization:

1. Determine if timer N is periodic capable, save that information to avoid re-reading it every time.

2. Determine allowed interrupt routing for current timer and allocate an interrupt for it.

I am enabling the timers only when I actually use them, so there's no "real" initialization of comparators here.

Keep in mind that allowed interrupt routing **may be insane**. Namely, you probably want to use some of ISA interrupts - or, at very least, be able to use them at one point unambiguously. Last time I checked VirtualBox allowed mappings for HPET, it allowed every timer to be routed to *any* of 32 I/O APIC inputs present on the system. Knowing how buggy hardware can be, I wouldn't be too surprised if there exists a PC with HPET claiming that input #31 is allowed, when there are only 24 I/O APIC inputs. Be aware of this when choosing interrupt routing for timers.

## Using timers
### One-shot mode
To enable one-shot mode:

```
// "time" is time in femtoseconds from now to interrupt
if (time < COUNTER_CLK_PERIOD)
{
    time = adjust_time(time);
}

write_register_64(timer_configuration(n), (ioapic_input << 9) | (1 << 2));
write_register_64(timer_comparator(n), read_register(main_counter) + time);
```

I hope the above code is obvious. If it's not, please analyze the meaning of specific fields in registers used above.

### Periodic mode
To enable periodic mode:

```
// "time" is time in femtoseconds from now to interrupt
if (time < COUNTER_CLK_PERIOD)
{
    time = adjust_time(time);
}

write_register_64(timer_configuration(n), (ioapic_input << 9) | (1 << 2) | (1 << 3) | (1 << 6));
write_register_64(timer_comparator(n), read_register(main_counter) + time);
write_register_64(timer_comparator(n), time);
```

This snippet requires some more comments.

Bit 2 is the same as above, Interrupt Enable. Bit 3 is also quite straightforward - 1 means periodic timer. But we've also set bit 6. Why?

Let's take a look at quote from the HPET specification:

<blockquote>Timer n Value Set: [...] Software uses this read/write bit only for timers that have been set to periodic mode. By writing this bit to a 1, the software is then allowed to directly set a periodic timer’s accumulator.</blockquote>
<blockquote>Software does NOT have to write this bit back to 0 (it automatically clears). Software should not write a 1 to this bit position if the timer is set to non-periodic mode.</blockquote>

This means that next write to timer N comparator register will have the usual meaning, while *second* next write will write directly to the accumulator. I believe that the wording could've been much better.

## See also
- [IOAPIC](IOAPIC)
- [APIC](APIC)
- [timer](APIC)(APIC timer)
- [PIT](PIT)
- [RTC](RTC)

## External links
- [HPET article on Wikipedia](http://en.wikipedia.org/wiki/High_Precision_Event_Timer)
- [Intel's HPET Specification v1.0a](http://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/software-developers-hpet-spec-1-0a.pdf)


[Devices](Category:Common)(Category:Common Devices)
[Category:Interrupts](Category:Interrupts)
[Category:Timers](Category:Timers)
