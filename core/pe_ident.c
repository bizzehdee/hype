#include "pe_ident.h"

#define DOS_MAGIC 0x5A4Du     /* "MZ" */
#define PE_SIGNATURE 0x4550u  /* "PE" (followed by two zero bytes) */
#define RSDS_SIGNATURE 0x53445352u /* "RSDS" */

/* PE32+ offsets used here, from the COFF/PE specification. */
#define DOS_LFANEW_OFF 0x3Cu
#define COFF_OPTIONAL_MAGIC_OFF 0x18u /* relative to the PE signature */
#define PE32PLUS_MAGIC 0x20Bu
#define PE32_MAGIC 0x10Bu

static int rd16(hype_pe_read_fn read, void *ctx, uint64_t va, uint16_t *out) {
    uint8_t b[2];
    if (read(ctx, va, b, 2) != 0) return -1;
    *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return 0;
}

static int rd32(hype_pe_read_fn read, void *ctx, uint64_t va, uint32_t *out) {
    uint8_t b[4];
    if (read(ctx, va, b, 4) != 0) return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

/* The image's total mapped size (SizeOfImage), or 0 if this is not a PE image.
 * Also yields the offset of the optional header, which the debug-directory
 * lookup needs. */
static uint32_t pe_image_size(hype_pe_read_fn read, void *ctx, uint64_t base, uint64_t *pe_off,
                              uint16_t *opt_magic) {
    uint16_t mz = 0, magic = 0;
    uint32_t lfanew = 0, sig = 0, size = 0;

    if (rd16(read, ctx, base, &mz) != 0 || mz != DOS_MAGIC) return 0;
    if (rd32(read, ctx, base + DOS_LFANEW_OFF, &lfanew) != 0) return 0;
    /* A wild e_lfanew would send the reads far outside the image; UEFI headers
     * are small, so bound it rather than trusting the field. */
    if (lfanew < 0x40u || lfanew > 0x400u) return 0;
    if (rd32(read, ctx, base + lfanew, &sig) != 0) return 0;
    if ((sig & 0xFFFFu) != PE_SIGNATURE || (sig >> 16) != 0u) return 0;
    if (rd16(read, ctx, base + lfanew + COFF_OPTIONAL_MAGIC_OFF, &magic) != 0) return 0;
    if (magic != PE32PLUS_MAGIC && magic != PE32_MAGIC) return 0;
    /* SizeOfImage sits at optional-header offset 0x38 in both PE32 and PE32+. */
    if (rd32(read, ctx, base + lfanew + COFF_OPTIONAL_MAGIC_OFF + 0x38u, &size) != 0) return 0;
    if (size == 0u) return 0;
    if (pe_off != 0) *pe_off = lfanew;
    if (opt_magic != 0) *opt_magic = magic;
    return size;
}

uint64_t hype_pe_find_image_base(hype_pe_read_fn read, void *ctx, uint64_t addr) {
    uint64_t page;
    unsigned int steps;

    if (read == 0) return 0;
    page = addr & ~(uint64_t)(HYPE_PE_SCAN_STEP - 1u);
    for (steps = 0; steps < HYPE_PE_SCAN_MAX_PAGES; steps++) {
        uint32_t size = pe_image_size(read, ctx, page, 0, 0);
        if (size != 0u) {
            /* Only accept an image that actually covers the address. Without
             * this the scan returns the first unrelated image below it, which
             * is a confidently wrong answer rather than no answer. */
            if (addr < page + (uint64_t)size) return page;
            return 0;
        }
        if (page < HYPE_PE_SCAN_STEP) break;
        page -= HYPE_PE_SCAN_STEP;
    }
    return 0;
}

int hype_pe_module_name(hype_pe_read_fn read, void *ctx, uint64_t base, char *out,
                        unsigned int out_size) {
    uint64_t pe_off = 0;
    uint16_t opt_magic = 0;
    uint32_t dir_rva = 0, dir_size = 0;
    uint32_t i;
    uint64_t data_dirs;

    if (read == 0 || out == 0 || out_size == 0u) return -1;
    out[0] = '\0';
    if (pe_image_size(read, ctx, base, &pe_off, &opt_magic) == 0u) return -1;

    /* The data directories follow the optional header; the debug directory is
     * entry 6. Their offset differs between PE32 and PE32+ because PE32 carries
     * an extra BaseOfData field. */
    data_dirs = base + pe_off + COFF_OPTIONAL_MAGIC_OFF + ((opt_magic == PE32PLUS_MAGIC) ? 0x70u
                                                                                        : 0x60u);
    if (rd32(read, ctx, data_dirs + 6u * 8u, &dir_rva) != 0) return -1;
    if (rd32(read, ctx, data_dirs + 6u * 8u + 4u, &dir_size) != 0) return -1;
    if (dir_rva == 0u || dir_size == 0u) return -1;

    /* Walk the debug directory entries looking for CodeView (type 2). */
    for (i = 0; i + 28u <= dir_size; i += 28u) {
        uint32_t type = 0, data_rva = 0, cv_sig = 0;
        uint64_t cv;
        if (rd32(read, ctx, base + dir_rva + i + 12u, &type) != 0) return -1;
        if (type != 2u) continue;
        if (rd32(read, ctx, base + dir_rva + i + 20u, &data_rva) != 0) return -1;
        if (data_rva == 0u) continue;
        cv = base + data_rva;
        if (rd32(read, ctx, cv, &cv_sig) != 0) return -1;
        if (cv_sig != RSDS_SIGNATURE) continue;
        {
            /* RSDS: 4-byte signature, 16-byte GUID, 4-byte age, then a NUL-
             * terminated path. Take the basename, drop the extension. */
            uint64_t p = cv + 24u;
            char path[128];
            unsigned int n = 0;
            unsigned int start = 0;
            unsigned int end;
            int terminated = 0;
            while (n + 1u < sizeof(path)) {
                uint8_t c;
                /*
                 * A failed read here is NOT a string terminator. Treating it as
                 * one yields a name assembled from partly-read memory and
                 * reports it as success -- a module confidently misnamed, which
                 * is worse than no name at all and is the exact failure this
                 * module exists to avoid. Found by a test that injects a read
                 * failure at every position in turn.
                 */
                if (read(ctx, p + n, &c, 1) != 0) return -1;
                if (c == 0u) { terminated = 1; break; }
                path[n] = (char)c;
                n++;
            }
            path[n] = '\0';
            if (n == 0u || !terminated) return -1;
            for (end = 0; end < n; end++) {
                if (path[end] == '\\' || path[end] == '/') start = end + 1u;
            }
            end = n;
            {
                unsigned int k;
                for (k = start; k < n; k++) {
                    if (path[k] == '.') { end = k; break; }
                }
            }
            {
                unsigned int w = 0;
                for (; start < end && w + 1u < out_size; start++, w++) out[w] = path[start];
                out[w] = '\0';
                return (w == 0u) ? -1 : 0;
            }
        }
    }
    return -1;
}
