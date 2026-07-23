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
            })

            /* Minimal resource template: claim bus 0 so the kernel associates
             * this bridge (and thus its _PRT) with segment 0 / bus 0. */
            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000,   /* granularity */
                    0x0000,   /* min bus 0 */
                    0x00FF,   /* max bus 255 */
                    0x0000,   /* translation */
                    0x0100)   /* length (256 buses) */
            })
        }

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
