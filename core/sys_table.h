#ifndef HYPE_SYS_TABLE_H
#define HYPE_SYS_TABLE_H

#include "efi_types.h"

/*
 * Global EFI_SYSTEM_TABLE pointer, set once early in efi_main. Exists
 * so code that has no other way to reach it -- e.g. an interrupt
 * handler invoked by hardware, with no argument to thread it through --
 * can still get at ConOut for panic output, until M1-5/M1-7 move panic
 * output to the serial driver and this stops being the only channel.
 */
void hype_sys_table_set(EFI_SYSTEM_TABLE *system_table);
EFI_SYSTEM_TABLE *hype_sys_table_get(void);

#endif /* HYPE_SYS_TABLE_H */
