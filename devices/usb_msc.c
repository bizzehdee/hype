#include "usb_msc.h"

/*
 * #592: USB Mass Storage (Bulk-Only Transport + SCSI) class device. See usb_msc.h. Everything here
 * is pure logic over the hype_blk_backend_t; the xHCI controller (#591) delivers the control and
 * bulk transfers and moves the bytes to/from guest memory.
 */

/* ---- little-endian byte helpers (freestanding: no libc) ---------------------------------- */
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static uint32_t get_be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | p[1];
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void bcopy_bytes(uint8_t *d, const uint8_t *s, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}
static void bzero_bytes(uint8_t *d, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i] = 0;
    }
}

/* ---- USB descriptors --------------------------------------------------------------------- */
#define MSC_VENDOR_ID 0x1D6Bu  /* Linux Foundation, a benign well-known vendor */
#define MSC_PRODUCT_ID 0x0104u /* "hype removable disk" */

static const uint8_t k_device_desc[18] = {
    18, 0x01,             /* bLength, DEVICE */
    0x00, 0x02,           /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,     /* class/subclass/protocol: per-interface */
    64,                   /* bMaxPacketSize0 */
    (uint8_t)MSC_VENDOR_ID, (uint8_t)(MSC_VENDOR_ID >> 8),
    (uint8_t)MSC_PRODUCT_ID, (uint8_t)(MSC_PRODUCT_ID >> 8),
    0x00, 0x01,           /* bcdDevice 1.00 */
    0x01, 0x02, 0x03,     /* iManufacturer, iProduct, iSerial */
    0x01                  /* bNumConfigurations */
};

/* Configuration (9) + Interface (9) + 2 x Endpoint (7) = 32 bytes. */
static const uint8_t k_config_desc[32] = {
    9, 0x02,              /* CONFIGURATION */
    32, 0x00,             /* wTotalLength */
    0x01,                 /* bNumInterfaces */
    0x01,                 /* bConfigurationValue */
    0x00,                 /* iConfiguration */
    0x80,                 /* bmAttributes: bus-powered */
    0x32,                 /* bMaxPower 100 mA */
    /* Interface 0 */
    9, 0x04, 0x00, 0x00,  /* INTERFACE, if#0, alt 0 */
    0x02,                 /* bNumEndpoints */
    0x08, 0x06, 0x50,     /* class MSC, subclass SCSI, protocol BOT */
    0x00,                 /* iInterface */
    /* EP1 OUT, bulk, 512 */
    7, 0x05, 0x01, 0x02, 0x00, 0x02, 0x00,
    /* EP1 IN, bulk, 512 */
    7, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00
};

/* String 0: supported languages (English US). */
static const uint8_t k_string0[4] = {4, 0x03, 0x09, 0x04};

/* Build a UNICODE string descriptor from ASCII into out; returns its byte length. */
static uint32_t build_string(uint8_t *out, uint32_t out_max, const char *ascii) {
    uint32_t n = 0;
    uint32_t len;
    while (ascii[n] != '\0') {
        n++;
    }
    len = 2u + n * 2u;
    if (len > out_max) {
        len = out_max & ~1u;
        n = (len >= 2u) ? (len - 2u) / 2u : 0u;
    }
    out[0] = (uint8_t)len;
    out[1] = 0x03;
    {
        uint32_t i;
        for (i = 0; i < n; i++) {
            out[2 + i * 2] = (uint8_t)ascii[i];
            out[3 + i * 2] = 0;
        }
    }
    return len;
}

