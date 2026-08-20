#include <stdio.h>
#include "../host_pci.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

/* --- Fake config spaces (one read32 fn per topology) --- */

/* Single-function AHCI controller at 00:1f.0 (class 01/06/01, 32-bit ABAR). */
static uint32_t cfg_ahci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 31 || func != 0) {
        return 0xFFFFFFFFu; /* every other slot: no device */
    }
    switch (off) {
        case 0x00: return 0x29228086u;           /* vendor 8086, device 2922 */
        case 0x08: return 0x01060102u;           /* class 01, subclass 06 (AHCI), prog-if 01 */
        case 0x0C: return 0x00000000u;           /* header type 0, single function */
        case 0x24: return 0xFEBF1000u;           /* BAR5 (ABAR): 32-bit mem, base 0xFEBF1000 */
        default:   return 0x00000000u;
    }
}

/* NVMe controller at 00:01.0 (class 01/08/02) with a 64-bit BAR0. */
static uint32_t cfg_nvme(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 1 || func != 0) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x00111d1eu;           /* some vendor/device */
        case 0x08: return 0x01080200u;           /* class 01, subclass 08 (NVMe), prog-if 02 */
        case 0x0C: return 0x00000000u;
        case 0x10: return 0xF0000004u;           /* BAR0 lo: 64-bit mem (bits[2:1]=10), base low 0xF0000000 */
        case 0x14: return 0x00000001u;           /* BAR0 hi: base high */
        default:   return 0x00000000u;
    }
}

/* Two storage controllers: NVMe at 00:01.0 + AHCI at 00:1f.2 (real HW has
 * several; the plain find_storage returns only the first). */
static uint32_t cfg_two_storage(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus == 0 && dev == 1 && func == 0) {
        switch (off) {
            case 0x00: return 0x00111d1eu;
            case 0x08: return 0x01080200u;       /* NVMe */
            case 0x10: return 0xF0000004u; case 0x14: return 0x00000001u; /* 64-bit BAR0 */
            default:   return 0u;
        }
    }
    if (bus == 0 && dev == 0x1f && func == 0) { /* LPC bridge, multi-function bit set */
        switch (off) {
            case 0x00: return 0x9c438086u;       /* vendor 8086 */
            case 0x08: return 0x06010000u;       /* class 06 (bridge) -- not storage */
            case 0x0C: return 0x00800000u;       /* header-type byte (bits 23:16) MF bit set */
            default:   return 0u;
        }
    }
    if (bus == 0 && dev == 0x1f && func == 2) {
        switch (off) {
            case 0x00: return 0x8c028086u;       /* vendor 8086 */
            case 0x08: return 0x01060102u;       /* AHCI */
            case 0x24: return 0xFEBF1000u;       /* ABAR (BAR5) */
            default:   return 0u;
        }
    }
    return 0xFFFFFFFFu;
}

/* xHCI USB controller at 00:14.0 (class 0C/03/30) with a 64-bit BAR0. */
static uint32_t cfg_xhci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 0x14 || func != 0) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x1e318086u;           /* vendor/device */
        case 0x08: return 0x0c033000u;           /* class 0C, subclass 03 (USB), prog-if 30 (xHCI) */
        case 0x0C: return 0x00000000u;
        case 0x10: return 0xE0000004u;           /* BAR0 lo: 64-bit mem, base low 0xE0000000 */
        case 0x14: return 0x00000003u;           /* BAR0 hi */
        default:   return 0x00000000u;
    }
}

/* Two xHCI controllers on different buses (chipset + add-in), to exercise the
 * resumable find_xhci_from enumeration. Both are single-function. */
static uint32_t cfg_two_xhci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus == 0 && dev == 0x14 && func == 0) {
        switch (off) {
            case 0x00: return 0x1e318086u;
            case 0x08: return 0x0c033000u;
            case 0x10: return 0xE0000004u; case 0x14: return 0x00000003u;
            default:   return 0x00000000u;
        }
    }
    if (bus == 3 && dev == 0 && func == 0) {
        switch (off) {
            case 0x00: return 0x43ee1022u;           /* AMD xHCI */
            case 0x08: return 0x0c033000u;
            case 0x10: return 0xFC8A0004u; case 0x14: return 0x00000000u;
            default:   return 0x00000000u;
        }
    }
    return 0xFFFFFFFFu;
}

