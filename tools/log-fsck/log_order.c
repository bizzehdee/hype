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
 * Gaps are REPORTED, not failed, and MUST be judged across every sink at once.
 *
 * \HYPE.LOG is opened with HYPE_LOG_SINK_HYPE, not _ALL (boot/main.c's
 * hype_log_sink_open_ordered_durable call): guest console records go to the per-VM logs instead,
 * and #338 retired \HYPEFULL.LOG in favour of "merge the split files by their [offset] prefix to
 * recover the combined stream". So a gap in \HYPE.LOG alone usually means nothing at all -- it is
 * a burst of guest output sitting in \VMn.LOG. Checking the combined log on its own reported
 * 4-5 KB "missing" on five AMD-laptop runs and cost a wrongly-filed ticket before the design was
 * re-read.
 *
 * Pass every log from the run together and the union is checked, which is the stream that is
 * actually supposed to be complete:
 *
 *     tools/log-fsck/run-log-order.sh logs/<boot>/HYPE.LOG logs/<boot>/RUN1A.LOG
 *
 * A backward jump within one file cannot be explained that way, and is the hard failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long *g_all;   /* every stamp seen, across every file */
static unsigned long long g_all_n, g_all_cap;

static void all_add(unsigned long long v) {
    if (g_all_n == g_all_cap) {
        unsigned long long ncap = g_all_cap ? g_all_cap * 2ull : 65536ull;
        unsigned long long *nb = (unsigned long long *)realloc(g_all, (size_t)ncap * sizeof *nb);
        if (nb == 0) { fprintf(stderr, "log_order: out of memory\n"); exit(2); }
        g_all = nb; g_all_cap = ncap;
    }
    g_all[g_all_n++] = v;
}

static int cmp_u64(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static int scan_one(const char *path, int quiet, unsigned long long *out_back);

int main(int argc, char **argv) {
    int i;
    int quiet = 0;
    int rc = 0;
    int files = 0;
    unsigned long long total_back = 0ull;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file> [<file> ...] [--quiet]\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) if (strcmp(argv[i], "--quiet") == 0) quiet = 1;
    for (i = 1; i < argc; i++) {
        unsigned long long back = 0ull;
        if (strcmp(argv[i], "--quiet") == 0) continue;
        if (scan_one(argv[i], quiet, &back) != 0) return 2;
        total_back += back;
        files++;
    }
    if (files == 0) { fprintf(stderr, "log_order: no files given\n"); return 2; }

    /*
     * The union across every sink is the stream that is supposed to be complete. One file's own
     * gaps are expected -- see the header.
     */
    {
        unsigned long long j, gaps = 0ull, gap_bytes = 0ull, prev = 0ull;
        qsort(g_all, (size_t)g_all_n, sizeof *g_all, cmp_u64);
        for (j = 0; j < g_all_n; j++) {
            if (j != 0ull && g_all[j] > prev + 4096ull) {
                gaps++; gap_bytes += g_all[j] - prev;
                if (!quiet && gaps <= 10ull) {
                    printf("  UNION gap: stamp %llu after %llu -- %llu bytes in no sink\n",
                           g_all[j], prev, g_all[j] - prev);
                }
            }
            prev = g_all[j];
        }
        printf("union of %d file(s): %llu records, stamps %llu..%llu | gaps=%llu (%llu bytes)\n",
               files, g_all_n, g_all_n ? g_all[0] : 0ull, prev, gaps, gap_bytes);
    }
    free(g_all);
    if (total_back != 0ull) {
        printf("FAIL: record order does not match produced order [#809]\n");
        rc = 1;
    } else {
        printf("ok: every file monotonic [#809]\n");
    }
    return rc;
}

static int scan_one(const char *path, int quiet, unsigned long long *out_back) {
    FILE *f;
    char *buf;
    long size;
    long i;
    unsigned long long prev = 0ull, prev_off = 0ull;
    unsigned long long records = 0ull, back = 0ull, gaps = 0ull, gap_bytes = 0ull;
    unsigned long long first = 0ull, last = 0ull;
    int have_first = 0;
    const char *argv1 = path;

    f = fopen(argv1, "rb");
    if (f == 0) { fprintf(stderr, "log_order: cannot open %s\n", argv1); return 2; }
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0) { fprintf(stderr, "log_order: %s is empty\n", argv1); fclose(f); return 2; }
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
        all_add(v);
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

    printf("%s: %lld bytes, %llu records, stamps %llu..%llu | backward=%llu own-gaps=%llu "
           "(%llu bytes not in THIS sink)\n",
           argv1, (long long)size, records, first, last, back, gaps, gap_bytes);
    free(buf);
    *out_back = back;
    return 0;
}
