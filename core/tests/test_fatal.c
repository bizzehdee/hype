#include <stdio.h>
#include "../fatal.h"

static int failures = 0;

static int g_hook_ran = 0;
static void test_flush_hook(void) {
    g_hook_ran = 1;
}

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

    {
        EFI_GRAPHICS_OUTPUT_PROTOCOL fake_gop;
        int real_fb_marker;
        EFI_GRAPHICS_OUTPUT_PROTOCOL *got_proto;
        void *got_fb;

        hype_fatal_set_gop_protocol(&fake_gop, &real_fb_marker);
        got_proto = hype_fatal_get_gop_protocol();
        got_fb = hype_fatal_get_real_fb();
        if (got_proto != &fake_gop) {
            printf("FAIL: hype_fatal_get_gop_protocol() did not round-trip the registered pointer\n");
            failures++;
        }
        if (got_fb != &real_fb_marker) {
            printf("FAIL: hype_fatal_get_real_fb() did not round-trip the registered pointer\n");
            failures++;
        }

        /* The documented "clear gop, keep real_fb" pattern used right
         * after ExitBootServices(). */
        hype_fatal_set_gop_protocol(0, &real_fb_marker);
        if (hype_fatal_get_gop_protocol() != 0) {
            printf("FAIL: hype_fatal_get_gop_protocol() should return NULL after being cleared\n");
            failures++;
        }
        if (hype_fatal_get_real_fb() != &real_fb_marker) {
            printf("FAIL: hype_fatal_get_real_fb() should be unaffected by clearing the gop protocol\n");
            failures++;
        }
    }

    {
        /* Flush-hook round-trip (the mid-run panic log flush hype_fatal
         * calls before halting). */
        hype_flush_hook_t got_hook;
        hype_fatal_set_flush_hook(test_flush_hook);
        got_hook = hype_fatal_get_flush_hook();
        if (got_hook != test_flush_hook) {
            printf("FAIL: hype_fatal_get_flush_hook() did not round-trip the registered hook\n");
            failures++;
        }
        g_hook_ran = 0;
        got_hook(); /* the pointer must be callable and reach our helper */
        if (g_hook_ran != 1) {
            printf("FAIL: the registered flush hook did not run when called\n");
            failures++;
        }
        hype_fatal_set_flush_hook(0);
        if (hype_fatal_get_flush_hook() != 0) {
            printf("FAIL: hype_fatal_get_flush_hook() should return NULL after being cleared\n");
            failures++;
        }
    }

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
