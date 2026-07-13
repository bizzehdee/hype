#include "../core/efi_types.h"
#include "../core/console.h"
#include "../core/halt.h"
#include "../core/memmap.h"
#include "../core/sys_table.h"
#include "../arch/x86_64/cpu/gdt.h"
#include "../arch/x86_64/cpu/idt.h"
#include "../arch/x86_64/cpu/paging.h"

/* Static storage: still valid (and unmoving) after the segment reload
 * below, and after ExitBootServices() once M1-4 lands. */
static hype_gdt_entry_t g_gdt[HYPE_GDT_ENTRY_COUNT];
static hype_idt_entry_t g_idt[HYPE_IDT_ENTRY_COUNT];
static hype_pte_t g_pml4[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pdpt[HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));
static hype_pte_t g_pd[HYPE_PAGING_MAX_GB][HYPE_PAGING_ENTRIES_PER_TABLE] __attribute__((aligned(4096)));

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN map_size = 0, desc_size = 0, map_key = 0;
    EFI_STATUS status;

    hype_sys_table_set(SystemTable);
    hype_console_print(SystemTable, "hype\n");

    hype_gdt_build(g_gdt);
    hype_gdt_load(g_gdt, HYPE_GDT_ENTRY_COUNT);
    hype_console_print(SystemTable, "own GDT loaded\n");

    hype_idt_build(g_idt, hype_isr_stub_table, HYPE_GDT_CODE64_SEL);
    hype_idt_load(g_idt, HYPE_IDT_ENTRY_COUNT);
    hype_console_print(SystemTable, "own IDT loaded\n");

    hype_paging_build_identity(g_pml4, g_pdpt, g_pd, HYPE_PAGING_MAX_GB);
    hype_paging_load(g_pml4);
    hype_console_print(SystemTable, "own paging loaded\n");

    status = hype_memmap_get(SystemTable->BootServices, &map, &map_size, &desc_size, &map_key);
    if (status != EFI_SUCCESS) {
        hype_console_print(SystemTable, "failed to get memory map: 0x%llx\n", (unsigned long long)status);
        return status;
    }

    hype_memmap_dump(SystemTable, map, map_size, desc_size);
    SystemTable->BootServices->FreePool(map);

    status = hype_exit_boot_services(ImageHandle, SystemTable->BootServices);
    if (status != EFI_SUCCESS) {
        hype_console_print(SystemTable, "ExitBootServices failed: 0x%llx\n", (unsigned long long)status);
        return status;
    }

    /*
     * Boot Services -- including ConOut, which every hype_console_print
     * above depended on -- are gone as of the line above. This is now
     * the only kernel running on this CPU; there is no output channel
     * again until M1-5 (serial) or M1-6 (GOP), and nothing else built
     * yet for it to do, so halt.
     */
    hype_halt_forever();
}
