#include <stdio.h>
#include <string.h>
#include "../cmdparse.h"

static int failures = 0;
#define CHECK(desc, cond) do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

static hype_cmd_t P(const char *s) { return hype_cmd_parse(s); }

int main(void) {
    /* verbs + aliases */
    CHECK("empty -> NONE", P("")             .verb == HYPE_CMD_NONE);
    CHECK("spaces -> NONE", P("   ")         .verb == HYPE_CMD_NONE);
    CHECK("help", P("help")                  .verb == HYPE_CMD_HELP);
    CHECK("? alias", P("?")                   .verb == HYPE_CMD_HELP);
    CHECK("list", P("list")                  .verb == HYPE_CMD_LIST);
    CHECK("ls alias", P("ls")                 .verb == HYPE_CMD_LIST);
    CHECK("status", P("status vm0")          .verb == HYPE_CMD_STATUS);
    CHECK("start", P("start vm1")            .verb == HYPE_CMD_START);
    CHECK("stop", P("stop vm0")              .verb == HYPE_CMD_STOP);
    CHECK("pause alias -> STOP", P("pause vm0").verb == HYPE_CMD_STOP);
    CHECK("resume", P("resume vm0")          .verb == HYPE_CMD_RESUME);
    CHECK("shutdown", P("shutdown vm0")      .verb == HYPE_CMD_SHUTDOWN);
    CHECK("poweroff alias -> SHUTDOWN", P("poweroff vm0").verb == HYPE_CMD_SHUTDOWN);
    CHECK("off -> POWEROFF", P("off vm0")    .verb == HYPE_CMD_POWEROFF);
    CHECK("kill alias -> POWEROFF", P("kill vm0").verb == HYPE_CMD_POWEROFF);
    CHECK("focus", P("focus vm1")            .verb == HYPE_CMD_FOCUS);
    CHECK("switch alias -> FOCUS", P("switch 2").verb == HYPE_CMD_FOCUS);
    CHECK("confirm", P("confirm S4EWNF0")     .verb == HYPE_CMD_CONFIRM);
    CHECK("confirm carries serial arg", strcmp(P("confirm S4EWNF0").arg, "S4EWNF0") == 0);
    CHECK("garbage -> UNKNOWN", P("frobnicate").verb == HYPE_CMD_UNKNOWN);
    CHECK("resolution carries its value arg", strcmp(P("resolution 1920x1080").arg, "1920x1080") == 0);
    CHECK("config", P("config vm0").verb == HYPE_CMD_CONFIG);
    CHECK("cfg alias -> CONFIG", P("cfg vm0").verb == HYPE_CMD_CONFIG);
    CHECK("config carries its vm arg", strcmp(P("config vm0").arg, "vm0") == 0);

    /* case-insensitive */
    CHECK("STOP uppercase", P("STOP vm0")    .verb == HYPE_CMD_STOP);
    CHECK("ShUtDoWn mixed", P("ShUtDoWn x")  .verb == HYPE_CMD_SHUTDOWN);

    /* arg extraction */
    {
        hype_cmd_t c = P("stop vm0");
        CHECK("arg present", c.has_arg);
        CHECK("arg = vm0", strcmp(c.arg, "vm0") == 0);
    }
    {
        hype_cmd_t c = P("list");
        CHECK("no arg", !c.has_arg && c.arg[0] == '\0');
    }
    {
        hype_cmd_t c = P("   focus    vm1   extra ");
        CHECK("leading/inner ws: verb FOCUS", c.verb == HYPE_CMD_FOCUS);
        CHECK("leading/inner ws: arg vm1 (extra ignored)", strcmp(c.arg, "vm1") == 0);
    }
    {
        hype_cmd_t c = P("start 2");
        CHECK("numeric arg", strcmp(c.arg, "2") == 0);
    }
    /* over-long arg truncated safely */
    {
        hype_cmd_t c = P("stop aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        CHECK("long arg truncated", strlen(c.arg) < HYPE_CMD_ARG_MAX);
        CHECK("long arg still STOP", c.verb == HYPE_CMD_STOP);
    }
    /* NULL line */
    CHECK("NULL line -> NONE", P(NULL).verb == HYPE_CMD_NONE);

    /*
     * #459: the usage table is the single source the dashboard hint and `help` both render from.
     * Two hand-written copies had already fallen two verbs behind, so guard both directions:
     * every usage entry must name a verb the parser knows, and there must be exactly one entry
     * per verb. Adding HYPE_CMD_* without a usage line now fails here.
     */
    /* TERM-14 (#490): the three-argument form. */
    {
        hype_cmd_t c = P("set vm0 mem_mb 1024");
        CHECK("set verb", c.verb == HYPE_CMD_SET);
        CHECK("set arg1", c.has_arg && strcmp(c.arg, "vm0") == 0);
        CHECK("set arg2", c.has_arg2 && strcmp(c.arg2, "mem_mb") == 0);
        CHECK("set arg3", c.has_arg3 && strcmp(c.arg3, "1024") == 0);
        c = P("set   vm0    label   webbox");
        CHECK("set with extra ws", c.has_arg3 && strcmp(c.arg3, "webbox") == 0);
        c = P("set vm0 mem_mb");
        CHECK("missing value flagged", c.has_arg2 && !c.has_arg3);
        c = P("status vm0");
        CHECK("one-arg verbs leave arg2/3 empty", !c.has_arg2 && !c.has_arg3 &&
              c.arg2[0] == '\0' && c.arg3[0] == '\0');
        /* M9-2 (#175). */
        c = P("host reboot");
        CHECK("host verb", c.verb == HYPE_CMD_HOST && c.has_arg &&
              strcmp(c.arg, "reboot") == 0);
        /* #611. */
        c = P("dump vm0");
        CHECK("dump verb", c.verb == HYPE_CMD_DUMP && c.has_arg &&
              strcmp(c.arg, "vm0") == 0);
        /* #505: mkdisk's fourth argument (format). */
        c = P("mkdisk WD-1234 \\hype\\disks\\smp.img 10 raw");
        CHECK("mkdisk verb", c.verb == HYPE_CMD_MKDISK);
        CHECK("mkdisk arg3", c.has_arg3 && strcmp(c.arg3, "10") == 0);
        CHECK("mkdisk arg4", c.has_arg4 && strcmp(c.arg4, "raw") == 0);
        c = P("mkdisk WD-1234 \\hype\\disks\\smp.img 10");
        CHECK("mkdisk without format leaves arg4 empty", !c.has_arg4 && c.arg4[0] == '\0');
    }

    {
        unsigned i, n = hype_cmd_usage_count();
        /* The range END must name the LAST verb in the enum before HYPE_CMD_UNKNOWN. Adding a
         * verb without a usage entry (or the reverse) fails here, which is what caught #568's
         * `screenshot` before it shipped half-added. */
        CHECK("one usage entry per verb",
              n == (unsigned)(HYPE_CMD_DUMP - HYPE_CMD_HELP + 1));
        for (i = 0; i < n; i++) {
            hype_cmd_t c = P(hype_cmd_usage(i));
            if (c.verb == HYPE_CMD_UNKNOWN || c.verb == HYPE_CMD_NONE) {
                printf("FAIL: usage entry '%s' does not parse to a verb\n", hype_cmd_usage(i));
                failures++;
            }
        }
        CHECK("out-of-range usage index is empty, not a crash",
              hype_cmd_usage(n)[0] == '\0');
    }

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
