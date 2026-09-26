/*
 * SMP-13 (#469): memory isolation across vCPU switches on a shared core.
 *
 * Two (or more) VMs run this on ONE shared core, each with its own `seed=` on the command line.
 * Every round writes a seed- and round-dependent pattern over all of this guest's RAM, then
 * verifies all of it. The VMs alternate on the core many times per round, so:
 *
 *  - a missed NPT/EPT root swap would let this guest's writes land in the other VM's RAM (or read
 *    its RAM back), and the verify pass sees the OTHER seed's pattern;
 *  - a missed ASID/VPID change (a stale TLB entry from the other VM) does the same;
 *  - a host-side switch that corrupted guest state shows as a wrong value or a crash.
 *
 * Write-all-then-verify-all, as ram1 does, so aliasing inside one guest is caught too. A mismatch
 * reports the address, what was read, and whether it is the pattern of a different seed.
 *
 * smp=1 also brings up vCPU 1 of the SAME VM by INIT/SIPI, straight into long mode on this
 * guest's own page tables. It runs the same rounds over the upper half of RAM with its own seed,
 * while vCPU 0 covers the rest; the two share one core when the VM is on the shared tier.
 *
 *   cmdline: seed=<hex> [rounds=<decimal>] [smp=1]      (rounds defaults to 20)
 */
#include "micro.h"

#define NAME "sharemem"

#define KBOOT_LOAD_GPA 0x1000000ull
#define PAGE 4096ull
#define LOW_START 0x100000ull
#define LOW_END KBOOT_LOAD_GPA
#define HIGH_START 0x1400000ull
#define MIX 0x9E3779B97F4A7C15ull

/* smp=1 layout, below 1 MB and clear of the zero page (0x7000) and the cmdline (0x8000). */
#define TRAMP_GPA 0xA000u           /* SIPI vector 0x0A */
#define GDTR_GPA 0x9000u
#define GDT_GPA 0x9010u
#define AP_ENTRY_GPA 0x9500u        /* 64-bit entry the trampoline calls */
#define AP_STACK_GPA 0x9508u        /* its RSP */
#define AP_STATUS_GPA 0x9600u       /* [0] rounds done, [1] failed, [2] failing gpa */
#define AP_STACK_TOP 0x60000ull
#define LAPIC_BASE 0xFEE00000u

/*
 * Real mode -> long mode in one step: PAE, CR3 = 0x1000 (this guest's identity map), EFER.LME,
 * then CR0.PE|PG and a far jump into the 64-bit code segment. Assembled from this source with
 * clang, then pasted, as apunclaimed.c does:
 *      .code16
 *      cli; xor %ax,%ax; mov %ax,%ds; lgdtl 0x9000
 *      mov %cr4,%eax; or $0x20,%eax; mov %eax,%cr4
 *      mov $0x1000,%eax; mov %eax,%cr3
 *      mov $0xC0000080,%ecx; rdmsr; or $0x100,%eax; wrmsr
 *      mov %cr0,%eax; or $0x80000001,%eax; mov %eax,%cr0
 *      ljmpl $0x08,$0xA050
 *      .org 0x50
 *      .code64
 *      mov $0x10,%ax; mov %ax,%ds; mov %ax,%es; mov %ax,%ss
 *      mov 0x9508,%rsp; mov 0x9500,%rax; call *%rax
 *   1: hlt; jmp 1b
 */
static const unsigned char g_tramp[] = {
    0xfa, 0x31, 0xc0, 0x8e, 0xd8, 0x66, 0x0f, 0x01, 0x16, 0x00, 0x90, 0x0f,
    0x20, 0xe0, 0x66, 0x83, 0xc8, 0x20, 0x0f, 0x22, 0xe0, 0x66, 0xb8, 0x00,
    0x10, 0x00, 0x00, 0x0f, 0x22, 0xd8, 0x66, 0xb9, 0x80, 0x00, 0x00, 0xc0,
    0x0f, 0x32, 0x66, 0x0d, 0x00, 0x01, 0x00, 0x00, 0x0f, 0x30, 0x0f, 0x20,
    0xc0, 0x66, 0x0d, 0x01, 0x00, 0x00, 0x80, 0x0f, 0x22, 0xc0, 0x66, 0xea,
    0x50, 0xa0, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0xb8, 0x10, 0x00,
    0x8e, 0xd8, 0x8e, 0xc0, 0x8e, 0xd0, 0x48, 0x8b, 0x24, 0x25, 0x08, 0x95,
    0x00, 0x00, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x95, 0x00, 0x00, 0xff, 0xd0,
    0xf4, 0xeb, 0xfd
};

/* What the AP works on, set by vCPU 0 before the SIPI. */
static volatile uint64_t g_ap_lo, g_ap_hi, g_ap_seed, g_ap_rounds;