/* A USB controller that is NOT xHCI (EHCI, prog-if 0x20) -- must be skipped. */
static uint32_t cfg_ehci(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 0x1d || func != 0) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x1e2d8086u;
        case 0x08: return 0x0c032000u;           /* class 0C/03/20 = EHCI, not xHCI */
        default:   return 0x00000000u;
    }
}

/* No storage anywhere: a lone display controller at 00:02.0 (class 03). */
static uint32_t cfg_no_storage(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 2 || func != 0) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x12345678u;
        case 0x08: return 0x03000000u;           /* class 03 (display), not storage */
        case 0x0C: return 0x00000000u;
        default:   return 0x00000000u;
    }
}

/* Multi-function device at 00:1f: func0 is an LPC bridge (class 06) with the MF
 * bit set; func2 is the AHCI controller. Exercises the MF-probe path. */
static uint32_t cfg_multifunction(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 31) {
        return 0xFFFFFFFFu;
    }
    if (func == 0) {
        switch (off) {
            case 0x00: return 0x1c008086u;
            case 0x08: return 0x06010000u;       /* class 06 (bridge) -- not storage */
            case 0x0C: return 0x00800000u;       /* header-type byte (bits[23:16]) = 0x80: MF bit set */
            default:   return 0x00000000u;
        }
    }
    if (func == 2) {
        switch (off) {
            case 0x00: return 0x29228086u;
            case 0x08: return 0x01060102u;       /* AHCI */
            case 0x0C: return 0x00000000u;
            case 0x24: return 0xFEBF1000u;       /* ABAR */
            default:   return 0x00000000u;
        }
    }
    return 0xFFFFFFFFu; /* func1, func3..7 absent */
}

static void test_find_ahci(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_ahci, 0, &st);
    CHECK_HEX("AHCI found", 1, found);
    CHECK_HEX("kind = AHCI", HYPE_HOST_STORAGE_AHCI, st.kind);
    CHECK_HEX("bus", 0, st.bus);
    CHECK_HEX("dev", 31, st.dev);
    CHECK_HEX("func", 0, st.func);
    CHECK_HEX("vendor", 0x8086, st.vendor_id);
    CHECK_HEX("ABAR base (low bits masked)", 0xFEBF1000ull, st.bar_phys);
}

static void test_find_nvme_64bit_bar(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_nvme, 0, &st);
    CHECK_HEX("NVMe found", 1, found);
    CHECK_HEX("kind = NVMe", HYPE_HOST_STORAGE_NVME, st.kind);
    CHECK_HEX("64-bit BAR0 base assembled from both dwords", 0x1F0000000ull, st.bar_phys);
}

static void test_no_storage(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_no_storage, 0, &st);
    CHECK_HEX("nothing found", 0, found);
    CHECK_HEX("kind = NONE", HYPE_HOST_STORAGE_NONE, st.kind);
}

static void test_multifunction_storage(void) {
    hype_host_storage_t st;
    int found = hype_host_pci_find_storage(cfg_multifunction, 0, &st);
    CHECK_HEX("AHCI behind a multifunction device is found", 1, found);
    CHECK_HEX("kind = AHCI", HYPE_HOST_STORAGE_AHCI, st.kind);
    CHECK_HEX("func = 2", 2, st.func);
    CHECK_HEX("ABAR base", 0xFEBF1000ull, st.bar_phys);
}

/* --- Mutable fake config space for the interrupt-disable path --- */

/* One device at 00:1f.0. Dword-indexed store (offset/4). The layout carries a
 * capability list: cap ptr @0x34 -> 0x50 (MSI, id 0x05) -> 0x70 (MSI-X, id
 * 0x11) -> 0 (end). Command bit10 clear, MSI/MSI-X enables set, Status[4] set. */
#define STATUS_CAPS 0x0010u /* Status[4]: capabilities list present */

static uint32_t g_cfg[64];

