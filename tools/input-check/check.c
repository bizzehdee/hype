/*
 * Parse a section-6k input script with hype's OWN parser and report the verdict.
 *
 * This exists because a script that hype refuses to arm does not look like a failure. hype says
 * "PARSE ERROR line N: ... -- refusing to arm" and carries on; the guest still boots and still
 * writes a per-VM log, so the run looks like it worked. tools/hwstick/input/vm1.txt was over the
 * 128-byte argument limit and had therefore NEVER armed -- the sustained load it was written to
 * apply had not run once, across every hardware run that used it.
 *
 * Linking the real core/input_script.c rather than re-implementing the rules is the point: a
 * checker with its own idea of the limits would drift from the parser it is meant to protect.
 */
#include <stdio.h>
#include <string.h>
#include "../../core/input_script.h"

int main(int argc, char **argv) {
    static char buf[1 << 20];
    static hype_input_script_t sc;
    int rc = 0, i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <script> [script ...]\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        size_t n;
        hype_input_parse_result_t r;

        if (f == NULL) {
            printf("FAIL   %s -- cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        r = hype_input_script_parse(buf, (unsigned int)n, &sc);
        if (r.status == HYPE_INPUT_PARSE_OK) {
            printf("ok     %s -- %u directive(s)\n", argv[i], sc.count);
        } else {
            printf("FAIL   %s -- line %u: %s\n", argv[i], r.line,
                   hype_input_parse_status_str(r.status));
            rc = 1;
        }
    }
    return rc;
}
