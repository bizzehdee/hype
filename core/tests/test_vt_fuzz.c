#include <stdio.h>
#include <string.h>
#include "../vt_screen.h"

/*
 * #513: adversarial byte-stream fuzz of the VT interpreter, with canaries.
 *
 * The fifth real-hardware run pinned a deterministic host #PF to a garbage %s pointer whose
 * corrupted value decodes as two 32-bit integers -- the shape of parsed CSI parameters -- and
 * hype_fw_vm_t places `name` directly after the per-VM `term` grid. If any escape sequence can
 * write one byte past hype_vt_screen_t, the neighbours (`vm->name`, then `vcpu_count`, which is
 * exactly the "no vCPUs on the dashboard" symptom) are what it hits. This test feeds millions of
 * adversarial bytes -- heavy on ESC/CSI, huge parameters, parameter floods, saves/restores,
 * resets -- into a screen bracketed by canary regions, and checks the canaries and the cursor
 * invariants continuously. Deterministic (xorshift, fixed seed), per the repeatability rule.
 */

static int failures = 0;

#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

#define CANARY_BYTES 8192u
#define CANARY_FILL 0xA5u

typedef struct {
    unsigned char pre[CANARY_BYTES];
    hype_vt_screen_t s;
    unsigned char post[CANARY_BYTES];
} guarded_screen_t;

static guarded_screen_t g;

static unsigned long long xs_state;
static unsigned xs_next(void) {
    unsigned long long x = xs_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    xs_state = x;
    return (unsigned)(x >> 32);
}

static void arm_canaries(void) {
    memset(g.pre, CANARY_FILL, sizeof(g.pre));
    memset(g.post, CANARY_FILL, sizeof(g.post));
}

static int canaries_intact(void) {
    unsigned i;
    for (i = 0; i < CANARY_BYTES; i++) {
        if (g.pre[i] != CANARY_FILL || g.post[i] != CANARY_FILL) {
            printf("  canary broken at %s+%u (pre=0x%02x post=0x%02x)\n",
                   (g.pre[i] != CANARY_FILL) ? "pre" : "post", i, g.pre[i], g.post[i]);
            return 0;
        }
    }
    return 1;
}

static int invariants_hold(void) {
    /* put_glyph permits cur_col == cols (deferred wrap); nothing permits more. */
    return g.s.cur_col <= g.s.cols && g.s.cur_row < g.s.rows &&
           g.s.cols <= HYPE_VT_MAX_COLS && g.s.rows <= HYPE_VT_MAX_ROWS &&
           g.s.n_params <= HYPE_VT_MAX_PARAMS;
}

/* Weighted adversarial byte: mostly the characters that drive the parser's
 * edges, with printable filler so glyph writes and wraps happen too. */
static unsigned char fuzz_byte(void) {
    static const char specials[] = "\x1b[;?0123456789ABCDGHJKfhlmnrsudc78()\r\n\t\b\x07\x0b\x0c";
    unsigned r = xs_next();
    if ((r % 100u) < 70u) {
        return (unsigned char)specials[(r >> 8) % (sizeof(specials) - 1u)];
    }
    return (unsigned char)(0x20u + ((r >> 8) % 0x60u)); /* printable + DEL edge */
}

static void fuzz_one_screen(unsigned cols, unsigned rows, unsigned long long seed,
                            unsigned long bytes) {
    unsigned long i;
    char what[96];

    xs_state = seed;
    arm_canaries();
    hype_vt_screen_init(&g.s, cols, rows);
    for (i = 0; i < bytes; i++) {
        hype_vt_screen_feed(&g.s, fuzz_byte());
        if ((i & 0xFFFu) == 0xFFFu) {
            if (!invariants_hold() || !canaries_intact()) {
                snprintf(what, sizeof(what),
                         "fuzz %ux%u seed=%llu: corruption by byte %lu (cur=%u,%u dims=%u,%u)",
                         cols, rows, seed, i, g.s.cur_col, g.s.cur_row, g.s.cols, g.s.rows);
                CHECK(what, 0);
                return;
            }
        }
    }
    snprintf(what, sizeof(what), "fuzz %ux%u seed=%llu: final canaries", cols, rows, seed);
    CHECK(what, canaries_intact() && invariants_hold());
}

/* The exact sequences a bounds bug would live in, at maximum amplitude --
 * driven separately from the random stream so they always execute. */
static void directed_cases(void) {
    static const char *cases[] = {
        "\x1b[999999999;999999999H",                    /* CUP far out of range */
        "\x1b[4294967295d",                             /* VPA at UINT_MAX */
        "\x1b[4294967295G",                             /* CHA at UINT_MAX */
        "\x1b[999999999B\x1b[999999999C",               /* relative moves, huge */
        "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16H", /* parameter flood */
        "\x1b[;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;m",       /* semicolon flood */
        "\x1b[999999999A\x1b[999999999D",               /* huge up/left (underflow) */
        "\x1b[2J\x1b[1J\x1b[0J\x1b[2K\x1b[1K\x1b[0K",   /* erases at the edges */
        ("\x1b" "7\x1b[999;999H\x1b" "8"),                /* DECSC/DECRC round trip */
        "\x1b[?25l\x1b[?7h\x1b[r\x1b[999r",             /* private modes + scroll region */
        "\x1b" "c",                                     /* RIS mid-stream */
    };
    unsigned ci;
    char what[96];

    arm_canaries();
    hype_vt_screen_init(&g.s, 80, 25);
    for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const char *c = cases[ci];
        hype_vt_screen_write(&g.s, (const unsigned char *)c, (unsigned)strlen(c));
        /* Land glyphs at whatever position the sequence produced. */
        hype_vt_screen_write(&g.s, (const unsigned char *)"XY\r\nZ", 5);
        snprintf(what, sizeof(what), "directed case %u", ci);
        CHECK(what, canaries_intact() && invariants_hold());
    }
}

int main(void) {
    /* The laptop-shaped geometry (big panel), the QEMU-shaped one, the caps,
     * and a degenerate minimum. 2M adversarial bytes each. */
    fuzz_one_screen(240, 67, 0x513513513ull, 2000000ul);
    fuzz_one_screen(128, 48, 0xdecafbadull, 2000000ul);
    fuzz_one_screen(HYPE_VT_MAX_COLS, HYPE_VT_MAX_ROWS, 42ull, 2000000ul);
    fuzz_one_screen(1, 1, 7ull, 500000ul);
    directed_cases();

    if (failures == 0) {
        printf("test_vt_fuzz: all passed\n");
        return 0;
    }
    printf("test_vt_fuzz: %d failure(s)\n", failures);
    return 1;
}