/* ---- control transfers ------------------------------------------------------------------- */
static int msc_control(void *ctx, const uint8_t setup[8], uint8_t *data, uint32_t data_max,
                       uint32_t *data_len) {
    uint8_t bmRequestType = setup[0];
    uint8_t bRequest = setup[1];
    uint16_t wValue = (uint16_t)(setup[2] | ((uint16_t)setup[3] << 8));
    uint16_t wLength = (uint16_t)(setup[6] | ((uint16_t)setup[7] << 8));
    uint32_t out_len = 0;
    if (data_len) {
        *data_len = 0;
    }
    /* Never write past the buffer the controller actually provided. The descriptor copies below
     * clamp to wLength; fold data_max into that ceiling so a short data-stage TRB cannot overflow. */
    if ((uint32_t)wLength > data_max) {
        wLength = (uint16_t)data_max;
    }

    if ((bmRequestType & 0x60u) == 0x00u) {
        /* Standard request. */
        switch (bRequest) {
        case 0x06: { /* GET_DESCRIPTOR */
            uint8_t dtype = (uint8_t)(wValue >> 8);
            uint8_t dindex = (uint8_t)(wValue & 0xFF);
            if (dtype == 0x01) { /* DEVICE */
                out_len = sizeof(k_device_desc);
                if (out_len > wLength) out_len = wLength;
                if (data && out_len) bcopy_bytes(data, k_device_desc, out_len);
            } else if (dtype == 0x02) { /* CONFIGURATION */
                out_len = sizeof(k_config_desc);
                if (out_len > wLength) out_len = wLength;
                if (data && out_len) bcopy_bytes(data, k_config_desc, out_len);
            } else if (dtype == 0x03) { /* STRING */
                uint8_t tmp[64];
                uint32_t slen;
                if (dindex == 0) {
                    slen = sizeof(k_string0);
                    bcopy_bytes(tmp, k_string0, slen);
                } else if (dindex == 1) {
                    slen = build_string(tmp, sizeof(tmp), "hype");
                } else if (dindex == 2) {
                    slen = build_string(tmp, sizeof(tmp), "hype removable disk");
                } else {
                    slen = build_string(tmp, sizeof(tmp), "HYPEUSB0001");
                }
                out_len = slen;
                if (out_len > wLength) out_len = wLength;
                if (data && out_len) bcopy_bytes(data, tmp, out_len);
            } else {
                return -1; /* unsupported descriptor -> STALL */
            }
            break;
        }
        case 0x08: /* GET_CONFIGURATION */
            out_len = (wLength >= 1u) ? 1u : 0u;
            if (data && out_len) data[0] = 1u;
            break;
        case 0x00: /* GET_STATUS */
            out_len = (wLength >= 2u) ? 2u : 0u;
            if (data && out_len >= 2u) {
                data[0] = 0;
                data[1] = 0;
            }
            break;
        case 0x05: /* SET_ADDRESS -- the controller applies it; ACK */
        case 0x09: /* SET_CONFIGURATION */
        case 0x0B: /* SET_INTERFACE */
        case 0x01: /* CLEAR_FEATURE */
        case 0x03: /* SET_FEATURE */
            out_len = 0;
            break;
        default:
            return -1;
        }
    } else if ((bmRequestType & 0x60u) == 0x20u) {
        /* Class request (mass storage). */
        hype_usb_msc_t *msc = (hype_usb_msc_t *)ctx;
        switch (bRequest) {
        case 0xFE: /* Get Max LUN */
            out_len = (wLength >= 1u) ? 1u : 0u;
            if (data && out_len) data[0] = 0u; /* one LUN */
            break;
        case 0xFF: /* Bulk-Only Mass Storage Reset */
            if (msc) {
                msc->phase = HYPE_MSC_PHASE_CBW;
            }
            out_len = 0;
            break;
        default:
            return -1;
        }
    } else {
        return -1;
    }
    if (data_len) {
        *data_len = out_len;
    }
    return 0;
}

/* ---- SCSI over BOT ----------------------------------------------------------------------- */
static void set_sense(hype_usb_msc_t *msc, uint8_t key, uint8_t asc, uint8_t ascq) {
    msc->sense_key = key;
    msc->sense_asc = asc;
    msc->sense_ascq = ascq;
}

/* Prepare a small data-in response of `n` bytes held in msc->resp. */
static void resp_ready(hype_usb_msc_t *msc, uint32_t n) {
    msc->from_disk = 0;
    msc->resp_len = n;
    msc->resp_off = 0;
    msc->phase = (n > 0u) ? HYPE_MSC_PHASE_DATA_IN : HYPE_MSC_PHASE_CSW;
}