static uint64_t parse_hex(const char *s, int *ok) {
    uint64_t v = 0;
    int n = 0;
    if (s != 0 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (s != 0 && *s != '\0' && *s != ' ') {
        char c = *s++;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else { *ok = 0; return 0; }
        v = (v << 4) | d;
        n++;
    }
    *ok = (n > 0 && n <= 16);
    return v;
}

static uint64_t parse_dec(const char *s, int *ok) {
    uint64_t v = 0;
    int n = 0;
    while (s != 0 && *s >= '0' && *s <= '9') {
        v = v * 10u + (uint64_t)(*s++ - '0');
        n++;
    }
    *ok = (n > 0 && n < 10 && (s == 0 || *s == '\0' || *s == ' '));
    return v;
}

static inline uint64_t pat(uint64_t gpa, uint64_t seed, uint64_t round) {
    return (gpa * MIX) ^ seed ^ (round << 48);
}

static void write_range(uint64_t lo, uint64_t hi, uint64_t seed, uint64_t round) {
    uint64_t g;
    for (g = lo; g + PAGE <= hi; g += PAGE) {
        *(volatile uint64_t *)(uintptr_t)g = pat(g, seed, round);
        *(volatile uint64_t *)(uintptr_t)(g + PAGE - 8ull) = pat(g + PAGE - 8ull, seed, round);
    }
}

static int verify_range_quiet(uint64_t lo, uint64_t hi, uint64_t seed, uint64_t round,
                              volatile uint64_t *bad) {
    uint64_t g;
    for (g = lo; g + PAGE <= hi; g += PAGE) {
        if (*(volatile uint64_t *)(uintptr_t)g != pat(g, seed, round) ||
            *(volatile uint64_t *)(uintptr_t)(g + PAGE - 8ull) != pat(g + PAGE - 8ull, seed, round)) {
            *bad = g;
            return 0;
        }
    }
    return 1;
}

static int verify_range(uint64_t lo, uint64_t hi, uint64_t seed, uint64_t round) {
    uint64_t g;
    for (g = lo; g + PAGE <= hi; g += PAGE) {
        uint64_t a = *(volatile uint64_t *)(uintptr_t)g;
        uint64_t b = *(volatile uint64_t *)(uintptr_t)(g + PAGE - 8ull);
        if (a != pat(g, seed, round) || b != pat(g + PAGE - 8ull, seed, round)) {
            uint64_t other = a ^ (g * MIX) ^ (round << 48);
            micro_puts("micro/" NAME ": gpa ");
            micro_put_hex(g);
            micro_puts(" round ");
            micro_put_uint(round);
            micro_puts(" wanted ");
            micro_put_hex(pat(g, seed, round));
            micro_puts(" got ");
            micro_put_hex(a);
            micro_puts(" -- the pattern of seed ");
            micro_put_hex(other);
            micro_puts(other != seed ? " (ANOTHER guest's seed: cross-VM leak)\n" : "\n");
            return 0;
        }
    }
    return 1;
}

static void ap_main64(void) {
    volatile uint64_t *st = (volatile uint64_t *)(uintptr_t)AP_STATUS_GPA;
    uint64_t r;
    for (r = 1; r <= g_ap_rounds; r++) {
        write_range(g_ap_lo, g_ap_hi, g_ap_seed, r);
        if (!verify_range_quiet(g_ap_lo, g_ap_hi, g_ap_seed, r, &st[2])) {
            st[1] = 1;
            break;
        }
        st[0] = r;
    }
    for (;;) __asm__ volatile("hlt");
}

static void spin(unsigned long n) {
    volatile unsigned long i;
    for (i = 0; i < n; i++) {
    }
}

static void start_ap(uint64_t lo, uint64_t hi, uint64_t seed, uint64_t rounds) {
    volatile uint64_t *st = (volatile uint64_t *)(uintptr_t)AP_STATUS_GPA;
    unsigned int i;

    g_ap_lo = lo;
    g_ap_hi = hi;
    g_ap_seed = seed;
    g_ap_rounds = rounds;
    st[0] = 0;
    st[1] = 0;
    st[2] = 0;
    for (i = 0; i < (unsigned int)sizeof(g_tramp); i++) {
        *(volatile unsigned char *)(uintptr_t)(TRAMP_GPA + i) = g_tramp[i];
    }
    /* null, 64-bit code (L=1), flat data. Selector 0x08 = code, 0x10 = data. */
    *(volatile uint64_t *)(uintptr_t)(GDT_GPA + 0) = 0;
    *(volatile uint64_t *)(uintptr_t)(GDT_GPA + 8) = 0x00AF9A000000FFFFull;
    *(volatile uint64_t *)(uintptr_t)(GDT_GPA + 16) = 0x00CF92000000FFFFull;
    *(volatile uint16_t *)(uintptr_t)GDTR_GPA = (uint16_t)(24u - 1u);
    *(volatile uint32_t *)(uintptr_t)(GDTR_GPA + 2) = GDT_GPA;
    *(volatile uint64_t *)(uintptr_t)AP_ENTRY_GPA = (uint64_t)(uintptr_t)ap_main64;
    *(volatile uint64_t *)(uintptr_t)AP_STACK_GPA = AP_STACK_TOP;
    *(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + 0x310u) = 1u << 24;
    *(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + 0x300u) = 0x00004500u; /* INIT */
    spin(200000);
    *(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + 0x310u) = 1u << 24;
    *(volatile uint32_t *)(uintptr_t)(LAPIC_BASE + 0x300u) = 0x00004600u | (TRAMP_GPA >> 12);
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const unsigned char *zp = (const unsigned char *)(uintptr_t)zero_page_gpa;
    const char *cl = micro_cmdline(zero_page_gpa);
    const char *sv = micro_cmdline_value(cl, "seed");
    const char *rv = micro_cmdline_value(cl, "rounds");
    const char *mv = micro_cmdline_value(cl, "smp");
    uint64_t ram, seed, rounds = 20, r, top;
    int smp = 0;
    int ok = 1;

    micro_puts("\n");
    if (zero_page_gpa == 0ull || zp[0x1E8] == 0u) {
        micro_fail(NAME, "no zero page / no e820");
        micro_halt();
    }
    seed = parse_hex(sv, &ok);
    if (sv == 0 || !ok) {
        micro_fail(NAME, "needs seed=<hex> on the command line");
        micro_halt();
    }
    if (rv != 0) {
        rounds = parse_dec(rv, &ok);
        if (!ok || rounds == 0u) {
            micro_fail(NAME, "rounds= is not a positive decimal");
            micro_halt();
        }
    }
    {   /* The end of the highest usable (type 1) e820 entry: {addr, size, type} from 0x2D0. */
        unsigned i, n = zp[0x1E8];
        ram = 0;
        for (i = 0; i < n && i < 128u; i++) {
            uint64_t base = zero_page_gpa + 0x2D0ull + (uint64_t)i * 20ull;
            uint64_t a = *(const uint64_t *)(uintptr_t)base;
            uint64_t sz = *(const uint64_t *)(uintptr_t)(base + 8ull);
            uint32_t ty = *(const uint32_t *)(uintptr_t)(base + 16ull);
            if (ty == 1u && a + sz > ram && a < 0x100000000ull) ram = a + sz;
        }
    }
    if (ram <= HIGH_START + PAGE) {
        micro_fail(NAME, "too little RAM -- give this VM at least 32 mem_mb");
        micro_halt();
    }
    if (mv != 0) {
        if (mv[0] != '1' || (mv[1] != '\0' && mv[1] != ' ')) {
            micro_fail(NAME, "smp= must be 1");
            micro_halt();
        }
        smp = 1;
    }
    top = ram;
    if (smp) {
        uint64_t mid = (HIGH_START + (ram - HIGH_START) / 2u) & ~(PAGE - 1ull);
        start_ap(mid, ram, seed ^ 0x5555555555555555ull, rounds);
        top = mid;
        micro_puts("micro/" NAME ": vCPU 1 started on [");
        micro_put_hex(mid);
        micro_puts(", ");
        micro_put_hex(ram);
        micro_puts(")\n");
    }
    micro_puts("micro/" NAME ": seed ");
    micro_put_hex(seed);
    micro_puts(", ");
    micro_put_uint(ram / (1024ull * 1024ull));
    micro_puts(" MiB, ");
    micro_put_uint(rounds);
    micro_puts(" rounds\n");

    for (r = 1; r <= rounds; r++) {
        write_range(LOW_START, LOW_END, seed, r);
        write_range(HIGH_START, top, seed, r);
        if (!verify_range(LOW_START, LOW_END, seed, r) || !verify_range(HIGH_START, top, seed, r)) {
            micro_fail(NAME, "RAM did not read back this guest's own pattern");
            micro_halt();
        }
        if (r % 5u == 0u || r == rounds) {
            micro_puts("micro/" NAME ": round ");
            micro_put_uint(r);
            micro_puts(" of ");
            micro_put_uint(rounds);
            micro_puts(" verified\n");
        }
    }
    if (smp) {
        volatile uint64_t *st = (volatile uint64_t *)(uintptr_t)AP_STATUS_GPA;
        unsigned int w;
        for (w = 0; w < 6000u && st[0] < rounds && st[1] == 0u; w++) spin(1000000);
        micro_puts("micro/" NAME ": vCPU 1 verified ");
        micro_put_uint(st[0]);
        micro_puts(" of ");
        micro_put_uint(rounds);
        micro_puts(" rounds\n");
        if (st[1] != 0u) {
            micro_puts("micro/" NAME ": vCPU 1 mismatch at gpa ");
            micro_put_hex(st[2]);
            micro_puts("\n");
            micro_fail(NAME, "vCPU 1 did not read back its own pattern");
            micro_halt();
        }
        if (st[0] < rounds) {
            micro_fail(NAME, "vCPU 1 did not finish its rounds -- it never ran, or stopped");
            micro_halt();
        }
    }
    micro_puts("micro/" NAME ": ");
    micro_put_uint(rounds);
    micro_puts(" rounds written and verified with seed ");
    micro_put_hex(seed);
    micro_puts(", no foreign pattern seen\n");
    micro_pass(NAME);
    micro_halt();
}
