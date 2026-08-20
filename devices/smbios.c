#include "smbios.h"

/*
 * Strings in an SMBIOS structure follow the formatted area, each NUL
 * terminated, the set ending with a second NUL. A structure with no strings
 * still ends in two NULs.
 */
static uint32_t put_string(uint8_t *buf, uint32_t off, const char *s) {
    while (*s != '\0') {
        buf[off++] = (uint8_t)*s++;
    }
    buf[off++] = 0;
    return off;
}

static uint32_t end_strings(uint8_t *buf, uint32_t off) {
    buf[off++] = 0;
    return off;
}

/*
 * A structure with no strings still needs a terminated (empty) string set:
 * two NULs, not one. Writing a single NUL leaves the next structure's type
 * byte where a consumer expects the set terminator, which desynchronises the
 * walk for every structure after it.
 */
static uint32_t end_no_strings(uint8_t *buf, uint32_t off) {
    buf[off++] = 0;
    buf[off++] = 0;
    return off;
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64(uint8_t *p, uint64_t v) {
    unsigned i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xFFu);
    }
}

int hype_smbios_build(const hype_smbios_config_t *cfg, uint8_t *anchor, uint32_t anchor_size,
                      uint8_t *tables, uint32_t tables_size, hype_smbios_layout_t *out) {
    uint32_t off = 0;
    uint32_t i;

    if (cfg == 0 || anchor == 0 || tables == 0 || out == 0) {
        return -1;
    }
    if (anchor_size < sizeof(hype_smbios_entry_point_t) ||
        tables_size < HYPE_SMBIOS_TABLES_MIN_SIZE) {
        return -1;
    }
    if (cfg->cpu_count == 0u) {
        return -1;
    }
    /*
     * #562: threads_per_core must divide the logical CPU count, or the core count derived below is
     * a fiction. 0 means "not stated" and is read as 1; anything else that does not divide is a
     * caller bug and is refused rather than rounded, since a rounded topology is the kind of
     * almost-right table that gets believed.
     */
    if (cfg->threads_per_core > 1u && (cfg->cpu_count % cfg->threads_per_core) != 0u) {
        return -1;
    }
    for (i = 0; i < tables_size; i++) {
        tables[i] = 0;
    }

    /* Type 0 -- BIOS Information. */
    {
        uint8_t *p = tables + off;
        p[0] = HYPE_SMBIOS_TYPE_BIOS;
        p[1] = 0x18; /* formatted length, SMBIOS 2.4+ */
        put_le16(p + 2, 0x0000);
        p[4] = 1;    /* Vendor -> string 1 */
        p[5] = 2;    /* BIOS Version -> string 2 */
        put_le16(p + 6, 0xE800); /* start address segment */
        p[8] = 3;    /* Release Date -> string 3 */
        p[9] = 0;    /* ROM size: 0 => use the extended field; not claimed here */
        /* Characteristics: bit 3 "BIOS characteristics not supported" is the
         * honest answer for a virtual machine with no legacy BIOS services. */
        put_le64(p + 10, 1ull << 3);
        p[18] = 0;
        p[19] = 0;
        p[20] = 0; /* system BIOS major */
        p[21] = 0; /* system BIOS minor */
        p[22] = 0xFF; /* embedded controller: not present */
        p[23] = 0xFF;
        off += 0x18;
        off = put_string(tables, off, "hype");
        off = put_string(tables, off, "1.0");
        off = put_string(tables, off, "01/01/2026");
        off = end_strings(tables, off);
    }

    /* Type 1 -- System Information. */
    {
        uint8_t *p = tables + off;
        p[0] = HYPE_SMBIOS_TYPE_SYSTEM;
        p[1] = 0x1B;
        put_le16(p + 2, 0x0100);
        p[4] = 1; /* Manufacturer */
        p[5] = 2; /* Product Name */
        p[6] = 3; /* Version */
        p[7] = 0; /* Serial Number: none to report */
        for (i = 0; i < 16u; i++) {
            p[8 + i] = 0; /* UUID: all-zero means "not present", not a fake one */
        }
        p[24] = 6; /* Wake-up type: Power Switch */
        p[25] = 0; /* SKU */
        p[26] = 0; /* Family */
        off += 0x1B;
        off = put_string(tables, off, "hype");
        off = put_string(tables, off, "hype virtual machine");
        off = put_string(tables, off, "1.0");
        off = end_strings(tables, off);
    }

    /*
     * Type 4 -- Processor Information. ONE structure, per SOCKET (#562).
     *
     * This used to emit one per vCPU, each declaring a single single-threaded core -- so a 2-vCPU
     * guest was described as two sockets of one thread each, contradicting CPUID leaf 0xB/0x1F and
     * 0x8000001E, which report this VM's real threads_per_core. Worse, the only caller passed
     * cpu_count = 1 unconditionally, so EVERY guest was described as a single-core single-thread
     * machine whatever its `vcpus` said, and `dmidecode -t 4` disagreed with `lscpu` in the guest.
     *
     * One socket is what hype can honestly claim: it does not track which host package a VM's
     * cores came from, and inventing a socket split would be a second fiction. The core and thread
     * counts are the real ones.
     */
    {
        uint32_t tpc = cfg->threads_per_core ? cfg->threads_per_core : 1u;
        /*
         * cores cannot be 0: the guard above rejects a tpc > 1 that does not divide cpu_count, so
         * cpu_count >= tpc whenever tpc > 1, and cpu_count >= 1 always. No clamp for it -- an
         * unreachable branch is dead code that reads as a real case.
         */
        uint32_t cores = cfg->cpu_count / tpc;
        uint32_t threads = cfg->cpu_count;
        /* SMBIOS 2.x's core/thread fields are single BYTES. Saturating is the spec's own answer
         * (3.0 adds 16-bit counts at offset 0x2A, which this 0x2A-length structure does not
         * reach), and it beats letting the cast wrap: 256 threads would otherwise report as 0. */
        if (cores > 255u) {
            cores = 255u;
        }
        if (threads > 255u) {
            threads = 255u;
        }
        uint8_t *p = tables + off;
        p[0] = HYPE_SMBIOS_TYPE_PROCESSOR;
        p[1] = 0x2A;
        put_le16(p + 2, 0x0400u); /* one socket, so one fixed handle */
        p[4] = 1;    /* Socket Designation */
        p[5] = 0x03; /* Processor Type: Central Processor */
        p[6] = 0x02; /* Family: Unknown -- hype passes the host's CPUID through,
                      * so the guest reads the real family there rather than a
                      * number invented here. */
        p[7] = 2;    /* Manufacturer string */
        put_le64(p + 8, 0);   /* Processor ID: CPUID is the authority */
        p[16] = 3;   /* Version string */
        p[17] = 0;   /* Voltage: unknown */
        put_le16(p + 18, 0);  /* External clock: unknown */
        put_le16(p + 20, 0);  /* Max speed: unknown */
        put_le16(p + 22, 0);  /* Current speed: unknown */
        p[24] = 0x41;         /* Status: populated, enabled */
        p[25] = 0x06;         /* Upgrade: none */
        put_le16(p + 26, 0xFFFF); /* L1 cache handle: not provided */
        put_le16(p + 28, 0xFFFF);
        put_le16(p + 30, 0xFFFF);
        p[32] = 0; /* Serial */
        p[33] = 0; /* Asset tag */
        p[34] = 0; /* Part number */
        /*
         * #562: the real counts. Per plan.md §10 decision 47 a granted core is granted WHOLE, so
         * the guest sees cores * threads_per_core logical CPUs -- cpu_count already IS that
         * product, which is why cores divides out of it.
         */
        p[35] = (uint8_t)cores;   /* Core count */
        p[36] = (uint8_t)cores;   /* Cores enabled */
        p[37] = (uint8_t)threads; /* Thread count */
        put_le16(p + 38, 0x0004); /* Characteristics: 64-bit capable */
        put_le16(p + 40, 0x0002); /* Family 2: Unknown, per byte 6 */
        off += 0x2A;
        off = put_string(tables, off, "CPU0");
        off = put_string(tables, off, "hype");
        off = put_string(tables, off, "hype virtual processor");
        off = end_strings(tables, off);
    }

    /* Type 16 -- Physical Memory Array. */
    {
        uint8_t *p = tables + off;
        uint64_t kb = cfg->ram_bytes / 1024ull;
        p[0] = HYPE_SMBIOS_TYPE_MEMORY_ARRAY;
        p[1] = 0x17;
        put_le16(p + 2, 0x1000);
        p[4] = 0x03; /* Location: system board */
        p[5] = 0x03; /* Use: system memory */
        p[6] = 0x03; /* Error correction: none */
        /* Capacity in KB; 0x80000000 is the "see the extended field" escape,
         * which is only needed above 2 TB. */
        put_le32(p + 7, (uint32_t)(kb > 0x7FFFFFFFull ? 0x80000000ull : kb));
        put_le16(p + 11, 0xFFFE); /* no error-information handle */
        put_le16(p + 13, 1);      /* one memory device */
        put_le64(p + 15, kb > 0x7FFFFFFFull ? cfg->ram_bytes : 0);
        off += 0x17;
        off = end_no_strings(tables, off);
    }

    /* Type 17 -- Memory Device: the RAM the guest was actually given. */
    {
        uint8_t *p = tables + off;
        uint64_t mb = cfg->ram_bytes / (1024ull * 1024ull);
        p[0] = HYPE_SMBIOS_TYPE_MEMORY_DEVICE;
        p[1] = 0x28;
        put_le16(p + 2, 0x1100);
        put_le16(p + 4, 0x1000);  /* array handle */
        put_le16(p + 6, 0xFFFE);  /* no error information */
        put_le16(p + 8, 0xFFFF);  /* total width: unknown */
        put_le16(p + 10, 0xFFFF); /* data width: unknown */
        /* Size in MB; 0x7FFF is the escape to the extended field above 32 GB. */
        put_le16(p + 12, (uint16_t)(mb >= 0x7FFFull ? 0x7FFF : mb));
        p[14] = 0x09; /* Form factor: DIMM */
        p[15] = 0;    /* Device set: not part of a set */
        p[16] = 1;    /* Device Locator string */
        p[17] = 2;    /* Bank Locator string */
        p[18] = 0x02; /* Memory type: Unknown -- no physical part is being claimed */
        put_le16(p + 19, 0x0002); /* Type detail: Other */
        put_le16(p + 21, 0);      /* Speed: unknown */
        p[23] = 0; /* Manufacturer */
        p[24] = 0; /* Serial */
        p[25] = 0; /* Asset tag */
        p[26] = 0; /* Part number */
        p[27] = 0; /* Attributes */
        put_le32(p + 28, mb >= 0x7FFFull ? (uint32_t)cfg->ram_bytes : 0);
        put_le16(p + 32, 0); /* configured speed */
        put_le16(p + 34, 0); /* min voltage */
        put_le16(p + 36, 0); /* max voltage */
        put_le16(p + 38, 0); /* configured voltage */
        off += 0x28;
        off = put_string(tables, off, "DIMM0");
        off = put_string(tables, off, "System Board");
        off = end_strings(tables, off);
    }

    /* Type 127 -- End of table. */
    {
        uint8_t *p = tables + off;
        p[0] = HYPE_SMBIOS_TYPE_END;
        p[1] = 0x04;
        put_le16(p + 2, 0x7F00);
        off += 4;
        off = end_no_strings(tables, off);
    }

    if (off > tables_size) {
        return -1;
    }
    out->tables_length = off;

    /* The 3.0 entry point. Firmware relocates the structures and patches
     * table_address, so it is left zero here -- the same convention the ACPI
     * table-loader uses for every pointer it fixes up. */
    {
        hype_smbios_entry_point_t *ep = (hype_smbios_entry_point_t *)anchor;
        uint32_t sum = 0;
        uint8_t *raw = anchor;

        ep->anchor[0] = '_';
        ep->anchor[1] = 'S';
        ep->anchor[2] = 'M';
        ep->anchor[3] = '3';
        ep->anchor[4] = '_';
        ep->checksum = 0;
        ep->length = (uint8_t)sizeof(hype_smbios_entry_point_t);
        ep->major_version = 3;
        ep->minor_version = 0;
        ep->docrev = 0;
        ep->revision = 1;
        ep->reserved = 0;
        ep->table_max_size = off;
        ep->table_address = 0;

        for (i = 0; i < sizeof(hype_smbios_entry_point_t); i++) {
            sum += raw[i];
        }
        ep->checksum = (uint8_t)(0x100u - (sum & 0xFFu));
        out->anchor_length = (uint32_t)sizeof(hype_smbios_entry_point_t);
    }
    return 0;
}
