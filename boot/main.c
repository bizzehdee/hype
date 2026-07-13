#include "../core/efi_types.h"
#include "../core/console.h"
#include "../core/memmap.h"
#include "../core/sys_table.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"

/* Static storage: still valid (and unmoving) after the segment reload
 * below, and after ExitBootServices() once M1-4 lands. */
static hype_gdt_entry_t g_gdt[HYPE_GDT_ENTRY_COUNT];
static hype_idt_entry_t g_idt[HYPE_IDT_ENTRY_COUNT];

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;

    (void)ImageHandle;

    hype_sys_table_set(SystemTable);
    hype_console_print(SystemTable, "hype\n");

    hype_gdt_build(g_gdt);
    hype_gdt_load(g_gdt, HYPE_GDT_ENTRY_COUNT);
    hype_console_print(SystemTable, "own GDT loaded\n");

    hype_idt_build(g_idt, hype_isr_stub_table, HYPE_GDT_CODE64_SEL);
    hype_idt_load(g_idt, HYPE_IDT_ENTRY_COUNT);
    hype_console_print(SystemTable, "own IDT loaded\n");

    status = hype_memmap_get(SystemTable->BootServices, &map, &map_size, &desc_size, &map_key);
    if (status != EFI_SUCCESS) {
        hype_console_print(SystemTable, "failed to get memory map: 0x%llx\n", (unsigned long long)status);
        return status;
    }

    hype_memmap_dump(SystemTable, map, map_size, desc_size);
    SystemTable->BootServices->FreePool(map);

    /*
     * Returning EFI_SUCCESS here hands control back to firmware, which
     * keeps running its own subsequent code (the boot manager, etc.)
     * with our GDT/IDT still installed at the CPU level -- firmware
     * wasn't expecting that, so its first hardware interrupt after this
     * point hits our (currently panics-on-everything) IDT instead of
     * its own. That's expected and harmless for M1-2: this whole
     * boot-services-still-attached flow is transitional scaffolding.
     * Once M1-4 calls ExitBootServices(), control never returns to
     * firmware at all, and this stops being a real scenario.
     */
    return EFI_SUCCESS;
}