static uint64_t backend_sectors(const hype_usb_msc_t *msc) {
    return (msc->be != 0) ? msc->be->total_sectors : 0u;
}

/* Dispatch one SCSI CDB. Sets phase and csw_status. */
static void scsi_dispatch(hype_usb_msc_t *msc, const uint8_t *cdb, uint32_t cdb_len) {
    uint8_t op = (cdb_len > 0u) ? cdb[0] : 0xFFu;
    msc->csw_status = 0; /* pass unless a branch fails it */

    switch (op) {
    case 0x00: /* TEST UNIT READY */
        if (msc->be == 0) {
            set_sense(msc, 0x02, 0x3A, 0x00); /* NOT READY, medium not present */
            msc->csw_status = 1;
        }
        resp_ready(msc, 0);
        break;
    case 0x03: { /* REQUEST SENSE */
        uint32_t n = 18u;
        bzero_bytes(msc->resp, n);
        msc->resp[0] = 0x70;          /* current error, fixed format */
        msc->resp[2] = msc->sense_key; /* sense key */
        msc->resp[7] = 10u;            /* additional sense length */
        msc->resp[12] = msc->sense_asc;
        msc->resp[13] = msc->sense_ascq;
        if (cdb_len >= 5u && cdb[4] != 0u && cdb[4] < n) {
            n = cdb[4];
        }
        resp_ready(msc, n);
        break;
    }
    case 0x12: { /* INQUIRY */
        uint32_t n = 36u;
        bzero_bytes(msc->resp, n);
        msc->resp[0] = 0x00; /* direct-access block device */
        msc->resp[1] = 0x80; /* RMB: removable medium -- this is the point of #446 */
        msc->resp[2] = 0x05; /* SPC-3 */
        msc->resp[3] = 0x02; /* response data format */
        msc->resp[4] = 31u;  /* additional length */
        bcopy_bytes(&msc->resp[8], (const uint8_t *)"hype    ", 8);
        bcopy_bytes(&msc->resp[16], (const uint8_t *)"removable disk  ", 16);
        bcopy_bytes(&msc->resp[32], (const uint8_t *)"0001", 4);
        if (cdb_len >= 5u && cdb[4] != 0u && cdb[4] < n) {
            n = cdb[4];
        }
        resp_ready(msc, n);
        break;
    }
    case 0x1A: { /* MODE SENSE(6) */
        uint32_t n = 4u;
        bzero_bytes(msc->resp, n);
        msc->resp[0] = 3u; /* mode data length (following bytes) */
        msc->resp[1] = 0;  /* medium type */
        msc->resp[2] = 0;  /* device-specific: WP=0 */
        msc->resp[3] = 0;  /* block descriptor length */
        resp_ready(msc, n);
        break;
    }
    case 0x1E: /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x1B: /* START STOP UNIT */
    case 0x35: /* SYNCHRONIZE CACHE(10) */
        resp_ready(msc, 0);
        break;
    case 0x25: { /* READ CAPACITY(10) */
        uint64_t last = backend_sectors(msc);
        uint32_t last32;
        bzero_bytes(msc->resp, 8u);
        last = (last > 0u) ? (last - 1u) : 0u;
        last32 = (last > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)last;
        put_be32(&msc->resp[0], last32);          /* returned LBA */
        put_be32(&msc->resp[4], HYPE_MSC_SECTOR); /* block length */
        resp_ready(msc, 8u);
        break;
    }
    case 0x9E: { /* SERVICE ACTION IN(16): READ CAPACITY(16) = service action 0x10 */
        uint64_t last = backend_sectors(msc);
        bzero_bytes(msc->resp, 32u);
        last = (last > 0u) ? (last - 1u) : 0u;
        put_be32(&msc->resp[0], (uint32_t)(last >> 32));
        put_be32(&msc->resp[4], (uint32_t)last);
        put_be32(&msc->resp[8], HYPE_MSC_SECTOR);
        resp_ready(msc, 32u);
        break;
    }
    case 0x28:   /* READ(10) */
    case 0x2A: { /* WRITE(10) */
        uint64_t lba = get_be32(&cdb[2]);
        uint32_t blocks = get_be16(&cdb[7]);
        if (msc->be == 0) {
            set_sense(msc, 0x02, 0x3A, 0x00);
            msc->csw_status = 1;
            resp_ready(msc, 0);
            break;
        }
        if (!hype_blk_range_in_bounds(backend_sectors(msc), lba, blocks)) {
            set_sense(msc, 0x05, 0x21, 0x00); /* ILLEGAL REQUEST, LBA out of range */
            msc->csw_status = 1;
            resp_ready(msc, 0);
            break;
        }
        if (blocks == 0u) {
            resp_ready(msc, 0);
            break;
        }
        msc->from_disk = 1;
        msc->disk_lba = lba;
        msc->disk_remaining = blocks * HYPE_MSC_SECTOR;
        msc->phase = (op == 0x28) ? HYPE_MSC_PHASE_DATA_IN : HYPE_MSC_PHASE_DATA_OUT;
        break;
    }
    default:
        set_sense(msc, 0x05, 0x20, 0x00); /* ILLEGAL REQUEST, invalid command */
        msc->csw_status = 1;
        resp_ready(msc, 0);
        break;
    }
}

