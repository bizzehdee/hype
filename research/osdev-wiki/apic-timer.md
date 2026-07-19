---
title: 'APIC Timer'
source_page_id: '3127'
source_revision_id: '28146'
source_timestamp: '2023-07-10T14:11:19Z'
source_format: mediawiki
---

# APIC Timer

The great benefit of the Local [APIC](APIC) timer is that it is hardwired to each CPU core, unlike the [Interval Timer](Programmable)(PIT) which is a separate circuit. Because of this, there is no need for any resource management, which makes things easier. The downside is that it's oscillating at (one of) the CPU's frequencies, which varies from machine to machine, while the PIT uses a standard frequency (1,193,182 Hz). To make use of it, you have to know how many interrupts/sec it's capable of.

## APIC Timer Modes
The timer has 2 or 3 modes. The first 2 modes (periodic and one-shot) are supported by all local APICs. The third mode (TSC-Deadline mode) is an extension that is only supported on recent CPUs.

### Periodic Mode
For periodic mode, software sets a "initial count" and the local APIC uses it for a "current count". The local APIC decrements the current count until it reaches zero, then generates a timer IRQ and resets the current count to the initial count and begins decrementing the current count again. In this way the local APIC generates IRQs at a fixed rate depending on the initial count. The current count is decremented at a rate that depends on the CPU's external frequency ("bus frequency") divided by the value in the local APIC's "Divide Configuration Register".

For example, for a 2.4 GHz CPU with an external/bus frequency of 800 MHz, if the Divide Configuration Register is set to "divide by 4" and the initial count is set to 123456; then the local APIC timer would decrement the count at a rate of 200 MHz and generate a timer IRQ every 617.28 us, giving a rate of IRQs of 1620.01 Hz.

### One-Shot Mode
For one-shot mode, the local APIC decrements the current count (and generates a timer IRQ when the count reaches zero) in the same way as in periodic mode; however it doesn't reset the current count to the initial count when the current count reaches zero. Instead, software has to set a new count each time if it wants more timer IRQs.

The advantage of this mode is that software can have precise control over when timer IRQs occur. For example, during task switches an OS could set the count to a value that depends on the new task's priority (so that some tasks run for a small amount of time and other tasks run for a larger amount of time), and there wouldn't be any unwanted IRQs. Some OSs use this approach to implement a generic high precision timer service, where the local APIC count is set to a value that depends on which event will happen soonest. For example, if the currently running task switch should be pre-empted in 1234 nanoseconds, a sleeping task needs to wake up in 333 nanoseconds and alarm signal has to be sent in 44444 nanoseconds, then the timer's count would be set to 333 nanoseconds (the earliest delay needed) and when the the timer IRQ occurs the OS knows that there's 901 nanoseconds remaining before the current task should be pre-empted and 441111 nanoseconds until the alarm signal needs to be sent (and would set the count to 901 nanoseconds for the next timer IRQ).

The disadvantages are that it's harder to track real-time with one-shot mode and special care needs to be taken to avoid race conditions; especially if a new count is set before the old count expires.

### TSC-Deadline mode
TSC-Deadline mode is very different to the other 2 modes. Instead of using the CPU's external/bus frequency to decrement a count, software sets a "deadline" and the local APIC generates a timer IRQ when the value of the CPU's time stamp counter is greater than or equal to the deadline.

