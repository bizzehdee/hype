#include "png_write.h"

#define HYPE_PNG_SIGNATURE_LEN 8u
#define HYPE_PNG_CHUNK_OVERHEAD 12u /* length(4) + type(4) + crc(4), data excluded */
#define HYPE_PNG_IHDR_DATA_LEN 13u
#define HYPE_DEFLATE_STORED_MAX 65535u /* RFC 1951 3.2.4: LEN is a 16-bit field */
#define HYPE_DEFLATE_BLOCK_HEADER_LEN 5u /* 1 (BFINAL/BTYPE, byte-aligned) + LEN(2) + NLEN(2) */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    int overflowed;
} hype_png_w_t;

static void w_init(hype_png_w_t *w, uint8_t *buf, uint32_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflowed = 0;
}

static void w_bytes(hype_png_w_t *w, const uint8_t *src, uint32_t n) {
    uint32_t i;
    if (w->overflowed) {
        return;
    }
    if (n > w->cap - w->len) {
        w->overflowed = 1;
        return;
    }
    for (i = 0; i < n; i++) {
        w->buf[w->len + i] = src[i];
    }
    w->len += n;
}

static void w_u8(hype_png_w_t *w, uint8_t v) {
    w_bytes(w, &v, 1u);
}

static void w_u32_be(hype_png_w_t *w, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
    w_bytes(w, b, 4u);
}

/* Standard IEEE 802.3 CRC-32 (the PNG spec's own choice, Annex D), reflected
 * table-free bit-at-a-time form -- clear and correct over a fast lookup
 * table, which this one-shot per-screenshot encoder has no need of. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len) {
    uint32_t i, j;
    crc = ~crc;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8u; j++) {
            uint32_t mask = (uint32_t) - (int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* Writes one PNG chunk: length, type, data, CRC32(type+data). `type` is
 * exactly 4 ASCII bytes. */
static void w_chunk(hype_png_w_t *w, const char *type, const uint8_t *data, uint32_t data_len) {
    uint32_t crc;
    w_u32_be(w, data_len);
    w_bytes(w, (const uint8_t *)type, 4u);
    if (data_len > 0u) {
        w_bytes(w, data, data_len);
    }
    crc = crc32_update(0, (const uint8_t *)type, 4u);
    if (data_len > 0u) {
        crc = crc32_update(crc, data, data_len);
    }
    w_u32_be(w, crc);
}

/*
 * #463: the WORST case for a fixed-Huffman block -- every byte coded as a 9-bit literal (which
 * only happens for bytes >= 144 with no runs at all), plus the 3-bit block header and the 7-bit
 * end-of-block symbol.
 *
 * A screen capture never comes close: RLE over a text console compresses by two or three orders
 * of magnitude. But the OUTPUT BUFFER has to be sized for the worst case, not the typical one,
 * or a pathological image silently fails to encode.
 *
 * `raw_len` is always >= 1 here: every caller derives it as height*(1+width*3) with width and
 * height already validated nonzero by hype_png_write().
 */
static uint32_t deflate_fixed_max_len(uint32_t raw_len) {
    return (raw_len * 9u + 3u + 7u + 7u) / 8u;
}

uint32_t hype_png_encoded_size(uint32_t width, uint32_t height) {
    uint32_t raw_len, zlib_len, idat_data_len;
    if (width == 0u || height == 0u) {
        return 0u;
    }
    raw_len = height * (1u + width * 3u); /* per-row filter byte + RGB888 */
    zlib_len = 2u /* zlib header */ + deflate_fixed_max_len(raw_len) + 4u /* adler32 */;
    idat_data_len = zlib_len;
    return HYPE_PNG_SIGNATURE_LEN + (HYPE_PNG_CHUNK_OVERHEAD + HYPE_PNG_IHDR_DATA_LEN) +
           (HYPE_PNG_CHUNK_OVERHEAD + idat_data_len) + (HYPE_PNG_CHUNK_OVERHEAD + 0u);
}

/*
 * #463: bit-level output for a fixed-Huffman DEFLATE block.
 *
 * DEFLATE packs bits LSB-first within each byte, EXCEPT Huffman codes, which are packed
 * most-significant-bit-first (RFC 1951 section 3.1.1). That asymmetry is the classic way to get
 * this wrong, so the two cases are separate functions rather than one with a flag.
 */
typedef struct {
    hype_png_w_t *w;
    uint32_t bits;  /* accumulated, LSB-first */
    unsigned nbits; /* how many of `bits` are valid */
} hype_deflate_bw_t;

static void bw_init(hype_deflate_bw_t *b, hype_png_w_t *w) {
    b->w = w;
    b->bits = 0;
    b->nbits = 0;
}