/* ---- bulk transfers ---------------------------------------------------------------------- */
static int msc_bulk_out(void *ctx, const uint8_t *data, uint32_t len) {
    hype_usb_msc_t *msc = (hype_usb_msc_t *)ctx;
    if (msc == 0) {
        return -1;
    }
    if (msc->phase == HYPE_MSC_PHASE_CBW) {
        /* Parse the Command Block Wrapper. */
        if (len < 31u || get_be32(data) != 0x55534243u /* 'USBC' little-endian dword */) {
            /* signature check: CBW signature is 0x43425355; as a LE dword the bytes are
             * 55 53 42 43, so read as big-endian that's 0x55534243. */
            return -1;
        }
        msc->tag = (uint32_t)data[4] | ((uint32_t)data[5] << 8) | ((uint32_t)data[6] << 16) |
                   ((uint32_t)data[7] << 24);
        msc->data_len = (uint32_t)data[8] | ((uint32_t)data[9] << 8) | ((uint32_t)data[10] << 16) |
                        ((uint32_t)data[11] << 24);
        msc->cbw_dir_in = (data[12] & 0x80u) ? 1u : 0u;
        msc->residue = msc->data_len;
        {
            uint8_t cb_len = data[14];
            if (cb_len > 16u) {
                cb_len = 16u;
            }
            msc->cbws++;
            scsi_dispatch(msc, &data[15], cb_len);
        }
        /* A no-data command jumps straight to the status phase. */
        if (msc->data_len == 0u) {
            msc->phase = HYPE_MSC_PHASE_CSW;
        }
        return 0;
    }
    if (msc->phase == HYPE_MSC_PHASE_DATA_OUT) {
        /* WRITE data phase: stream to the backend, sector by sector via the bounce buffer to
         * tolerate any alignment. */
        uint32_t off = 0;
        while (off < len && msc->disk_remaining > 0u) {
            uint32_t chunk = HYPE_MSC_SECTOR;
            if (chunk > len - off) chunk = len - off;
            if (chunk > msc->disk_remaining) chunk = msc->disk_remaining;
            if (chunk == HYPE_MSC_SECTOR) {
                if (hype_blk_backend_write(msc->be, msc->disk_lba, 1u, &data[off]) != 0) {
                    msc->csw_status = 1;
                    set_sense(msc, 0x04, 0x00, 0x00);
                    break;
                }
            } else {
                /* partial trailing sector: read-modify-write through the bounce */
                bzero_bytes(msc->bounce, HYPE_MSC_SECTOR);
                (void)hype_blk_backend_read(msc->be, msc->disk_lba, 1u, msc->bounce);
                bcopy_bytes(msc->bounce, &data[off], chunk);
                if (hype_blk_backend_write(msc->be, msc->disk_lba, 1u, msc->bounce) != 0) {
                    msc->csw_status = 1;
                    set_sense(msc, 0x04, 0x00, 0x00);
                    break;
                }
            }
            msc->disk_lba++;
            off += chunk;
            msc->disk_remaining -= chunk;
            msc->residue -= (msc->residue >= chunk) ? chunk : msc->residue;
        }
        if (msc->disk_remaining == 0u) {
            msc->phase = HYPE_MSC_PHASE_CSW;
        }
        return 0;
    }
    return -1; /* unexpected bulk OUT */
}

