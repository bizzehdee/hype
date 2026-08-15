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

/* `raw_len` is always >= 1 here in practice: every caller derives it as
 * height*(1+width*3) with width/height already validated nonzero by
 * hype_png_write(), so there is no empty-input case to special-case. */
static uint32_t deflate_stored_len(uint32_t raw_len) {
    uint32_t blocks = (raw_len + HYPE_DEFLATE_STORED_MAX - 1u) / HYPE_DEFLATE_STORED_MAX;
    return blocks * HYPE_DEFLATE_BLOCK_HEADER_LEN + raw_len;
}

uint32_t hype_png_encoded_size(uint32_t width, uint32_t height) {
    uint32_t raw_len, zlib_len, idat_data_len;
    if (width == 0u || height == 0u) {
        return 0u;
    }
    raw_len = height * (1u + width * 3u); /* per-row filter byte + RGB888 */
    zlib_len = 2u /* zlib header */ + deflate_stored_len(raw_len) + 4u /* adler32 */;
    idat_data_len = zlib_len;
    return HYPE_PNG_SIGNATURE_LEN + (HYPE_PNG_CHUNK_OVERHEAD + HYPE_PNG_IHDR_DATA_LEN) +
           (HYPE_PNG_CHUNK_OVERHEAD + idat_data_len) + (HYPE_PNG_CHUNK_OVERHEAD + 0u);
}

/* Writes the zlib-wrapped, stored-DEFLATE-compressed IDAT payload directly into `w` (rather than
 * building it in a side buffer first): a zlib header, then the raw scanline bytes split into
 * <=65535-byte stored blocks each with their own 5-byte header, then the Adler-32 of the whole
 * raw stream. `row_source` supplies one row at a time (filter byte + RGB888) so this never needs
 * the whole image assembled contiguously in memory first. */
static void write_idat_payload(hype_png_w_t *w, const uint8_t *rgb, uint32_t width, uint32_t height,
                               uint32_t stride_bytes) {
    uint32_t raw_len = height * (1u + width * 3u);
    uint32_t row_bytes = width * 3u;
    uint32_t remaining = raw_len;
    uint32_t row = 0;
    uint32_t byte_in_row = 0; /* 0 = the filter byte is next; else offset into pixel bytes */
    uint32_t adler_s1 = 1u, adler_s2 = 0u;

    /* zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (no preset dict, check bits valid
     * for CMF/FLG as a big-endian 16-bit value -- 0x7801 % 31 == 0, the required property). */
    w_u8(w, 0x78u);
    w_u8(w, 0x01u);

    while (remaining > 0u) {
        uint32_t block_len = (remaining < HYPE_DEFLATE_STORED_MAX) ? remaining
                                                                    : HYPE_DEFLATE_STORED_MAX;
        uint32_t bfinal = (block_len == remaining) ? 1u : 0u;
        uint32_t emitted = 0;

        w_u8(w, (uint8_t)bfinal); /* BTYPE=00 (stored) in the low bits too; only BFINAL matters */
        w_u8(w, (uint8_t)(block_len & 0xFFu));
        w_u8(w, (uint8_t)((block_len >> 8) & 0xFFu));
        w_u8(w, (uint8_t)(~block_len & 0xFFu));
        w_u8(w, (uint8_t)((~block_len >> 8) & 0xFFu));

        while (emitted < block_len) {
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
            w_u8(w, b);
            adler_s1 = (adler_s1 + b) % 65521u;
            adler_s2 = (adler_s2 + adler_s1) % 65521u;
            emitted++;
        }
        remaining -= block_len;
    }

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

    /* IDAT's length/CRC need the payload's length up front (chunks are not streamable in this
     * writer), so it's computed the same way hype_png_encoded_size() does rather than built into
     * a side buffer first -- write_idat_payload() is called TWICE: once (via a throwaway length
     * calc already done above) to know the length, and once for real into `w`. Simpler: derive
     * the length arithmetically (already have it) and let w_chunk's length come from that. */
    {
        uint32_t raw_len = height * (1u + width * 3u);
        uint32_t idat_len = 2u + deflate_stored_len(raw_len) + 4u;
        w_u32_be(&w, idat_len);
        w_bytes(&w, (const uint8_t *)"IDAT", 4u);
        {
            uint32_t crc_start = w.len;
            write_idat_payload(&w, rgb, width, height, stride_bytes);
            if (!w.overflowed) {
                uint32_t crc = crc32_update(0, (const uint8_t *)"IDAT", 4u);
                crc = crc32_update(crc, w.buf + crc_start, w.len - crc_start);
                w_u32_be(&w, crc);
            }
        }
    }

    w_chunk(&w, "IEND", 0, 0u);

    return w.overflowed ? 0u : w.len;
}
