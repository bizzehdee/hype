#include <stdio.h>
#include <string.h>

#include "../pe_ident.h"

static int failures = 0;
#define CHECK(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)
#define CHECK_INT(desc, expected, actual)                                                    \
    do {                                                                                     \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                      \
        if (e_ != a_) {                                                                      \
            printf("FAIL: %s (expected %lld, got %lld)\n", (desc), e_, a_);                  \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

/* A synthetic guest address space: one region of bytes at a chosen base. */
#define MEM_BASE 0x7f440000ull
#define MEM_SIZE 0x40000u
static unsigned char g_mem[MEM_SIZE];

static long g_read_countdown = -1; /* if >=0, fail the read that hits 0 */

static int mem_read(void *ctx, uint64_t va, void *dst, uint64_t len) {
    (void)ctx;
    if (g_read_countdown >= 0) { if (g_read_countdown-- == 0) return -1; }
    if (va < MEM_BASE || va + len > MEM_BASE + MEM_SIZE) return -1; /* unmapped */
    memcpy(dst, g_mem + (va - MEM_BASE), (size_t)len);
    return 0;
}

static void put16(unsigned int off, uint16_t v) {
    g_mem[off] = (unsigned char)v; g_mem[off + 1] = (unsigned char)(v >> 8);
}
static void put32(unsigned int off, uint32_t v) {
    g_mem[off] = (unsigned char)v; g_mem[off + 1] = (unsigned char)(v >> 8);
    g_mem[off + 2] = (unsigned char)(v >> 16); g_mem[off + 3] = (unsigned char)(v >> 24);
}

/* Build a PE32+ image at page-aligned offset `off` with SizeOfImage `size`.
 * If `pdb` is non-null, add a debug directory with a CodeView RSDS record. */
static void build_pe(unsigned int off, uint32_t size, const char *pdb) {
    unsigned int lfanew = 0x80u;
    unsigned int opt = off + lfanew + 0x18u;
    unsigned int dirs = opt + 0x70u;
    unsigned int dbg_rva, cv_rva;

    put16(off, 0x5A4Du);                 /* "MZ" */
    put32(off + 0x3Cu, lfanew);
    put32(off + lfanew, 0x00004550u);    /* "PE\0\0" */
    put16(opt, 0x20Bu);                  /* PE32+ */
    put32(opt + 0x38u, size);            /* SizeOfImage */

    if (pdb == 0) return;
    dbg_rva = 0x1000u;
    cv_rva = 0x2000u;
    put32(dirs + 6u * 8u, dbg_rva);      /* debug directory RVA */
    put32(dirs + 6u * 8u + 4u, 28u);     /* one entry */
    put32(off + dbg_rva + 12u, 2u);      /* type = CodeView */
    put32(off + dbg_rva + 20u, cv_rva);  /* AddressOfRawData */
    put32(off + cv_rva, 0x53445352u);    /* "RSDS" */
    memcpy(g_mem + off + cv_rva + 24u, pdb, strlen(pdb) + 1u);
}

static void test_finds_the_image_containing_the_address(void) {
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x8000u, 0);
    /* An address well inside the image resolves to its base. */
    CHECK_INT("base found", MEM_BASE + 0x10000u,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x14321u));
    /* The base itself resolves to itself. */
    CHECK_INT("base resolves to itself", MEM_BASE + 0x10000u,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x10000u));
}

/*
 * The size check is the point. Without it the backward scan returns whatever
 * image happens to sit below the address -- a confidently wrong answer, which
 * is worse than no answer when the whole purpose is naming a module.
 */
static void test_address_beyond_the_image_is_not_attributed(void) {
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x2000u, 0); /* image covers 0x10000..0x12000 only */
    CHECK_INT("address past the image is refused", 0,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x13000u));
}

static void test_no_image_at_all(void) {
    memset(g_mem, 0, sizeof(g_mem));
    CHECK_INT("nothing found in blank memory", 0,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x8000u));
}

