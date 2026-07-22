#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../vt_screen.h"

static int failures = 0;

#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

#define CHECK_EQ(desc, expected, actual) \
    do { long long e=(long long)(expected), a=(long long)(actual); \
        if (e != a) { printf("FAIL: %s: expected %lld, got %lld\n",(desc),e,a); failures++; } } while (0)

static void feed_str(hype_vt_screen_t *s, const char *str) {
    hype_vt_screen_write(s, (const uint8_t *)str, (unsigned)strlen(str));
}

/* Grab a row's visible text into a C string (cols chars + NUL). */
static void row_text(const hype_vt_screen_t *s, unsigned row, char *out) {
    for (unsigned c = 0; c < s->cols; c++) out[c] = (char)hype_vt_screen_cell(s, c, row).ch;
    out[s->cols] = '\0';
}

int main(void) {
    hype_vt_screen_t *s = malloc(sizeof(*s));
    char buf[HYPE_VT_MAX_COLS + 1];

    /* --- init: cleared grid, cursor home, clamped dims --- */
    hype_vt_screen_init(s, 80, 25);
    CHECK_EQ("init cols", 80, s->cols);
    CHECK_EQ("init rows", 25, s->rows);
    CHECK_EQ("cursor home col", 0, s->cur_col);
    CHECK_EQ("cursor home row", 0, s->cur_row);
    CHECK_EQ("blank cell", ' ', hype_vt_screen_cell(s, 10, 10).ch);
    hype_vt_screen_init(s, 9999, 9999);
    CHECK_EQ("cols clamped to max", HYPE_VT_MAX_COLS, s->cols);
    CHECK_EQ("rows clamped to max", HYPE_VT_MAX_ROWS, s->rows);

    /* --- plain text + cursor advance --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "hello");
    row_text(s, 0, buf);
    CHECK("plain text written", strncmp(buf, "hello", 5) == 0);
    CHECK_EQ("cursor advanced", 5, s->cur_col);

    /* --- CR / LF --- */
    feed_str(s, "\r\nworld");
    CHECK_EQ("LF moved to row 1", 1, s->cur_row);
    row_text(s, 1, buf);
    CHECK("text on row 1 after CRLF", strncmp(buf, "world", 5) == 0);
    CHECK("row 0 preserved", strncmp((row_text(s,0,buf), buf), "hello", 5) == 0);

    /* --- backspace --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "ab\bX");
    row_text(s, 0, buf);
    CHECK("backspace overwrites", strncmp(buf, "aX", 2) == 0);

    /* --- tab to next multiple of 8 --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "a\tb");
    CHECK_EQ("tab stop col of 'b'", 9, s->cur_col); /* 'a'@0, tab->8, 'b'@8, cur=9 */
    CHECK_EQ("tab: b at col 8", 'b', hype_vt_screen_cell(s, 8, 0).ch);

    /* --- auto-wrap at right edge --- */
    hype_vt_screen_init(s, 4, 4);
    feed_str(s, "ABCDE"); /* ABCD fills row0, E wraps to row1col0 */
    row_text(s, 0, buf);
    CHECK("wrap row0 full", strncmp(buf, "ABCD", 4) == 0);
    CHECK_EQ("wrap: E on row1", 'E', hype_vt_screen_cell(s, 0, 1).ch);
    CHECK_EQ("wrap: cursor row1 col1", 1, s->cur_row);

    /* --- scroll on LF past last row --- */
    hype_vt_screen_init(s, 8, 2);
    feed_str(s, "one\r\ntwo\r\nthree");
    /* row0 should now be "two", row1 "three" (one scrolled off) */
    row_text(s, 0, buf);
    CHECK("scroll: row0=two", strncmp(buf, "two", 3) == 0);
    row_text(s, 1, buf);
    CHECK("scroll: row1=three", strncmp(buf, "three", 5) == 0);

    /* --- CUP absolute positioning (1-based) --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[5;10HX");
    CHECK_EQ("CUP row", 4, 4); /* row5 -> index4 */
    CHECK_EQ("CUP: X at (col9,row4)", 'X', hype_vt_screen_cell(s, 9, 4).ch);

    /* --- cursor moves A/B/C/D --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[10;10H"); /* to (9,9) */
    feed_str(s, "\x1b[3A");     /* up 3 -> row6 */
    CHECK_EQ("CUU", 6, s->cur_row);
    feed_str(s, "\x1b[2B");     /* down 2 -> row8 */
    CHECK_EQ("CUD", 8, s->cur_row);
    feed_str(s, "\x1b[4C");     /* right 4 -> col13 */
    CHECK_EQ("CUF", 13, s->cur_col);
    feed_str(s, "\x1b[5D");     /* left 5 -> col8 */
    CHECK_EQ("CUB", 8, s->cur_col);
    feed_str(s, "\x1b[100A");   /* clamp at top */
    CHECK_EQ("CUU clamps at 0", 0, s->cur_row);

    /* --- CHA / VPA absolute col/row --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[20G"); /* col 20 -> index19 */
    CHECK_EQ("CHA", 19, s->cur_col);
    feed_str(s, "\x1b[7d");  /* row 7 -> index6 */
    CHECK_EQ("VPA", 6, s->cur_row);

    /* --- EL erase-to-end (K / K0) --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "ABCDEF");
    feed_str(s, "\x1b[3G");  /* back to col 2 (the 'C') */
    feed_str(s, "\x1b[K");   /* erase C..end of line */
    row_text(s, 0, buf);
    CHECK("EL0: kept AB", strncmp(buf, "AB", 2) == 0);
    CHECK_EQ("EL0: C erased", ' ', hype_vt_screen_cell(s, 2, 0).ch);

    /* --- EL erase-to-start (K1) --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "ABCDEF");
    feed_str(s, "\x1b[3G");   /* col2 = 'C' */
    feed_str(s, "\x1b[1K");   /* erase start..cursor inclusive */
    CHECK_EQ("EL1: A erased", ' ', hype_vt_screen_cell(s, 0, 0).ch);
    CHECK_EQ("EL1: C erased", ' ', hype_vt_screen_cell(s, 2, 0).ch);
    CHECK_EQ("EL1: D kept", 'D', hype_vt_screen_cell(s, 3, 0).ch);

    /* --- ED erase whole screen (J2) --- */
    hype_vt_screen_init(s, 8, 3);
    feed_str(s, "aaaa\r\nbbbb\r\ncccc");
    feed_str(s, "\x1b[2J");
    CHECK_EQ("ED2: (0,0) cleared", ' ', hype_vt_screen_cell(s, 0, 0).ch);
    CHECK_EQ("ED2: (2,2) cleared", ' ', hype_vt_screen_cell(s, 2, 2).ch);

    /* --- ED erase cursor-to-end (J0) --- */
    hype_vt_screen_init(s, 8, 3);
    feed_str(s, "aaaa\r\nbbbb\r\ncccc");
    feed_str(s, "\x1b[2;1H");  /* row2 col1 */
    feed_str(s, "\x1b[0J");    /* erase from here down */
    CHECK_EQ("ED0: row0 kept", 'a', hype_vt_screen_cell(s, 0, 0).ch);
    CHECK_EQ("ED0: row1 erased", ' ', hype_vt_screen_cell(s, 0, 1).ch);
    CHECK_EQ("ED0: row2 erased", ' ', hype_vt_screen_cell(s, 0, 2).ch);

    /* --- SGR: fg colour + bold, then reset --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[1;31mR");
    {
        hype_vt_cell_t c = hype_vt_screen_cell(s, 0, 0);
        CHECK("SGR bold set", (c.attr & HYPE_VT_ATTR_BOLD) != 0);
        CHECK_EQ("SGR fg=red(1)", 1u, (c.attr >> HYPE_VT_ATTR_FG_SHIFT) & HYPE_VT_ATTR_COLOR_MASK);
    }
    feed_str(s, "\x1b[0mN");
    {
        hype_vt_cell_t c = hype_vt_screen_cell(s, 1, 0);
        CHECK("SGR reset clears bold", (c.attr & HYPE_VT_ATTR_BOLD) == 0);
        CHECK_EQ("SGR reset fg=default", HYPE_VT_DEFAULT_FG, (c.attr >> HYPE_VT_ATTR_FG_SHIFT) & HYPE_VT_ATTR_COLOR_MASK);
    }
    /* bare ESC[m == reset */
    feed_str(s, "\x1b[7m\x1b[mP");
    CHECK("bare ESC[m resets reverse", (hype_vt_screen_cell(s, 2, 0).attr & HYPE_VT_ATTR_REVERSE) == 0);

    /* --- save / restore cursor --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[5;5H\x1b[s");  /* park + save at (4,4) */
    feed_str(s, "\x1b[20;20H");      /* move away */
    feed_str(s, "\x1b[u");           /* restore */
    CHECK_EQ("restore col", 4, s->cur_col);
    CHECK_EQ("restore row", 4, s->cur_row);

    /* --- DEC private mode toggles are swallowed harmlessly --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[?25l\x1b[?25hZ"); /* hide/show cursor, then 'Z' */
    CHECK_EQ("private-mode swallowed, Z printed", 'Z', hype_vt_screen_cell(s, 0, 0).ch);
    CHECK_EQ("cursor after private+Z", 1, s->cur_col);

    /* --- RIS full reset (ESC c) --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "junk\x1b[5;5H\x1b" "c");
    CHECK_EQ("RIS clears (0,0)", ' ', hype_vt_screen_cell(s, 0, 0).ch);
    CHECK_EQ("RIS home cursor col", 0, s->cur_col);
    CHECK_EQ("RIS home cursor row", 0, s->cur_row);

    /* --- param not tainted by a prior sequence (reset_csi zeroes) --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[15;15H");  /* leaves params {15,15} internally */
    feed_str(s, "\x1b[H");       /* no params -> must go home, not (15,15) */
    CHECK_EQ("no-param CUP homes col (no stale param)", 0, s->cur_col);
    CHECK_EQ("no-param CUP homes row (no stale param)", 0, s->cur_row);

    /* --- SGR: background colour, bold-off/reverse-off, default fg/bg --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[44mB");  /* bg = blue(4) */
    CHECK_EQ("SGR bg=blue(4)", 4u,
             (hype_vt_screen_cell(s, 0, 0).attr >> HYPE_VT_ATTR_BG_SHIFT) & HYPE_VT_ATTR_COLOR_MASK);
    feed_str(s, "\x1b[49mC");  /* bg back to default */
    CHECK_EQ("SGR 49 bg=default", HYPE_VT_DEFAULT_BG,
             (hype_vt_screen_cell(s, 1, 0).attr >> HYPE_VT_ATTR_BG_SHIFT) & HYPE_VT_ATTR_COLOR_MASK);
    feed_str(s, "\x1b[1;7m");   /* bold + reverse on */
    feed_str(s, "\x1b[22m");    /* bold off, reverse stays */
    feed_str(s, "D");
    CHECK("SGR 22 clears bold", (hype_vt_screen_cell(s, 2, 0).attr & HYPE_VT_ATTR_BOLD) == 0);
    CHECK("SGR 22 keeps reverse", (hype_vt_screen_cell(s, 2, 0).attr & HYPE_VT_ATTR_REVERSE) != 0);
    feed_str(s, "\x1b[27mE");   /* reverse off */
    CHECK("SGR 27 clears reverse", (hype_vt_screen_cell(s, 3, 0).attr & HYPE_VT_ATTR_REVERSE) == 0);
    feed_str(s, "\x1b[31m\x1b[39mF"); /* fg red then default */
    CHECK_EQ("SGR 39 fg=default", HYPE_VT_DEFAULT_FG,
             (hype_vt_screen_cell(s, 4, 0).attr >> HYPE_VT_ATTR_FG_SHIFT) & HYPE_VT_ATTR_COLOR_MASK);

    /* --- ED erase start-to-cursor (J1) --- */
    hype_vt_screen_init(s, 8, 3);
    feed_str(s, "aaaa\r\nbbbb\r\ncccc");
    feed_str(s, "\x1b[2;3H");  /* row2 col3 */
    feed_str(s, "\x1b[1J");    /* erase start..cursor */
    CHECK_EQ("ED1: row0 erased", ' ', hype_vt_screen_cell(s, 0, 0).ch);
    CHECK_EQ("ED1: row1 up-to-cursor erased", ' ', hype_vt_screen_cell(s, 1, 1).ch);
    CHECK_EQ("ED1: row2 untouched", 'c', hype_vt_screen_cell(s, 0, 2).ch);

    /* --- EL erase whole line (K2) --- */
    hype_vt_screen_init(s, 8, 2);
    feed_str(s, "abcd");
    feed_str(s, "\x1b[2K");
    row_text(s, 0, buf);
    CHECK("EL2 clears line", buf[0] == ' ' && buf[3] == ' ');

    /* --- BEL is swallowed, cursor unchanged --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "a\ab");
    CHECK_EQ("BEL swallowed: cursor at 2", 2, s->cur_col);
    row_text(s, 0, buf);
    CHECK("BEL: text uninterrupted", strncmp(buf, "ab", 2) == 0);

    /* --- FF / VT behave as line feed --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x0c");  /* FF */
    CHECK_EQ("FF -> next row", 1, s->cur_row);
    feed_str(s, "\x0b");  /* VT */
    CHECK_EQ("VT -> next row", 2, s->cur_row);

    /* --- non-printable control byte dropped, not rendered --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "a\x01" "b"); /* 0x01 (SOH) must be dropped */
    row_text(s, 0, buf);
    CHECK("control byte dropped", strncmp(buf, "ab", 2) == 0);

    /* --- CSI intermediate byte (0x20-0x2F) consumed, sequence still ends --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[0 qZ"); /* DECSCUSR: space intermediate then final 'q' */
    CHECK_EQ("intermediate consumed, Z printed", 'Z', hype_vt_screen_cell(s, 0, 0).ch);

    /* --- empty leading CSI param (';') defaults correctly --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[;5H"); /* row default 1, col 5 */
    CHECK_EQ("empty-leading-param CUP row", 0, s->cur_row);
    CHECK_EQ("empty-leading-param CUP col", 4, s->cur_col);

    /* --- more params than HYPE_VT_MAX_PARAMS: no overflow, still terminates --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[1;2;3;4;5;6;7;8;9;10mY");
    CHECK_EQ("param overflow safe, Y printed", 'Y', hype_vt_screen_cell(s, 0, 0).ch);

    /* --- deferred wrap at bottom-right triggers a scroll --- */
    hype_vt_screen_init(s, 3, 2);
    feed_str(s, "ABC");   /* fills row0, cursor parked past col (deferred) */
    feed_str(s, "DEF");   /* wraps to row1 */
    feed_str(s, "GHI");   /* row1 full; next glyph must scroll */
    feed_str(s, "J");     /* deferred-wrap from row1 -> scroll, J at row1col0 */
    CHECK_EQ("wrap-scroll: J landed row1", 'J', hype_vt_screen_cell(s, 0, 1).ch);
    row_text(s, 0, buf);
    CHECK("wrap-scroll: DEF scrolled up to row0", strncmp(buf, "GHI", 3) == 0);

    /* --- out-of-range cell read returns a blank default cell --- */
    hype_vt_screen_init(s, 80, 25);
    {
        hype_vt_cell_t oob = hype_vt_screen_cell(s, 9999, 9999);
        CHECK_EQ("OOB cell ch=space", ' ', oob.ch);
        CHECK_EQ("OOB cell attr=default", HYPE_VT_DEFAULT_ATTR, oob.attr);
    }

    /* --- init clamps zero dims up to 1 --- */
    hype_vt_screen_init(s, 0, 0);
    CHECK_EQ("init cols 0 -> 1", 1, s->cols);
    CHECK_EQ("init rows 0 -> 1", 1, s->rows);

    /* --- CUD / CUF clamp at the far edges --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[100B");
    CHECK_EQ("CUD clamps at bottom", 24, s->cur_row);
    feed_str(s, "\x1b[100C");
    CHECK_EQ("CUF clamps at right", 79, s->cur_col);

    /* --- CUB past column 0 clamps to 0 (ternary True side) --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[3G");    /* col 2 */
    feed_str(s, "\x1b[10D");   /* left 10 -> clamp 0 */
    CHECK_EQ("CUB clamps at 0", 0, s->cur_col);

    /* --- CHA / VPA overshoot clamp --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[999G");
    CHECK_EQ("CHA overshoot clamps", 79, s->cur_col);
    feed_str(s, "\x1b[999d");
    CHECK_EQ("VPA overshoot clamps", 24, s->cur_row);

    /* --- HVP ('f') is an alias of CUP ('H') --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[4;6fQ");
    CHECK_EQ("HVP Q at (col5,row3)", 'Q', hype_vt_screen_cell(s, 5, 3).ch);

    /* --- ED / EL with an out-of-spec selector fall to the 'whole' default --- */
    hype_vt_screen_init(s, 8, 2);
    feed_str(s, "abcd\r\nefgh");
    feed_str(s, "\x1b[9J");   /* ED default -> whole screen */
    CHECK_EQ("ED default clears all", ' ', hype_vt_screen_cell(s, 0, 0).ch);
    hype_vt_screen_init(s, 8, 2);
    feed_str(s, "abcd");
    feed_str(s, "\x1b[9K");   /* EL default -> whole line */
    CHECK_EQ("EL default clears line", ' ', hype_vt_screen_cell(s, 0, 0).ch);

    /* --- tab at the right edge clamps to the last column --- */
    hype_vt_screen_init(s, 80, 25);
    feed_str(s, "\x1b[79G\t"); /* col 78; next stop 80 >= cols -> clamp 79 */
    CHECK_EQ("tab at edge clamps", 79, s->cur_col);

    /* --- generation bumps on activity --- */
    hype_vt_screen_init(s, 80, 25);
    {
        unsigned g0 = s->generation;
        feed_str(s, "x");
        CHECK("generation advanced", s->generation > g0);
    }

    free(s);
    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