/* Raw bits, LSB-first: block headers and Huffman "extra" bits. */
static void bw_bits(hype_deflate_bw_t *b, uint32_t value, unsigned count) {
    b->bits |= (value & ((1u << count) - 1u)) << b->nbits;
    b->nbits += count;
    while (b->nbits >= 8u) {
        w_u8(b->w, (uint8_t)(b->bits & 0xFFu));
        b->bits >>= 8;
        b->nbits -= 8u;
    }
}

/* A Huffman code: emitted MSB-first, so reverse it into the LSB-first stream. */
static void bw_code(hype_deflate_bw_t *b, uint32_t code, unsigned count) {
    unsigned i;
    for (i = 0; i < count; i++) {
        bw_bits(b, (code >> (count - 1u - i)) & 1u, 1u);
    }
}

static void bw_flush(hype_deflate_bw_t *b) {
    if (b->nbits > 0u) {
        w_u8(b->w, (uint8_t)(b->bits & 0xFFu));
        b->bits = 0;
        b->nbits = 0;
    }
}

/* Fixed literal/length code lengths and values, RFC 1951 section 3.2.6. */
static void bw_literal(hype_deflate_bw_t *b, uint8_t byte) {
    if (byte < 144u) {
        bw_code(b, 0x30u + byte, 8u);
    } else {
        bw_code(b, 0x190u + (byte - 144u), 9u);
    }
}

static void bw_end_of_block(hype_deflate_bw_t *b) {
    bw_code(b, 0u, 7u); /* symbol 256, in the 7-bit range 0x00-0x17 */
}

/*
 * Length codes 257-285 and their extra bits (RFC 1951 section 3.2.5, table). Only lengths 3-258
 * occur, which the caller guarantees.
 */
static void bw_length(hype_deflate_bw_t *b, uint32_t len) {
    static const uint16_t base[] = {3,   4,   5,   6,   7,   8,   9,   10,  11,  13,
                                    15,  17,  19,  23,  27,  31,  35,  43,  51,  59,
                                    67,  83,  99,  115, 131, 163, 195, 227, 258};
    static const uint8_t extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    unsigned i = 28u; /* 258 is its own code (285) with no extra bits */

    while (i > 0u && base[i] > len) {
        i--;
    }
    {
        unsigned sym = 257u + i;
        /* Symbols 257-279 are 7 bits (0x00-0x17); 280-287 are 8 bits (0xC0-0xC7). */
        if (sym < 280u) {
            bw_code(b, sym - 256u, 7u);
        } else {
            bw_code(b, 0xC0u + (sym - 280u), 8u);
        }
        if (extra[i] > 0u) {
            bw_bits(b, len - base[i], extra[i]);
        }
    }
}

/* Distance 1 only -- this encoder emits nothing else. Fixed distance codes are a flat 5-bit
 * code, and distance 1 is code 0 with no extra bits. */
static void bw_distance_one(hype_deflate_bw_t *b) {
    bw_code(b, 0u, 5u);
}

/*
 * #463: run-length encode `byte` repeated `run` times into the fixed-Huffman block.
 *
 * One literal, then back-references at distance 1 -- the standard RLE idiom, and legal because a
 * DEFLATE match may overlap its own output. A match needs length >= 3, so the 1-2 byte tail is
 * emitted as literals.
 */
static void bw_run(hype_deflate_bw_t *b, uint8_t byte, uint32_t run) {
    uint32_t rest;

    bw_literal(b, byte);
    rest = run - 1u;
    while (rest >= 3u) {
        uint32_t take = (rest > 258u) ? 258u : rest;
        /* Never leave a remainder of 1 or 2 when a longer match could have avoided it -- those
         * cost 8-9 bits each as literals, against ~12 bits for the whole match. */
        if (rest - take == 1u || rest - take == 2u) {
            take -= 3u;
        }
        bw_length(b, take);
        bw_distance_one(b);
        rest -= take;
    }
    while (rest > 0u) {
        bw_literal(b, byte);
        rest--;
    }
}

/* Writes the zlib-wrapped, fixed-Huffman-DEFLATE IDAT payload directly into `w`: a zlib header,
 * one fixed-Huffman block RLE-coding the raw scanline bytes, then the Adler-32 of the raw
 * stream. Rows are generated on the fly (filter byte + RGB888) so the whole image is never
 * assembled contiguously. */
