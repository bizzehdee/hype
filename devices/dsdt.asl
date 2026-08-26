/*
 * M4-6b2: hype's guest DSDT source.
 *
 * The DSDT was previously a header-only placeholder (no AML), which is fine
 * in legacy-PIC mode (the guest routes device IRQs via the 8259 + the PCI
 * Interrupt Line register). But once hype delivers a real MADT (SVM-STRIO)
 * the ACPI-mode Linux kernel routes PCI device interrupts through the ACPI
 * _PRT (PCI routing table) instead -- with none present, the AHCI driver
 * failed to probe: "can't derive routing for PCI INT A / no GSI / -22".
 *
 * This minimal DSDT declares the PCI host bridge (segment 0, bus 0) with a
 * _PRT that routes the AHCI function's interrupt pins to I/O APIC GSIs. It is
 * compiled with iasl (see devices/README or the Makefile note) and the AML
 * *body* (everything after the 36-byte SDT header iasl emits) is embedded as
 * devices/dsdt_aml.h; hype fills its own SDT header at runtime and the fw_cfg
 * table-loader recomputes the checksum, so only the body is used from here.
 *
 * IMPORTANT COUPLING: the GSIs below (AHCI INTA -> GSI 16) must match what
 * hype actually raises on the I/O APIC for the AHCI line -- see
 * HYPE_FW_1_AHCI_GSI in boot/main.c. Change both together.
 *
 * Regenerate:  iasl -tc devices/dsdt.asl   (then extract the body -> dsdt_aml.h;
 *              tools/gen-dsdt-aml.sh automates it)
 */