static void cfg_reset(void) {
    unsigned i;
    for (i = 0; i < 64; i++) {
        g_cfg[i] = 0u;
    }
    g_cfg[0x00 / 4] = 0x29228086u;                 /* vendor/device */
    g_cfg[0x04 / 4] = (STATUS_CAPS << 16) | 0x0006u; /* status[4]=caps, command bits 1-2 set */
    g_cfg[0x08 / 4] = 0x01060102u;                 /* AHCI class */
    g_cfg[0x34 / 4] = 0x00000050u;                 /* capabilities pointer -> 0x50 */
    /* MSI capability @0x50: id=0x05, next=0x70, message-control (high16)=0x0001 (enabled). */
    g_cfg[0x50 / 4] = (0x0001u << 16) | (0x70u << 8) | 0x05u;
    /* MSI-X capability @0x70: id=0x11, next=0x90, message-control=0x8003 (enabled + table size). */
    g_cfg[0x70 / 4] = (0x8003u << 16) | (0x90u << 8) | 0x11u;
    /* PM capability @0x90: id=0x01, next=0x00, PM caps (high16) irrelevant here. */
    g_cfg[0x90 / 4] = (0x0000u << 16) | (0x00u << 8) | 0x01u;
    /* PMCSR @0x94: PowerState (bits [1:0]) = 0 (D0) by default. */
    g_cfg[0x94 / 4] = 0u;
}

static uint32_t cfg_rw_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 31 || func != 0) {
        return 0xFFFFFFFFu;
    }
    return g_cfg[(off & 0xFCu) / 4];
}

static void cfg_rw_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t val) {
    if (bus != 0 || dev != 31 || func != 0) {
        return;
    }
    g_cfg[(off & 0xFCu) / 4] = val;
}

static void test_find_cap(void) {
    cfg_reset();
    CHECK_HEX("find MSI cap @0x50", 0x50u,
              hype_host_pci_find_cap(cfg_rw_read, 0, 31, 0, HYPE_HOST_PCI_CAP_MSI));
    CHECK_HEX("find MSI-X cap @0x70", 0x70u,
              hype_host_pci_find_cap(cfg_rw_read, 0, 31, 0, HYPE_HOST_PCI_CAP_MSIX));
    CHECK_HEX("absent cap -> 0", 0u,
              hype_host_pci_find_cap(cfg_rw_read, 0, 31, 0, 0x10u));
    /* No capability list (Status[4] clear) -> 0 even though a ptr is present. */
    cfg_reset();
    g_cfg[0x04 / 4] &= ~(STATUS_CAPS << 16);
    CHECK_HEX("no cap list -> 0", 0u,
              hype_host_pci_find_cap(cfg_rw_read, 0, 31, 0, HYPE_HOST_PCI_CAP_MSI));
    /* A self-referential next-pointer must not loop forever (bounded walk). */
    cfg_reset();
    g_cfg[0x50 / 4] = (0x0001u << 16) | (0x50u << 8) | 0x05u; /* id 5, next -> itself */
    CHECK_HEX("looping list, absent id terminates", 0u,
              hype_host_pci_find_cap(cfg_rw_read, 0, 31, 0, 0x11u));
}

static void test_disable_interrupts(void) {
    cfg_reset();
    hype_host_pci_disable_interrupts(cfg_rw_read, cfg_rw_write, 0, 31, 0);
    /* Command[10] set; status half written as 0 (RW1C-safe); other command bits kept. */
    CHECK_HEX("INTx disable bit set", 0x0400u, g_cfg[0x04 / 4] & 0x0400u);
    CHECK_HEX("command low bits preserved", 0x0006u, g_cfg[0x04 / 4] & 0x0006u);
    CHECK_HEX("status half zeroed on write", 0u, g_cfg[0x04 / 4] >> 16);
    /* MSI enable cleared, id/next preserved. */
    CHECK_HEX("MSI enable cleared", 0u, (g_cfg[0x50 / 4] >> 16) & 0x1u);
    CHECK_HEX("MSI id/next preserved", 0x7005u, g_cfg[0x50 / 4] & 0xFFFFu);
    /* MSI-X enable (bit15) cleared, table-size bits preserved. */
    CHECK_HEX("MSI-X enable cleared", 0u, (g_cfg[0x70 / 4] >> 16) & 0x8000u);
    CHECK_HEX("MSI-X table-size bits preserved", 0x0003u, (g_cfg[0x70 / 4] >> 16) & 0x7FFFu);

    /* A device with no MSI/MSI-X: only the INTx bit is touched, no crash. */
    cfg_reset();
    g_cfg[0x04 / 4] &= ~(STATUS_CAPS << 16); /* drop the capability list */
    hype_host_pci_disable_interrupts(cfg_rw_read, cfg_rw_write, 0, 31, 0);
    CHECK_HEX("INTx set even without caps", 0x0400u, g_cfg[0x04 / 4] & 0x0400u);
}

