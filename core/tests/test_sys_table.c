#include <stdio.h>
#include "../sys_table.h"

static int failures = 0;

int main(void) {
    EFI_SYSTEM_TABLE fake;
    EFI_SYSTEM_TABLE *got;

    hype_sys_table_set(&fake);
    got = hype_sys_table_get();
    if (got != &fake) {
        printf("FAIL: hype_sys_table_get() did not round-trip the pointer set by hype_sys_table_set()\n");
        failures++;
    }

    hype_sys_table_set(0);
    got = hype_sys_table_get();
    if (got != 0) {
        printf("FAIL: hype_sys_table_get() should return NULL after being set to NULL\n");
        failures++;
    }

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
