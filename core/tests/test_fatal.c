#include <stdio.h>
#include "../fatal.h"

static int failures = 0;

int main(void) {
    hype_gop_console_t con;
    hype_gop_console_t *got;

    hype_fatal_set_gop(&con);
    got = hype_fatal_get_gop();
    if (got != &con) {
        printf("FAIL: hype_fatal_get_gop() did not round-trip the pointer set by hype_fatal_set_gop()\n");
        failures++;
    }

    hype_fatal_set_gop(0);
    got = hype_fatal_get_gop();
    if (got != 0) {
        printf("FAIL: hype_fatal_get_gop() should return NULL after being set to NULL\n");
        failures++;
    }

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