Despite these differences, software would/could use it in the same way that one-shot mode would be used. The advantages (compared to one-shot mode) are that you get higher precision (because the CPU's time stamp counter runs at the CPU's (nominal) internal frequency rather than the CPU's external/bus frequency), and it's easier to avoid/handle race conditions.

## Enabling APIC Timer
Before enabling the local APIC timer, you should setup the rest of the local APIC. This includes:

- Determine the local APIC's physical address (via. ACPI tables or MultiProcessor Specification tables)
- Specify a spurious interrupt and software enable the APIC
- Make sure the TPR (Task Priority Register) is set (so it won't block/postpone lower priority IRQs)

Once that's done:

- Set the local APIC timer's divide configuration register
- Configure the local APIC timer's interrupt vector and unmask the timer's IRQ
- Set the local APIC timer's initial count

Note: It's recommended to follow the order given above (especially setting the local APIC timer's initial count last). Doing things in a different order (e.g. setting the initial count, then enabling the timer) can lead to problems on some (real or virtual) machines (e.g. everything seems right and counter is decreasing, but IRQ is never sent).

## Initializing
*Please note that this is the recommended way of determining the frequency of the APIC timer.*

**NOTE:** According to Intel's documentation for IA-32 (x86) and Intel 64 (x86_64), APIC timer's frequency is equal to the bus' frequency **OR** the core crystal's frequency divided by the chosen frequency divider. The bus' and the core crystal's frequency can be found in the [CPUID](CPUID) functions [0x15](https://sandpile.org/x86/cpuid.htm#level_0000_0015h) and [0x16](https://sandpile.org/x86/cpuid.htm#level_0000_0016h), respectively. Through CPUID.0x15 can also be determined the TSC frequency. The frequency of the APIC timer depends on whether the system is using local APIC or *discrete* APIC ([82489DX](https://en.wikipedia.org/wiki/Advanced_Programmable_Interrupt_Controller)). When the local APIC is built in core's crystal, the APIC timer is using core's frequency. Otherwise, it is using the bus frequency.


There are several ways to do this, but all of them use a different, CPU bus frequency independent clock source to do that. Examples: [Time Clock](Real)(RTC), [Counter](TimeStamp)(TSC), PIT or even polling [registers](CMOS)(CMOS#Getting_Current_Date_and_Time_from_RTC). In this tutorial we will use the good old PIT, as it's the easiest. Steps that need to be done:
- Reset APIC to a well known state
- Enable APIC timer
- Reset APIC timer counter
- Wait a specific amount of time measured by a different clock
- Get number of ticks from APIC timer counter
- Adjust it to a second
- Divide it by the quantum of your choice (results X)
- Make the APIC timer fire an interrupt at every X ticks

The APIC timer can be set to make a tick (decrease counter) at a given frequency, which is called "divide value". This means you have to multiply APIC timer counter ticks by this divide value to get the true CPU bus frequency. You could use a value of 1 (ticks on every bus cycle) up to 128 (ticks on every 128th cycle). See Intel manual vol3A Chapter 9.5.4 for details. Note that according to my tests, Bochs seems not to handle divide value of 1 properly, so I will use 16.

### Prerequisites
Before we start, let's define some constant and functions.
```
apic		= the linear address where you have mapped the APIC registers

APIC_APICID	= 20h
APIC_APICVER	= 30h
APIC_TASKPRIOR	= 80h
APIC_EOI	= 0B0h
APIC_LDR	= 0D0h
APIC_DFR	= 0E0h
APIC_SPURIOUS	= 0F0h
APIC_ESR	= 280h
APIC_ICRL	= 300h
APIC_ICRH	= 310h
APIC_LVT_TMR	= 320h
APIC_LVT_PERF	= 340h
APIC_LVT_LINT0	= 350h
APIC_LVT_LINT1	= 360h
APIC_LVT_ERR	= 370h
APIC_TMRINITCNT	= 380h
APIC_TMRCURRCNT	= 390h
APIC_TMRDIV	= 3E0h
APIC_LAST	= 38Fh
APIC_DISABLE	= 10000h
APIC_SW_ENABLE	= 100h
APIC_CPUFOCUS	= 200h
APIC_NMI	= (4<<8)
TMR_PERIODIC	= 20000h
TMR_BASEDIV	= (1<<20)

		;Interrupt Service Routines
isr_dummytmr:	mov			dword [apic+APIC_EOI], 0
		iret
isr_spurious:	iret
		;function to set a specific interrupt gate in IDT
		;al=interrupt
		;ebx=isr entry point
writegate:	...
		ret
```
I will also assume that you have a working [IDT](IDT), and you have a function to write a gate for a specific interrupt: writegate(intnumber,israddress).
Furthermore, to make things simple, I'll assume that you did not change the default interrupt mapping found in almost every tutorial:
- interrupt 0-31: exceptions
- interrupt 32: timer, IRQ0
- interrupt 39: spurious irq, IRQ7
If you've already changed this, modify accordingly.


### Example code in ASM
Here's a possible way to initialize APIC timer in fasm syntax assembly:
```
		;you should read MSR, get APIC base and map to "apic"
		;you should have used lidt properly

		;set up isrs
		mov			al, 32
		mov			ebx, isr_dummytmr
		call			writegate
		mov			al, 39
		mov			ebx, isr_spurious
		call			writegate

		;initialize LAPIC to a well known state
		mov			dword [apic+APIC_DFR], 0FFFFFFFFh
		mov			eax, dword [apic+APIC_LDR]
		and			eax, 00FFFFFFh
		or			al, 1
		mov			dword [apic+APIC_LDR], eax
		mov			dword [apic+APIC_LVT_TMR], APIC_DISABLE
		mov			dword [apic+APIC_LVT_PERF], APIC_NMI
		mov			dword [apic+APIC_LVT_LINT0], APIC_DISABLE
		mov			dword [apic+APIC_LVT_LINT1], APIC_DISABLE
		mov			dword [apic+APIC_TASKPRIOR], 0
		;okay, now we can enable APIC
		;global enable
		mov			ecx, 1bh
		rdmsr
		bts			eax, 11
		wrmsr
		;software enable, map spurious interrupt to dummy isr
		mov			dword [apic+APIC_SPURIOUS], 39+APIC_SW_ENABLE
		;map APIC timer to an interrupt, and by that enable it in one-shot mode
		mov			dword [apic+APIC_LVT_TMR], 32
		;set up divide value to 16
		mov			dword [apic+APIC_TMRDIV], 03h

		;ebx=0xFFFFFFFF;
		xor			ebx, ebx
		dec			ebx

		;initialize PIT Ch 2 in one-shot mode
		;waiting 1 sec could slow down boot time considerably,
		;so we'll wait 1/100 sec, and multiply the counted ticks
		mov			dx, 61h
		in			al, dx
		and			al, 0fdh
		or			al, 1
		out			dx, al
		mov			al, 10110010b
		out			43h, al
		;1193180/100 Hz = 11931 = 2e9bh
		mov			al, 9bh		;LSB
		out			42h, al
		in			al, 60h		;short delay
		mov			al, 2eh		;MSB
		out			42h, al
		;reset PIT one-shot counter (start counting)
		in			al, dx
		and			al, 0feh
		out			dx, al		;gate low
		or			al, 1
		out			dx, al		;gate high
		;reset APIC timer (set counter to -1)
		mov			dword [apic+APIC_TMRINITCNT], ebx
		;now wait until PIT counter reaches zero
@@:		in			al, dx
		and			al, 20h
		jz			@b
		;stop APIC timer
		mov			dword [apic+APIC_LVT_TMR], APIC_DISABLE
		;now do the math...
		xor			eax, eax
		xor			ebx, ebx
		dec			eax
		;get current counter value
		mov			ebx, dword [apic+APIC_TMRCURRCNT]
		;it is counted down from -1, make it positive
		sub			eax, ebx
		inc			eax
		;we used divide value different than 1, so now we have to multiply the result by 16
		shl			eax, 4		;*16
		xor			edx, edx
		;moreover, PIT did not wait a whole sec, only a fraction, so multiply by that too
		mov			ebx, 100	;*PITHz
		mul			ebx
	;-----edx:eax now holds the CPU bus frequency-----
		;now calculate timer counter value of your choice
		;this means that tasks will be preempted 1000 times in a second. 100 is popular too.
		mov			ebx, 1000
		xor			edx, edx
		div			ebx
		;again, we did not use divide value of 1
		shr			eax, 4		;/16
		;sanity check, min 16
		cmp			eax, 010h
		jae			@f
		mov			eax, 010h
		;now eax holds appropriate number of ticks, use it as APIC timer counter initializer
@@:		mov			dword [apic+APIC_TMRINITCNT], eax
		;finally re-enable timer in periodic mode
		mov			dword [apic+APIC_LVT_TMR], 32 or TMR_PERIODIC
		;setting divide value register again not needed by the manuals
		;although I have found buggy hardware that required it
		mov			dword [apic+APIC_TMRDIV], 03h
```

### Example code in C
This code is an example of how to initialize the APIC timer so that it ticks every 10 milliseconds. This is done by letting the APIC timer run, waiting for 10ms using the PIT and then getting the number of ticks that were done from the APIC timer. It assumes that you have functions to "write"/"read" the APIC's registers and "pit_prepare_sleep"/"pit_perform_sleep" to perform an as accurate as possible measurement of the timer's frequency.
```
void apic_start_timer() {
        // Tell APIC timer to use divider 16
        write(APIC_REGISTER_TIMER_DIV, 0x3);
 
        // Prepare the PIT to sleep for 10ms (10000µs)
        pit_prepare_sleep(10000);
 
        // Set APIC init counter to -1
        write(APIC_REGISTER_TIMER_INITCNT, 0xFFFFFFFF);
 
        // Perform PIT-supported sleep
        pit_perform_sleep();
 
        // Stop the APIC timer
        write(APIC_REGISTER_LVT_TIMER, APIC_LVT_INT_MASKED);
 
        // Now we know how often the APIC timer has ticked in 10ms
        uint32_t ticksIn10ms = 0xFFFFFFFF - read(APIC_REGISTER_TIMER_CURRCNT);
 
        // Start timer as periodic on IRQ 0, divider 16, with the number of ticks we counted
        write(APIC_REGISTER_LVT_TIMER, 32 | APIC_LVT_TIMER_MODE_PERIODIC);
        write(APIC_REGISTER_TIMER_DIV, 0x3);
        write(APIC_REGISTER_TIMER_INITCNT, ticksIn10ms);
}
```

## See also
### Articles
- [APIC](APIC)
### Threads
- [APIC timer](http://www.osdev.org/phpBB2/viewtopic.php?t=10686)

### External Links
- [Volume 3A:System Programming Guide, Part 1,manuals has a chapter on the APIC](http://www.intel.com/products/processor/manuals/)
- [Advanced Programmable Interrupt Controller by Mike Rieker](http://www.osdever.net/tutorials/view/advanced-programming-interrupt-controller)

[Category:Interrupts](Category:Interrupts)
[Category:Timers](Category:Timers)
