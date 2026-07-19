#ifndef HYPE_CORE_CLOCKFACTS_H
#define HYPE_CORE_CLOCKFACTS_H

/*
 * PERF-1 (gaps #3/#4): pin the guest kernel's own clocksource-selection and
 * delay-calibration dmesg lines into a small fixed buffer, so hype can re-emit
 * them into the RT-3 cold-boot-surviving nvlog on every diagnostics tick.
 *
 * Why this exists: those lines ("Calibrating delay loop ... lpj=NNNN",
 * "clocksource: Switched to clocksource tsc", "Marking TSC unstable") print
 * VERY early in a Linux boot. On the serial-less HW test box they scroll off
 * the GOP screen, and they also fall out of the 16 KB nvlog tail long before
 * the ~5-minute boot reaches login. Capturing them once, here, keeps them
 * available for the login-time snapshot -- directly answering "did Linux keep
 * the TSC as its clocksource?" (gap #3) and "is loops_per_jiffy sane?" (#4)
 * instead of inferring it.
 *
 * Pure logic (operates on a caller-owned buffer, no I/O) so it is fully
 * unit-testable; the glue that feeds it drained console lines lives in the
 * exempt FW-1 path.
 */

#define HYPE_CLOCKFACTS_CAP 384u

typedef struct hype_clockfacts {
    char buf[HYPE_CLOCKFACTS_CAP]; /* NUL-terminated; captured lines joined by " | " */
    unsigned int len;              /* strlen(buf) */
} hype_clockfacts_t;

/* Empties the buffer (buf[0]='\0', len=0). */
void hype_clockfacts_reset(hype_clockfacts_t *cf);

/* If `line` contains one of the clock-relevant key substrings and isn't
 * already captured, append it (joined with " | ", truncated to fit, never
 * overflowing) and return 1. Returns 0 on no-match, duplicate, or no room. */
int hype_clockfacts_observe(hype_clockfacts_t *cf, const char *line);

#endif /* HYPE_CORE_CLOCKFACTS_H */