static void test_wake_enable_mmio(void) {
    uint16_t cmd;
    /* Device left in D3 with Memory-Space + Bus-Master disabled (the dead-BAR
     * state seen on the AMD FCH SATA on some boots). */
    cfg_reset();
    g_cfg[0x94 / 4] = 0x3u;             /* PMCSR PowerState = D3hot */
    g_cfg[0x04 / 4] &= ~0x0006u;        /* clear Command MEM + BME */
    cmd = hype_host_pci_wake_enable_mmio(cfg_rw_read, cfg_rw_write, 0, 31, 0);
    CHECK_HEX("PowerState forced to D0", 0u, g_cfg[0x94 / 4] & 0x3u);
    CHECK_HEX("Memory-Space enable set", 0x0002u, g_cfg[0x04 / 4] & 0x0002u);
    CHECK_HEX("Bus-Master enable set", 0x0004u, g_cfg[0x04 / 4] & 0x0004u);
    CHECK_HEX("status half zeroed on write", 0u, g_cfg[0x04 / 4] >> 16);
    CHECK_HEX("returned command has MEM|BME", 0x0006u, cmd & 0x0006u);

    /* No PM capability: MEM|BME still enabled, no crash. */
    cfg_reset();
    g_cfg[0x04 / 4] &= ~(STATUS_CAPS << 16); /* drop the capability list */
    g_cfg[0x04 / 4] &= ~0x0006u;
    (void)hype_host_pci_wake_enable_mmio(cfg_rw_read, cfg_rw_write, 0, 31, 0);
    CHECK_HEX("MEM|BME set without PM cap", 0x0006u, g_cfg[0x04 / 4] & 0x0006u);
}

/*
 * #80: QEMU's e1000 as hype sees it -- 8086:100E, class 0x02/0x00, BAR0 a 32-bit memory BAR.
 * Real values, so a byte-order or shift mistake in the finder shows up as a wrong address rather
 * than passing against a made-up fixture.
 */
static uint32_t cfg_e1000(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 3 || func != 0) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x100E8086u;  /* Intel 82540EM */
        case 0x08: return 0x02000003u;  /* class 0x02 / subclass 0x00 / prog-if 0x00 */
        case 0x10: return 0xFEB80000u;  /* BAR0: 32-bit memory BAR */
        default:   return 0x00000000u;
    }
}

/* A wireless NIC: class 0x02 SUBCLASS 0x80 (other), which must NOT match an Ethernet search. */
static uint32_t cfg_wifi(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus != 0 || dev != 0x14 || func != 3) {
        return 0xFFFFFFFFu;
    }
    switch (off) {
        case 0x00: return 0x24fb8086u;
        case 0x08: return 0x02800000u;  /* 0x02/0x80 -- a network device, not Ethernet */
        case 0x10: return 0xFEC00004u;
        default:   return 0x00000000u;
    }
}

/* Two Ethernet NICs, to exercise resumable enumeration -- a machine with chipset plus add-in. */
static uint32_t cfg_two_nics(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus == 0 && dev == 3 && func == 0) {
        switch (off) {
            case 0x00: return 0x100E8086u;
            case 0x08: return 0x02000003u;
            case 0x10: return 0xFEB80000u;
            default:   return 0x00000000u;
        }
    }
    if (bus == 2 && dev == 0 && func == 0) {
        switch (off) {
            case 0x00: return 0x816810ecu;  /* Realtek RTL8168 -- found, but not drivable */
            case 0x08: return 0x02000000u;
            case 0x10: return 0xFE900000u;
            default:   return 0x00000000u;
        }
    }
    return 0xFFFFFFFFu;
}

static void test_find_nic(void) {
    hype_host_nic_t n;

    CHECK_HEX("the e1000 is found", 1, hype_host_pci_find_nic(cfg_e1000, 0, &n));
    CHECK_HEX("vendor", 0x8086u, n.vendor_id);
    CHECK_HEX("device", 0x100Eu, n.device_id);
    CHECK_HEX("BAR0 base", 0xFEB80000u, (unsigned)n.bar_phys);
    CHECK_HEX("bus", 0, n.bus);
    CHECK_HEX("dev", 3, n.dev);

    /* A network device that is not Ethernet must not match: reporting a wireless NIC as the
     * uplink would have hype try to drive a device it has no driver for, and the failure would
     * look like a broken e1000 driver rather than the wrong device. */
    CHECK_HEX("a wireless NIC is not an Ethernet match", 0, hype_host_pci_find_nic(cfg_wifi, 0, &n));

    /* No NIC at all is a normal outcome -- networking is opt-in per VM, so this must not be an
     * error path. */
    CHECK_HEX("no NIC reports zero", 0, hype_host_pci_find_nic(cfg_no_storage, 0, &n));
}