/* Garbage that merely starts with "MZ" must not be accepted -- firmware memory
 * is full of coincidental byte pairs. */
static void test_mz_without_a_valid_pe_header_is_rejected(void) {
    memset(g_mem, 0, sizeof(g_mem));
    put16(0x10000u, 0x5A4Du);
    put32(0x10000u + 0x3Cu, 0x80u);
    put32(0x10000u + 0x80u, 0xDEADBEEFu); /* not "PE\0\0" */
    CHECK_INT("bogus MZ rejected", 0,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x10100u));
}

static void test_wild_lfanew_is_rejected(void) {
    memset(g_mem, 0, sizeof(g_mem));
    put16(0x10000u, 0x5A4Du);
    put32(0x10000u + 0x3Cu, 0x7FFFFFFFu); /* absurd e_lfanew */
    CHECK_INT("wild e_lfanew rejected", 0,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x10100u));
}

static void test_module_name_from_codeview(void) {
    char name[32];
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x8000u, "d:\\build\\X64\\DxeCore.pdb");
    CHECK_INT("name extracted", 0,
              hype_pe_module_name(mem_read, 0, MEM_BASE + 0x10000u, name, sizeof(name)));
    CHECK_INT("basename, no extension", 0, strcmp(name, "DxeCore"));
}

static void test_module_name_handles_forward_slashes_and_no_directory(void) {
    char name[32];
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x8000u, "/home/x/loader.pdb");
    CHECK_INT("ok", 0, hype_pe_module_name(mem_read, 0, MEM_BASE + 0x10000u, name, sizeof(name)));
    CHECK_INT("posix path", 0, strcmp(name, "loader"));

    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x8000u, "bare.pdb");
    CHECK_INT("ok2", 0, hype_pe_module_name(mem_read, 0, MEM_BASE + 0x10000u, name, sizeof(name)));
    CHECK_INT("no directory part", 0, strcmp(name, "bare"));
}

/* A release image with no debug record is a legitimate outcome. The caller must
 * be told so it can report the base address rather than invent a name. */
static void test_missing_debug_record_reports_failure(void) {
    char name[32];
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x8000u, 0);
    CHECK_INT("no debug directory -> -1", -1,
              hype_pe_module_name(mem_read, 0, MEM_BASE + 0x10000u, name, sizeof(name)));
    CHECK_INT("output left empty", 0u, (unsigned int)strlen(name));
}

static void test_name_truncates_into_a_small_buffer(void) {
    char name[5];
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x8000u, "VeryLongModuleName.pdb");
    CHECK_INT("ok", 0, hype_pe_module_name(mem_read, 0, MEM_BASE + 0x10000u, name, sizeof(name)));
    CHECK_INT("truncated to fit", 4u, (unsigned int)strlen(name));
    CHECK_INT("prefix kept", 0, strncmp(name, "Very", 4));
}

static void test_null_safety(void) {
    char name[8];
    CHECK_INT("null reader (find)", 0, (long long)hype_pe_find_image_base(0, 0, 0x1000));
    CHECK_INT("null reader (name)", -1, hype_pe_module_name(0, 0, 0x1000, name, sizeof(name)));
    CHECK_INT("null out", -1, hype_pe_module_name(mem_read, 0, MEM_BASE, 0, 8));
    CHECK_INT("zero out size", -1, hype_pe_module_name(mem_read, 0, MEM_BASE, name, 0));
}

/* PE32 (not PE32+) puts the data directories 0x10 earlier, because of the extra
 * BaseOfData field. Getting that wrong reads the wrong directory entirely. */