DefinitionBlock ("", "DSDT", 2, "HYPE  ", "HYPEDSDT", 0x00000001)
{
    /* M8-6: S5 (soft-off) sleep package. On a hardware-reduced-ACPI guest the OS
     * writes (_S5[0] << 2) | SLP_EN to the FADT SLEEP_CONTROL register to power
     * off; hype detects that write and transitions the VM to OFF. Only S5 is
     * declared (no S1-S4), so the sole SLEEP_CONTROL write is an orderly
     * power-off. SLP_TYPa = 5 here must match boot/main.c's detect. */
    Name (\_S5, Package (0x04)
    {
        0x05,  /* SLP_TYPa */
        0x05,  /* SLP_TYPb */
        0x00,
        0x00
    })

    Scope (\_SB)
    {
        /*
         * #436: the processor object. Every real x86 platform declares its
         * CPUs in the ACPI namespace -- the MADT lists their local APICs, and
         * the namespace gives each one an object an OS can attach to. hype
         * declared none, so a guest walking the namespace found a machine with
         * no processors at all while the MADT said otherwise: the same
         * describe-a-different-machine defect as the MCFG bus range and the
         * hardware-reduced flag.
         *
         * _UID 0 matches the ACPI processor UID hype writes into the MADT's
         * first Local APIC entry. hype gives each guest exactly one vCPU
         * today, so exactly one object is declared here; when a guest can have
         * several, these must be generated to match cpu_count (as the MADT
         * entries already are) rather than fixed in this static AML.
         */
        Device (CP00)
        {
            Name (_HID, "ACPI0007")  /* Processor Device */
            Name (_UID, 0x00)
        }

        /*
         * #436: the HPET, at the fixed 0xFED00000 the HPET table names. A
         * guest that walks the namespace rather than reading the table finds
         * the same block at the same address, which is the point: the two
         * descriptions of one piece of hardware must agree.
         */
        Device (HPET)
        {
            Name (_HID, EisaId ("PNP0103"))
            Name (_UID, 0x00)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadOnly, 0xFED00000, 0x00000400)
            })
        }

        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))  /* PCI Express root bridge */
            Name (_CID, EisaId ("PNP0A03"))  /* legacy PCI compatible */
            Name (_SEG, 0x00)
            Name (_BBN, 0x00)                /* base bus number 0 */
            Name (_UID, 0x00)

            /* {PCI address (dev<<16 | 0xFFFF = all functions), INTx pin (0=A..3=D),
             *  source (0 = routed directly to a global interrupt), source_index (GSI)}.
             * AHCI is device 2; only INTA is used by the model, but all four pins
             * are mapped for completeness. GSIs 16-19 sit above the 16 ISA lines.
             * M5-7 (#196): virtio-blk is device 3, INTA -> GSI 20 (clear of the
             * dev-2 block); must match HYPE_FW_1_VIRTIO_GSI in boot/main.c. */
            Name (_PRT, Package ()
            {
                Package () { 0x0002FFFF, 0x00, 0x00, 0x10 },  /* dev 2 INTA -> GSI 16 */
                Package () { 0x0002FFFF, 0x01, 0x00, 0x11 },  /* dev 2 INTB -> GSI 17 */
                Package () { 0x0002FFFF, 0x02, 0x00, 0x12 },  /* dev 2 INTC -> GSI 18 */
                Package () { 0x0002FFFF, 0x03, 0x00, 0x13 },  /* dev 2 INTD -> GSI 19 */
                Package () { 0x0003FFFF, 0x00, 0x00, 0x14 },  /* dev 3 INTA -> GSI 20 (virtio-blk) */
                /* #519: slot 0's NVMe front-end is device 5, and it had no entry at all -- so its
                 * completion interrupt was not merely unraised, it was unroutable. It shares GSI 20
                 * with virtio-blk because the two are alternatives for the same slot (#333 selects
                 * one front-end) and can never both be present, which is the assumption
                 * fw_1_slot_gsi() already encodes. */
                Package () { 0x0005FFFF, 0x00, 0x00, 0x14 },  /* dev 5 INTA -> GSI 20 (NVMe slot 0) */
                /* #81: virtio-net is device 4, and it shares GSI 20 rather than taking a new pin
                 * because there is no new pin -- the 24-entry IO-APIC is fully allocated (16-19
                 * dev 2, 20 here, 21 dev 31, 22 and 23 the extra disk slots). Sharing a
                 * level-triggered PCI interrupt is ordinary; what it requires is that hype treat
                 * the line as the OR of its devices, which fw-1's dispatch now does in one place.
                 * Present unconditionally, like the disk-slot entries: a _PRT entry for an absent
                 * device is inert, and a conditional table would need a per-config DSDT. */
                Package () { 0x0004FFFF, 0x00, 0x00, 0x14 },  /* dev 4 INTA -> GSI 20 (virtio-net) */
                /* #440: the ICH9 SATA function is 00:1f.2. _PRT keys on
                 * device/pin, so function 2 routes via dev31 INTA. GSI 21 keeps
                 * it OFF the CD controller's line (dev 2 INTA -> GSI 16): hype's
                 * per-device deassert would otherwise drop a still-pending
                 * interrupt from the other HBA sharing the pin. Must match
                 * HYPE_FW_1_ATA_GSI in boot/main.c. */
                Package () { 0x001FFFFF, 0x00, 0x00, 0x15 },  /* dev 31 INTA -> GSI 21 (ICH9 SATA) */
                /* #329: extra disk slots. Slot 0 keeps the per-bus legacy devices above; slots 1
                 * and 2 are devices 6 and 7 whatever their bus, each with its own GSI -- these are
                 * the LAST two free pins on the 24-pin IO-APIC, which is what caps a VM at 3 disks
                 * (HYPE_FW_1_MAX_DISKS in boot/main.c). Present unconditionally: a _PRT entry for
                 * an absent device is inert, and a conditional table would need a per-config DSDT. */
                Package () { 0x0006FFFF, 0x00, 0x00, 0x16 },  /* dev 6 INTA -> GSI 22 (disk slot 1) */
                Package () { 0x0007FFFF, 0x00, 0x00, 0x17 },  /* dev 7 INTA -> GSI 23 (disk slot 2) */
                /*
                 * #727: extra optical drives. `cdroms =` attaches any number of CD-ROMs, and
                 * hype_ahci_t models exactly ONE port, so each drive is its own AHCI HBA on its
                 * own PCI device -- devices 10 upward (2/3/4/5/6/7/8/9/31 are spoken for).
                 *
                 * They all share GSI 16 with the dev-2 CD controller, because there is no free
                 * pin: the 24-entry IO-APIC is fully allocated (see the disk-slot note above,
                 * which is why disks cap at 3). Sharing a level-triggered PCI line is ordinary;
                 * what it requires is that hype treat the line as the OR of its devices, which
                 * fw_1_extra_optical_irq_pending() in boot/main.c does. #440 is the warning that
                 * makes that mandatory rather than tidy: a per-device deassert on a shared pin
                 * drops a sibling HBA's still-pending interrupt, which is exactly why #440 moved
                 * the SATA function to its own GSI instead.
                 *
                 * Listed unconditionally and beyond what HYPE_FW_1_MAX_OPTICAL currently
                 * presents, on the same reasoning the disk-slot and virtio-net entries record: a
                 * _PRT entry for an absent device is inert, and a conditional table would need a
                 * per-config DSDT. Raising the cap in boot/main.c therefore needs no ACPI change
                 * and no regenerated AML.
                 */
                Package () { 0x000AFFFF, 0x00, 0x00, 0x10 },  /* dev 10 INTA -> GSI 16 (optical 1) */
                Package () { 0x000BFFFF, 0x00, 0x00, 0x10 },  /* dev 11 INTA -> GSI 16 (optical 2) */
                Package () { 0x000CFFFF, 0x00, 0x00, 0x10 },  /* dev 12 INTA -> GSI 16 (optical 3) */
                Package () { 0x000DFFFF, 0x00, 0x00, 0x10 },  /* dev 13 INTA -> GSI 16 (optical 4) */
                Package () { 0x000EFFFF, 0x00, 0x00, 0x10 },  /* dev 14 INTA -> GSI 16 (optical 5) */
                Package () { 0x000FFFFF, 0x00, 0x00, 0x10 },  /* dev 15 INTA -> GSI 16 (optical 6) */
                Package () { 0x0010FFFF, 0x00, 0x00, 0x10 },  /* dev 16 INTA -> GSI 16 (optical 7) */
            })

            /* Claim bus 0 so the kernel associates this bridge (and thus its _PRT) with
             * segment 0 / bus 0, and declare the windows the bridge decodes.
             *
             * #354: the memory window used to be missing entirely -- only the bus range was
             * declared. Linux does not police that, so this went unnoticed; OpenBSD checks every
             * BAR against the host bridge's declared resources and rejected both AHCI
             * controllers with "mem address conflict 0x80001000/0x1000", then read the version
             * register as 0x00000000 and refused to attach ("unsupported AHCI revision").
             *
             * The range starts at the 2 GiB line because that is where the guest firmware places
             * BARs, immediately above the default 2048 MiB of guest RAM, and stops below the
             * I/O APIC at 0xFEC00000. NOTE: a VM configured with more than 2048 MiB of RAM will
             * have RAM overlapping the bottom of this window -- see the follow-up issue; the
             * window has to track the RAM top, which a static AML blob cannot do. */
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000,   /* granularity */
                    0x0000,   /* min bus 0 */
                    0x00FF,   /* max bus 255 */
                    0x0000,   /* translation */
                    0x0100)   /* length (256 buses) */

                /* #440: the window must EXCLUDE the ECAM region at 0xE0000000
                 * (HYPE_FW_1_ECAM_GPA, 256 MiB). Windows reserves the MCFG
                 * range and its arbiter fails the whole root bus with problem
                 * code 12 / STATUS_CONFLICTING_ADDRESSES when a bridge window
                 * overlaps it -- measured: ACPI\PNP0A08 "Problem 12", no
                 * Enum\PCI key, no storage driver ever started. QEMU's Q35
                 * DSDT splits its window around the MCFG for the same reason. */
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x00000000,   /* granularity */
                    0x80000000,   /* min  -- 2 GiB, just above guest RAM */
                    0xDFFFFFFF,   /* max  -- last byte below the ECAM */
                    0x00000000,   /* translation */
                    0x60000000)   /* length */

                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed,
                    NonCacheable, ReadWrite,
                    0x00000000,   /* granularity */
                    0xF0000000,   /* min  -- resume above the ECAM */
                    0xFEBFFFFF,   /* max  -- last byte below the I/O APIC */
                    0x00000000,   /* translation */
                    0x0EC00000)   /* length */

                /* The legacy I/O space the bridge forwards: com0/com1, the PS/2 controller, the
                 * PIT and the ACPI PM block all live here and are otherwise undeclared. */
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000,   /* granularity */
                    0x0000,   /* min */
                    0x0CF7,   /* max -- stop below the PCI config ports at 0xCF8 */
                    0x0000,   /* translation */
                    0x0CF8)   /* length */
                WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                    0x0000,
                    0x0D00,   /* resume above the config ports */
                    0xFFFF,
                    0x0000,
                    0xF300)
            })

            /* #440: the ICH9 LPC bridge at 00:1f.0, carrying the legacy ISA
             * devices AS ITS CHILDREN. They used to be siblings of PCI0 at
             * \_SB, which Linux tolerates -- but Windows' resource arbiter
             * then treats their I/O ports as root-level allocations that
             * CONFLICT with PCI0's own I/O windows, and fails the whole root
             * bus with problem code 12 (measured: ACPI\PNP0A08 status
             * 0xC0000018, no Enum\PCI key, no storage stack). Under the LPC
             * bridge the arbiter charges them to the bridge's windows, which
             * is where every real Q35 machine puts them. */
            Device (ISA)
            {
                Name (_ADR, 0x001F0000)

                /* M4-6d7: legacy ISA devices. In ACPI/APIC mode Linux wires an ISA
                 * IRQ into the I/O APIC only when a namespace device claims it via
                 * _CRS (acpi_pnp -> acpi_register_gsi); with none declared, the 8250
                 * driver still probes ttyS0 at 0x3f8 but irq4 is never routed to any
                 * interrupt controller, so every userspace serial write stalls on a
                 * TX-empty IRQ that cannot arrive (and i8042 refuses to probe at all:
                 * "PNP: No PS/2 controller found"). These mirror what QEMU's Q35 DSDT
                 * declares, matching hype's existing COM1/COM2 + PS/2 models. */
                Device (COM1)
                {
                    Name (_HID, EisaId ("PNP0501"))  /* 16550-compatible UART */
                    Name (_UID, 0x01)
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x03F8, 0x03F8, 0x00, 0x08)
                        IRQNoFlags () {4}
                    })
                }

                Device (COM2)
                {
                    Name (_HID, EisaId ("PNP0501"))
                    Name (_UID, 0x02)
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x02F8, 0x02F8, 0x00, 0x08)
                        IRQNoFlags () {3}
                    })
                }

                Device (KBD)
                {
                    Name (_HID, EisaId ("PNP0303"))  /* PS/2 keyboard (i8042 port A) */
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0060, 0x0060, 0x00, 0x01)
                        IO (Decode16, 0x0064, 0x0064, 0x00, 0x01)
                        IRQNoFlags () {1}
                    })
                }

                /*
                 * #303: the CMOS/RTC node. Linux's rtc_cmos binds either through PNP (PNP0B00) or as a
                 * platform device, and its own log line says which it took -- "registered platform RTC
                 * device (no PNP device found)" is the platform fallback, i.e. it looked for this node and
                 * did not find one. hype DOES model a CMOS RTC at 0x70/0x71 (devices/cmos.c, made legal for
                 * EDK2's PcRtc by #286), so a machine description that omits it is simply wrong.
                 *
                 * Standard resources: the two-port index/data pair at 0x70, and IRQ8 for the periodic and
                 * alarm interrupts. hype does not currently RAISE IRQ8 (register C always reads 0), so a
                 * guest asking for periodic wakeups will not get them -- declaring the line is still correct
                 * for the device that exists, and the alternative (omitting it) makes the node unbindable.
                 */
                Device (RTC)
                {
                    Name (_HID, EisaId ("PNP0B00"))  /* MC146818-compatible real-time clock */
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0070, 0x0070, 0x00, 0x02)
                        IRQNoFlags () {8}
                    })
                }

                Device (MOU)
                {
                    Name (_HID, EisaId ("PNP0F13"))  /* PS/2 mouse (i8042 port B) */
                    Name (_STA, 0x0F)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IRQNoFlags () {12}
                    })
                }
            }
        }

    }
}
