#include "sys_table.h"

static EFI_SYSTEM_TABLE *g_system_table = 0;

void hype_sys_table_set(EFI_SYSTEM_TABLE *system_table) {
    g_system_table = system_table;
}

EFI_SYSTEM_TABLE *hype_sys_table_get(void) {
    return g_system_table;
}
