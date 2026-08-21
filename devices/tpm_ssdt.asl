DefinitionBlock ("", "SSDT", 2, "HYPE", "HYPETPM", 0x00000001)
{
    Scope (\_SB)
    {
        Device (TPM)
        {
            Name (_HID, "MSFT0101")   /* Windows fTPM / Linux tpm_crb match */
            Name (_UID, Zero)
            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0xFED40000, 0x00001000)
            })
            Method (_STA, 0, NotSerialized)
            {
                Return (0x0F)
            }
        }
    }
}