/* Build the 13-byte CSW into out. */
static void build_csw(hype_usb_msc_t *msc, uint8_t *out) {
    put_le32(&out[0], 0x53425355u); /* 'USBS' */
    put_le32(&out[4], msc->tag);
    put_le32(&out[8], msc->residue);
    out[12] = msc->csw_status;
    msc->csws++;
}

static int msc_bulk_in(void *ctx, uint8_t *data, uint32_t max, uint32_t *len) {
    hype_usb_msc_t *msc = (hype_usb_msc_t *)ctx;
    uint32_t produced = 0;
    if (msc == 0) {
        return -1;
    }
    if (len) {
        *len = 0;
    }
    if (msc->phase == HYPE_MSC_PHASE_CSW) {
        uint8_t csw[13];
        if (max < 13u) {
            return -1;
        }
        build_csw(msc, csw);
        bcopy_bytes(data, csw, 13u);
        produced = 13u;
        msc->phase = HYPE_MSC_PHASE_CBW; /* ready for the next command */
        if (len) {
            *len = produced;
        }
        return 0;
    }
    if (msc->phase == HYPE_MSC_PHASE_DATA_IN) {
        if (msc->from_disk) {
            while (produced < max && msc->disk_remaining > 0u) {
                uint32_t chunk = HYPE_MSC_SECTOR;
                if (chunk > max - produced) chunk = max - produced;
                if (chunk > msc->disk_remaining) chunk = msc->disk_remaining;
                if (chunk == HYPE_MSC_SECTOR) {
                    if (hype_blk_backend_read(msc->be, msc->disk_lba, 1u, &data[produced]) != 0) {
                        msc->csw_status = 1;
                        set_sense(msc, 0x04, 0x00, 0x00);
                        break;
                    }
                } else {
                    bzero_bytes(msc->bounce, HYPE_MSC_SECTOR);
                    if (hype_blk_backend_read(msc->be, msc->disk_lba, 1u, msc->bounce) != 0) {
                        msc->csw_status = 1;
                        set_sense(msc, 0x04, 0x00, 0x00);
                        break;
                    }
                    bcopy_bytes(&data[produced], msc->bounce, chunk);
                }
                msc->disk_lba++;
                produced += chunk;
                msc->disk_remaining -= chunk;
                msc->residue -= (msc->residue >= chunk) ? chunk : msc->residue;
            }
            if (msc->disk_remaining == 0u) {
                msc->phase = HYPE_MSC_PHASE_CSW;
            }
        } else {
            uint32_t avail = msc->resp_len - msc->resp_off;
            produced = (avail < max) ? avail : max;
            bcopy_bytes(data, &msc->resp[msc->resp_off], produced);
            msc->resp_off += produced;
            msc->residue -= (msc->residue >= produced) ? produced : msc->residue;
            if (msc->resp_off >= msc->resp_len) {
                msc->phase = HYPE_MSC_PHASE_CSW;
            }
        }
        if (len) {
            *len = produced;
        }
        return 0;
    }
    return -1;
}

static const hype_usb_device_ops_t k_msc_ops = {
    .control = msc_control,
    .bulk_out = msc_bulk_out,
    .bulk_in = msc_bulk_in,
};

hype_usb_device_t *hype_usb_msc_init(hype_usb_msc_t *msc, const hype_blk_backend_t *be) {
    uint32_t i;
    if (msc == 0) {
        return 0;
    }
    for (i = 0; i < sizeof(*msc); i++) {
        ((uint8_t *)msc)[i] = 0;
    }
    msc->be = be;
    msc->phase = HYPE_MSC_PHASE_CBW;
    msc->usb.ops = &k_msc_ops;
    msc->usb.ctx = msc;
    return &msc->usb;
}