static void test_pe32_data_directory_offset(void) {
    char name[32];
    unsigned int off = 0x10000u, lfanew = 0x80u;
    unsigned int opt = off + lfanew + 0x18u;
    unsigned int dirs = opt + 0x60u; /* PE32 layout */
    memset(g_mem, 0, sizeof(g_mem));
    put16(off, 0x5A4Du);
    put32(off + 0x3Cu, lfanew);
    put32(off + lfanew, 0x00004550u);
    put16(opt, 0x10Bu);              /* PE32 */
    put32(opt + 0x38u, 0x8000u);
    put32(dirs + 6u * 8u, 0x1000u);
    put32(dirs + 6u * 8u + 4u, 28u);
    put32(off + 0x1000u + 12u, 2u);
    put32(off + 0x1000u + 20u, 0x2000u);
    put32(off + 0x2000u, 0x53445352u);
    memcpy(g_mem + off + 0x2000u + 24u, "PE32Mod.pdb", 12);

    CHECK_INT("pe32 base found", MEM_BASE + off,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + off + 0x100u));
    CHECK_INT("pe32 name ok", 0,
              hype_pe_module_name(mem_read, 0, MEM_BASE + off, name, sizeof(name)));
    CHECK_INT("pe32 name", 0, strcmp(name, "PE32Mod"));
}

/* An optional-header magic that is neither PE32 nor PE32+ is not a PE image. */
static void test_bad_optional_magic_rejected(void) {
    memset(g_mem, 0, sizeof(g_mem));
    put16(0x10000u, 0x5A4Du);
    put32(0x10000u + 0x3Cu, 0x80u);
    put32(0x10000u + 0x80u, 0x00004550u);
    put16(0x10000u + 0x80u + 0x18u, 0x1234u); /* bogus magic */
    CHECK_INT("bad optional magic rejected", 0,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x10100u));
}

static void test_zero_size_of_image_rejected(void) {
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0u, 0);
    CHECK_INT("SizeOfImage 0 rejected", 0,
              (long long)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x10100u));
}

/* Debug entries that are not CodeView must be skipped, not misread. */
static void test_non_codeview_debug_entries_are_skipped(void) {
    char name[32];
    unsigned int off = 0x10000u, dirs, opt;
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(off, 0x8000u, 0);
    opt = off + 0x80u + 0x18u;
    dirs = opt + 0x70u;
    put32(dirs + 6u * 8u, 0x1000u);
    put32(dirs + 6u * 8u + 4u, 56u);          /* two entries */
    put32(off + 0x1000u + 12u, 16u);          /* entry 0: not CodeView */
    put32(off + 0x1000u + 28u + 12u, 2u);     /* entry 1: CodeView */
    put32(off + 0x1000u + 28u + 20u, 0x2000u);
    put32(off + 0x2000u, 0x53445352u);
    memcpy(g_mem + off + 0x2000u + 24u, "Second.pdb", 11);
    CHECK_INT("second entry used", 0,
              hype_pe_module_name(mem_read, 0, MEM_BASE + off, name, sizeof(name)));
    CHECK_INT("name from the CodeView entry", 0, strcmp(name, "Second"));
}

/* A CodeView record that is not RSDS (e.g. older NB10) carries no usable name. */
static void test_non_rsds_codeview_rejected(void) {
    char name[32];
    unsigned int off = 0x10000u, dirs, opt;
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(off, 0x8000u, 0);
    opt = off + 0x80u + 0x18u;
    dirs = opt + 0x70u;
    put32(dirs + 6u * 8u, 0x1000u);
    put32(dirs + 6u * 8u + 4u, 28u);
    put32(off + 0x1000u + 12u, 2u);
    put32(off + 0x1000u + 20u, 0x2000u);
    put32(off + 0x2000u, 0x3031424Eu); /* "NB10" */
    CHECK_INT("non-RSDS rejected", -1,
              hype_pe_module_name(mem_read, 0, MEM_BASE + off, name, sizeof(name)));
}

static void test_empty_pdb_path_rejected(void) {
    char name[32];
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(0x10000u, 0x8000u, "");
    CHECK_INT("empty path rejected", -1,
              hype_pe_module_name(mem_read, 0, MEM_BASE + 0x10000u, name, sizeof(name)));
}

