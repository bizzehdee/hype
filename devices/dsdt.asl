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
             * are mapped for completeness. GSIs 16-19 sit above the 16 ISA lines. */
            Name (_PRT, Package ()
            {
                Package () { 0x0002FFFF, 0x00, 0x00, 0x10 },  /* dev 2 INTA -> GSI 16 */
                Package () { 0x0002FFFF, 0x01, 0x00, 0x11 },  /* dev 2 INTB -> GSI 17 */
                Package () { 0x0002FFFF, 0x02, 0x00, 0x12 },  /* dev 2 INTC -> GSI 18 */
                Package () { 0x0002FFFF, 0x03, 0x00, 0x13 },  /* dev 2 INTD -> GSI 19 */
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
    }
}