static void test_find_all_nics(void) {
    hype_host_nic_t n;
    uint32_t bdf = 0;

    CHECK_HEX("first NIC found", 1, hype_host_pci_find_nic_from(cfg_two_nics, 3, 0u, &n, &bdf));
    CHECK_HEX("first is the Intel", 0x8086u, n.vendor_id);

    /* Resume past it: the second is a Realtek, which hype cannot drive -- but it must be
     * ENUMERABLE, so the operator is told what else is in the machine rather than left with
     * "hype picked one and said nothing about the others". */
    CHECK_HEX("second NIC found", 1,
              hype_host_pci_find_nic_from(cfg_two_nics, 3, bdf + 1u, &n, &bdf));
    CHECK_HEX("second is the Realtek", 0x10ecu, n.vendor_id);
    CHECK_HEX("with its own BAR", 0xFE900000u, (unsigned)n.bar_phys);

    CHECK_HEX("no third NIC", 0, hype_host_pci_find_nic_from(cfg_two_nics, 3, bdf + 1u, &n, &bdf));
}

static void test_find_xhci(void) {
    hype_host_xhci_t x;
    CHECK_HEX("xHCI found", 1, hype_host_pci_find_xhci(cfg_xhci, 0, &x));
    CHECK_HEX("xHCI dev", 0x14, x.dev);
    CHECK_HEX("xHCI vendor", 0x8086, x.vendor_id);
    CHECK_HEX("xHCI BAR0 (64-bit assembled, low masked)", 0x3E0000000ull, x.bar_phys);
    /* EHCI (prog-if 0x20) is not xHCI -> not matched. */
    CHECK_HEX("EHCI not matched as xHCI", 0, hype_host_pci_find_xhci(cfg_ehci, 0, &x));
    /* no USB at all */
    CHECK_HEX("no xHCI in storage-only space", 0, hype_host_pci_find_xhci(cfg_ahci, 0, &x));

    /* Enumerate ALL xHCI controllers by looping find_xhci_from (buses 0..3). */
    {
        uint32_t cur = 0, bdf = 0;
        int n = 0, saw_intel = 0, saw_amd = 0;
        while (hype_host_pci_find_xhci_from(cfg_two_xhci, 3, cur, &x, &bdf)) {
            if (x.vendor_id == 0x8086) saw_intel = 1;
            if (x.vendor_id == 0x1022) saw_amd = 1;
            n++;
            cur = bdf + 1u;
            if (n > 8) break; /* guard */
        }
        CHECK_HEX("found both xHCI controllers", 2, n);
        CHECK_HEX("saw intel xHCI", 1, saw_intel);
        CHECK_HEX("saw amd xHCI", 1, saw_amd);
    }
}

static void test_find_all_storage(void) {
    /* Enumerate ALL storage controllers by looping find_storage_from. */
    hype_host_storage_t st;
    uint32_t cur = 0;
    int n = 0, saw_nvme = 0, saw_ahci = 0;
    while (hype_host_pci_find_storage_from(cfg_two_storage, 0, cur, &st)) {
        if (st.kind == HYPE_HOST_STORAGE_NVME) saw_nvme = 1;
        if (st.kind == HYPE_HOST_STORAGE_AHCI) saw_ahci = 1;
        n++;
        cur = ((uint32_t)st.bus << 8) | ((uint32_t)st.dev << 3) | (uint32_t)st.func;
        cur += 1u;
        if (n > 8) break; /* safety */
    }
    CHECK_HEX("found both storage controllers", 2, n);
    CHECK_HEX("saw NVMe", 1, saw_nvme);
    CHECK_HEX("saw AHCI", 1, saw_ahci);
    /* plain find_storage still returns just the first (NVMe, lower bdf) */
    CHECK_HEX("find_storage returns first", HYPE_HOST_STORAGE_NVME,
              (hype_host_pci_find_storage(cfg_two_storage, 0, &st), st.kind));
}

int main(void) {
    test_find_ahci();
    test_find_all_storage();
    test_find_nvme_64bit_bar();
    test_no_storage();
    test_multifunction_storage();
    test_find_cap();
    test_disable_interrupts();
    test_wake_enable_mmio();
    test_find_nic();
    test_find_all_nics();
    test_find_xhci();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
