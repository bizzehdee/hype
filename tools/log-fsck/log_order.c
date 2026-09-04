/*
 * #809: does a log file's own record order match the order the records were produced?
 *
 * Every hype log record carries an ABSOLUTE produced-offset prefix -- "[0000267153] " -- which
 * #585 made absolute across capture-buffer reclaims precisely so this question could be asked.
 * Nothing asked it. tools/log-fsck's existing judge checks that the volume is clean and each
 * file is readable at the length hype believes it wrote; a file whose bytes are all present, all
 * intact, and in the WRONG ORDER passes that unchanged.
 *
 * Boot AMD-L0 run 5 was exactly that: 293,959 bytes whose newest record is stamped 267,153, with
 * the final 26,799 bytes holding records stamped ~243,400. Readable, correct length, chain clean
 * -- and 24 KB of the run replayed out of sequence at the end. Every conclusion drawn from a hype
 * log assumes file order, so this needs a gate.
 *
 *   usage: log_order <file> [--quiet]
 *
 * Exit 0 = monotonic. Exit 1 = a backward jump. Exit 2 = unreadable.
 *
 * Gaps are REPORTED, not failed: a gap is the normal signature of a run that lost its tail or was
 * reclaimed while a sink was down, and of a filtered per-VM sink, which only ever holds a subset
 * of the records. A backward jump cannot be explained that way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    FILE *f;
    char *buf;
    long size;
    long i;
    int quiet = 0;
    unsigned long long prev = 0ull, prev_off = 0ull;
    unsigned long long records = 0ull, back = 0ull, gaps = 0ull, gap_bytes = 0ull;
    unsigned long long first = 0ull, last = 0ull;
    int have_first = 0;

    if (argc < 2) { fprintf(stderr, "usage: %s <file> [--quiet]\n", argv[0]); return 2; }
    for (i = 2; i < argc; i++) if (strcmp(argv[i], "--quiet") == 0) quiet = 1;

    f = fopen(argv[1], "rb");
    if (f == 0) { fprintf(stderr, "log_order: cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0) { fprintf(stderr, "log_order: %s is empty\n", argv[1]); fclose(f); return 2; }
    buf = (char *)malloc((size_t)size);
    if (buf == 0) { fclose(f); fprintf(stderr, "log_order: out of memory\n"); return 2; }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(buf); fprintf(stderr, "log_order: short read\n"); return 2;
    }
    fclose(f);

    /*
     * Scan for the prefix anywhere a line can start, including the very first byte. Deliberately
     * not line-split first: run 5's out-of-order region begins MID-LINE ("w-1 SPIN ..."), so a
     * parser that trusts line boundaries would mis-attribute the first record it finds there.
     */
    for (i = 0; i + 12 <= size; i++) {
        unsigned long long v = 0ull;
        int d;
        if (buf[i] != '[') continue;
        if (i != 0 && buf[i - 1] != '\n') continue;
        for (d = 0; d < 10; d++) {
            char c = buf[i + 1 + d];
            if (c < '0' || c > '9') break;
            v = v * 10ull + (unsigned long long)(c - '0');
        }
        if (d != 10 || buf[i + 11] != ']') continue;

        records++;
        if (!have_first) { first = v; have_first = 1; }
        else if (v < prev) {
            back++;
            if (!quiet && back <= 10ull) {
                printf("  BACKWARD at file offset %ld: stamp %llu after %llu (from offset %llu)"
                       " -- %llu bytes earlier in the produced stream\n",
                       i, v, prev, prev_off, prev - v);
            }
        } else if (v > prev + 4096ull) {
            gaps++;
            gap_bytes += v - prev;
            if (!quiet && gaps <= 10ull) {
                printf("  gap at file offset %ld: stamp %llu after %llu -- %llu bytes missing\n",
                       i, v, prev, v - prev);
            }
        }
        if (v >= prev) { prev = v; prev_off = (unsigned long long)i; }
        last = v;
    }

    printf("%s: %lld bytes, %llu records, stamps %llu..%llu | backward=%llu gaps=%llu "
           "(%llu bytes missing)\n",
           argv[1], (long long)size, records, first, last, back, gaps, gap_bytes);
    free(buf);
    if (back != 0ull) { printf("  FAIL: record order does not match produced order [#809]\n"); return 1; }
    printf("  ok: monotonic [#809]\n");
    return 0;
}
