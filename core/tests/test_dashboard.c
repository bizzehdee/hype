#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dashboard.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

/* Read row `r` of the grid into a NUL-terminated string (trailing blanks kept). */
static void row_text(const hype_vt_screen_t *s, unsigned r, char *out) {
    unsigned c;
    for (c = 0; c < s->cols; c++) out[c] = (char)hype_vt_screen_cell(s, c, r).ch;
    /* trim trailing spaces for easier substring checks */
    while (c > 0 && out[c - 1] == ' ') c--;
    out[c] = '\0';
}

static int row_has(const hype_vt_screen_t *s, unsigned r, const char *needle) {
    char buf[HYPE_VT_MAX_COLS + 1];
    row_text(s, r, buf);
    return strstr(buf, needle) != NULL;
}

int main(void) {
    /* --- uptime formatting --- */
    char up[16];
    hype_dashboard_fmt_uptime(up, 0);       CHECK("uptime 0", strcmp(up, "00:00:00") == 0);
    hype_dashboard_fmt_uptime(up, 754);     CHECK("uptime 12:34", strcmp(up, "00:12:34") == 0);
    hype_dashboard_fmt_uptime(up, 3661);    CHECK("uptime 1:01:01", strcmp(up, "01:01:01") == 0);
    hype_dashboard_fmt_uptime(up, 45296);   CHECK("uptime 12:34:56", strcmp(up, "12:34:56") == 0);

    /* --- full render --- */
    hype_vt_screen_t *s = malloc(sizeof(*s));
    hype_vt_screen_init(s, 80, 25);

    hype_vm_dash_info_t vms[2] = {
        { "alpine", "linux", "running", 3, 6144, 754, "test.iso", 1 },
        { "fedora", "linux", "off",     0, 6144, 0,   0,          0 },
    };
    hype_dashboard_render(s, vms, 2, 45296);

    CHECK("header line names product", row_has(s, 0, "hype - VM dashboard"));
    CHECK("header shows host uptime", row_has(s, 0, "12:34:56"));
    CHECK("column header present", row_has(s, 2, "NAME") && row_has(s, 2, "STATE") && row_has(s, 2, "MEDIA"));

    /* row 3 = vm1 (alpine, focused) */
    CHECK("vm1 focused marker", row_has(s, 3, ">1"));
    CHECK("vm1 name", row_has(s, 3, "alpine"));
    CHECK("vm1 os", row_has(s, 3, "linux"));
    CHECK("vm1 state", row_has(s, 3, "running"));
    CHECK("vm1 cpu", row_has(s, 3, "3%"));
    CHECK("vm1 mem", row_has(s, 3, "6144M"));
    CHECK("vm1 uptime", row_has(s, 3, "00:12:34"));
    CHECK("vm1 media", row_has(s, 3, "test.iso"));

    /* row 4 = vm2 (fedora, off, no media) */
    CHECK("vm2 name", row_has(s, 4, "fedora"));
    CHECK("vm2 state off", row_has(s, 4, "off"));
    CHECK("vm2 no focus marker (starts with plain '2')", !row_has(s, 4, ">2"));
    CHECK("vm2 media dash", row_has(s, 4, "-"));

    /* --- long name is truncated to the column, no overflow crash --- */
    hype_vt_screen_init(s, 80, 25);
    hype_vm_dash_info_t big[1] = {
        { "a-really-long-vm-name-exceeding-the-column", "linux", "running", 100, 65536, 99999, "some-long-media-name.iso", 0 },
    };
    hype_dashboard_render(s, big, 1, 0);
    CHECK("long name truncated (col header intact on row2)", row_has(s, 2, "NAME"));
    CHECK("long-name row rendered something", row_has(s, 3, "a-really-long"));

    /* --- NULL identity fields fall back to placeholders (no crash) --- */
    hype_vt_screen_init(s, 80, 25);
    {
        hype_vm_dash_info_t nullish[1] = { { NULL, NULL, NULL, 0, 512, 0, NULL, 0 } };
        hype_dashboard_render(s, nullish, 1, 0);
        CHECK("null name -> '?'", row_has(s, 3, "?"));
        CHECK("null media -> '-'", row_has(s, 3, "-"));
    }

    /* --- zero VMs: header still renders, no crash --- */
    hype_vt_screen_init(s, 80, 25);
    hype_dashboard_render(s, NULL, 0, 10);
    CHECK("empty dashboard header", row_has(s, 0, "hype - VM dashboard"));

    free(s);
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