/* A zero debug-directory RVA means no debug data at all. */
static void test_zero_debug_rva(void) {
    char name[32];
    unsigned int off = 0x10000u, opt, dirs;
    memset(g_mem, 0, sizeof(g_mem));
    build_pe(off, 0x8000u, 0);
    opt = off + 0x80u + 0x18u;
    dirs = opt + 0x70u;
    put32(dirs + 6u * 8u, 0u);
    put32(dirs + 6u * 8u + 4u, 28u);
    CHECK_INT("zero rva -> -1", -1,
              hype_pe_module_name(mem_read, 0, MEM_BASE + off, name, sizeof(name)));
}

/* An address in unmapped memory: every read fails and the scan must stop
 * cleanly rather than spin to its bound and fault. */
static void test_unmapped_address(void) {
    char name[32];
    memset(g_mem, 0, sizeof(g_mem));
    CHECK_INT("unmapped find", 0, (long long)hype_pe_find_image_base(mem_read, 0, 0x10000000ull));
    CHECK_INT("unmapped name", -1, hype_pe_module_name(mem_read, 0, 0x10000000ull, name,
                                                       sizeof(name)));
}

/* Scanning down from a low address must stop at zero, not wrap around. */
static void test_scan_stops_at_low_addresses(void) {
    memset(g_mem, 0, sizeof(g_mem));
    CHECK_INT("no wrap below zero", 0, (long long)hype_pe_find_image_base(mem_read, 0, 0x800ull));
}

/*
 * Sweep every read in both entry points with an injected failure, one position
 * at a time. Guest memory is not guaranteed readable at any of these offsets --
 * a page boundary or an unmapped hole can land anywhere -- so each of these
 * paths is reachable in practice, and none of them may fault or produce a name
 * from partly-read data.
 */
static void test_read_failures_at_every_position_are_survived(void) {
    char name[32];
    long n;
    for (n = 0; n < 40; n++) {
        memset(g_mem, 0, sizeof(g_mem));
        build_pe(0x10000u, 0x8000u, "d:\\b\\Sweep.pdb");
        g_read_countdown = n;
        (void)hype_pe_find_image_base(mem_read, 0, MEM_BASE + 0x10100u);
        g_read_countdown = -1;

        memset(g_mem, 0, sizeof(g_mem));
        build_pe(0x10000u, 0x8000u, "d:\\b\\Sweep.pdb");
        name[0] = 'x';
        g_read_countdown = n;
        if (hype_pe_module_name(mem_read, 0, MEM_BASE + 0x10000u, name, sizeof(name)) == 0) {
            /* If it claims success, the name must be the real one -- never a
             * fragment assembled from a truncated read. */
            CHECK_INT("name on success is complete", 0, strcmp(name, "Sweep"));
        }
        g_read_countdown = -1;
    }
}

int main(void) {
    test_finds_the_image_containing_the_address();
    test_address_beyond_the_image_is_not_attributed();
    test_no_image_at_all();
    test_mz_without_a_valid_pe_header_is_rejected();
    test_wild_lfanew_is_rejected();
    test_module_name_from_codeview();
    test_module_name_handles_forward_slashes_and_no_directory();
    test_missing_debug_record_reports_failure();
    test_name_truncates_into_a_small_buffer();
    test_null_safety();
    test_pe32_data_directory_offset();
    test_bad_optional_magic_rejected();
    test_zero_size_of_image_rejected();
    test_non_codeview_debug_entries_are_skipped();
    test_non_rsds_codeview_rejected();
    test_empty_pdb_path_rejected();
    test_zero_debug_rva();
    test_unmapped_address();
    test_scan_stops_at_low_addresses();
    test_read_failures_at_every_position_are_survived();
    if (failures != 0) {
        printf("test_pe_ident: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_pe_ident: all checks passed\n");
    return 0;
}