static void write_idat_payload(hype_png_w_t *w, const uint8_t *rgb, uint32_t width, uint32_t height,
                               uint32_t stride_bytes) {
    uint32_t raw_len = height * (1u + width * 3u);
    uint32_t row_bytes = width * 3u;
    uint32_t consumed = 0;
    uint32_t row = 0;
    uint32_t byte_in_row = 0; /* 0 = the filter byte is next; else offset into pixel bytes */
    uint32_t adler_s1 = 1u, adler_s2 = 0u;
    hype_deflate_bw_t bw;
    uint32_t run = 0;
    uint8_t run_byte = 0;

    /* zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (no preset dict, check bits valid
     * for CMF/FLG as a big-endian 16-bit value -- 0x7801 % 31 == 0, the required property). */
    w_u8(w, 0x78u);
    w_u8(w, 0x01u);

    bw_init(&bw, w);
    bw_bits(&bw, 1u, 1u); /* BFINAL = 1: one block for the whole image */
    bw_bits(&bw, 1u, 2u); /* BTYPE = 01: fixed Huffman */

    while (consumed < raw_len) {
        uint8_t b;
        if (byte_in_row == 0u) {
            b = 0u; /* PNG filter type 0: None */
            byte_in_row = 1u;
        } else {
            const uint8_t *src_row = rgb + (uint64_t)row * stride_bytes;
            b = src_row[byte_in_row - 1u];
            byte_in_row++;
            if (byte_in_row - 1u == row_bytes) {
                byte_in_row = 0u;
                row++;
            }
        }
        adler_s1 = (adler_s1 + b) % 65521u;
        adler_s2 = (adler_s2 + adler_s1) % 65521u;
        consumed++;

        if (run > 0u && b == run_byte) {
            run++;
        } else {
            if (run > 0u) {
                bw_run(&bw, run_byte, run);
            }
            run_byte = b;
            run = 1u;
        }
        if (w->overflowed) {
            return; /* the caller reports the failure; do not keep encoding into nothing */
        }
    }
    if (run > 0u) {
        bw_run(&bw, run_byte, run);
    }
    bw_end_of_block(&bw);
    bw_flush(&bw);

    w_u32_be(w, (adler_s2 << 16) | adler_s1);
}

uint32_t hype_png_write(const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t stride_bytes,
                        uint8_t *out, uint32_t out_cap) {
    static const uint8_t signature[HYPE_PNG_SIGNATURE_LEN] = {0x89, 0x50, 0x4E, 0x47,
                                                              0x0D, 0x0A, 0x1A, 0x0A};
    hype_png_w_t w;

    if (rgb == 0 || out == 0 || width == 0u || height == 0u || stride_bytes < width * 3u) {
        return 0u;
    }
    /*
     * No separate "does it fit" precheck against hype_png_encoded_size() here -- w_bytes()'s
     * own incremental bounds check below is authoritative and catches an undersized `out_cap`
     * exactly the same way, so a second check would either be redundant (when the two agree,
     * the only case that can occur) or dead code (unreachable, since they always agree).
     */
    w_init(&w, out, out_cap);
    w_bytes(&w, signature, HYPE_PNG_SIGNATURE_LEN);

    {
        uint8_t ihdr[HYPE_PNG_IHDR_DATA_LEN];
        ihdr[0] = (uint8_t)(width >> 24);
        ihdr[1] = (uint8_t)(width >> 16);
        ihdr[2] = (uint8_t)(width >> 8);
        ihdr[3] = (uint8_t)width;
        ihdr[4] = (uint8_t)(height >> 24);
        ihdr[5] = (uint8_t)(height >> 16);
        ihdr[6] = (uint8_t)(height >> 8);
        ihdr[7] = (uint8_t)height;
        ihdr[8] = 8u;  /* bit depth */
        ihdr[9] = 2u;  /* color type: truecolor, no alpha */
        ihdr[10] = 0u; /* compression method: only value the spec defines */
        ihdr[11] = 0u; /* filter method: only value the spec defines */
        ihdr[12] = 0u; /* interlace: none */
        w_chunk(&w, "IHDR", ihdr, HYPE_PNG_IHDR_DATA_LEN);
    }

    /*
     * #463: the compressed payload's length is data-dependent, so it cannot be computed up front
     * the way the stored encoder's could. Reserve the length field, write the payload, then
     * back-patch it -- which also keeps the encoder single-pass and streaming.
     */
    {
        uint32_t len_pos = w.len;
        uint32_t crc_start;
        w_u32_be(&w, 0u); /* placeholder, patched below */
        w_bytes(&w, (const uint8_t *)"IDAT", 4u);
        crc_start = w.len;
        write_idat_payload(&w, rgb, width, height, stride_bytes);
        if (!w.overflowed) {
            uint32_t idat_len = w.len - crc_start;
            uint32_t crc;
            w.buf[len_pos + 0u] = (uint8_t)(idat_len >> 24);
            w.buf[len_pos + 1u] = (uint8_t)(idat_len >> 16);
            w.buf[len_pos + 2u] = (uint8_t)(idat_len >> 8);
            w.buf[len_pos + 3u] = (uint8_t)idat_len;
            crc = crc32_update(0, (const uint8_t *)"IDAT", 4u);
            crc = crc32_update(crc, w.buf + crc_start, idat_len);
            w_u32_be(&w, crc);
        }
    }

    w_chunk(&w, "IEND", 0, 0u);

    return w.overflowed ? 0u : w.len;
}
