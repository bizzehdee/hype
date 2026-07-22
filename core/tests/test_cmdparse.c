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
    CHECK("garbage -> UNKNOWN", P("frobnicate").verb == HYPE_CMD_UNKNOWN);

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

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
