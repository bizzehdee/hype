/*
 * NET-1 (#80): the Intel 82540EM host driver's HARDWARE half -- real MMIO, the reset sequence,
 * the descriptor rings, and polled send/receive.
 *
 * core/e1000.c holds everything decidable without a device (register layout, ring arithmetic, MAC
 * decode) and is unit-tested. This file touches real registers, so it is coverage-exempt like
 * every other *_hw.c and its correctness is established by running it against a device.
 *
 * POLLED, NOT INTERRUPT-DRIVEN. hype's dispatch loop already polls host devices (the USB HID
 * drain, the log drain), interrupt routing on the HOST side is a separate concern from guest
 * delivery, and a polled driver's failure modes are legible: nothing is waiting on a line that may
 * never assert. Interrupts can come later if a measurement asks for them.
 *
 * The rings and buffers are static rather than pool-allocated. They must be physically contiguous
 * and identity-mapped for the device to DMA into, they are needed before the pool exists on some
 * paths, and one NIC means one set -- the same reasoning core/xhci_hw.c uses for its rings.
 */
#include "e1000.h"
#include "arp.h"
#include "host_pci.h"
#include "fatal.h"

/* Descriptor rings. 16-byte descriptors, and the hardware requires the ring's BYTE length to be
 * 128-byte aligned -- hype_e1000_ring_len_bytes() refuses a count that is not. */
static uint8_t g_rx_ring[HYPE_E1000_RX_RING * HYPE_E1000_DESC_BYTES] __attribute__((aligned(4096)));
static uint8_t g_tx_ring[HYPE_E1000_TX_RING * HYPE_E1000_DESC_BYTES] __attribute__((aligned(4096)));
static uint8_t g_rx_bufs[HYPE_E1000_RX_RING][HYPE_E1000_BUF_BYTES] __attribute__((aligned(4096)));
static uint8_t g_tx_bufs[HYPE_E1000_TX_RING][HYPE_E1000_BUF_BYTES] __attribute__((aligned(4096)));

static volatile uint8_t *g_bar;
static hype_e1000_mac_t g_mac;
static unsigned int g_tx_tail;
static unsigned int g_rx_tail;
static int g_ready;

/* Counters, so a link that never comes up or a frame that never lands is a number rather than a
 * theory -- the #318 lesson: prefer a counter to a trace for "does X ever happen". */
static unsigned long long g_tx_frames;
static unsigned long long g_tx_full;
static unsigned long long g_rx_frames;
static unsigned long long g_rx_dropped;

static uint32_t reg_read(uint32_t off) { return *(volatile uint32_t *)(g_bar + off); }
static void reg_write(uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_bar + off) = v; }

/* A descriptor's fields, by byte offset -- legacy formats, manual 3.2.3 / 3.3.3. */
static void desc_set_addr(uint8_t *d, uint64_t phys) {
    *(volatile uint32_t *)(d + 0) = (uint32_t)phys;
    *(volatile uint32_t *)(d + 4) = (uint32_t)(phys >> 32);
}
static void desc_set_len(uint8_t *d, uint16_t len) { *(volatile uint16_t *)(d + 8) = len; }
static uint16_t desc_get_len(const uint8_t *d) { return *(const volatile uint16_t *)(d + 8); }
static void txd_set_cmd(uint8_t *d, uint8_t cmd) { *(volatile uint8_t *)(d + 11) = cmd; }
static uint8_t txd_status(const uint8_t *d) { return *(const volatile uint8_t *)(d + 12); }
static void txd_clear_status(uint8_t *d) { *(volatile uint8_t *)(d + 12) = 0u; }
static uint8_t rxd_status(const uint8_t *d) { return *(const volatile uint8_t *)(d + 12); }
static void rxd_clear_status(uint8_t *d) { *(volatile uint8_t *)(d + 12) = 0u; }

/*
 * Read one EEPROM word through EERD. Bounded: a NIC whose EEPROM never reports DONE must not spin
 * the host forever, and returning a failure lets the caller fall back to RAL/RAH.
 */
static int eeprom_read(uint8_t word_addr, uint16_t *out) {
    unsigned long long spins;

    reg_write(HYPE_E1000_REG_EERD,
              ((uint32_t)word_addr << HYPE_E1000_EERD_ADDR_SHIFT) | HYPE_E1000_EERD_START);
    for (spins = 0; spins < 1000000ull; spins++) {
        uint32_t v = reg_read(HYPE_E1000_REG_EERD);
        if ((v & HYPE_E1000_EERD_DONE) != 0u) {
            *out = (uint16_t)((v >> HYPE_E1000_EERD_DATA_SHIFT) & 0xFFFFu);
            return 0;
        }
    }
    return -1;
}

int hype_e1000_attach(uint64_t bar_phys) {
    unsigned int i;
    uint32_t ctrl;
    uint32_t rlen, tlen;
    unsigned long long spins;

    g_ready = 0;
    if (bar_phys == 0u) {
        return -1;
    }
    g_bar = (volatile uint8_t *)(uintptr_t)bar_phys;

    rlen = hype_e1000_ring_len_bytes(HYPE_E1000_RX_RING);
    tlen = hype_e1000_ring_len_bytes(HYPE_E1000_TX_RING);
    if (rlen == 0u || tlen == 0u) {
        /* A compile-time constant the hardware cannot express. Refused loudly rather than
         * rounded: programming a ring of a different size than the driver believes it has
         * desynchronises head and tail permanently. */
        hype_debug_print("e1000: ring sizes %u/%u are not 128-byte-aligned lengths [#80]\n",
                         HYPE_E1000_RX_RING, HYPE_E1000_TX_RING);
        return -1;
    }

    /* Reset, then wait for RST to self-clear. Bounded -- a device that never clears it is broken
     * or absent, and spinning forever here would hang the boot rather than report it. */
    reg_write(HYPE_E1000_REG_CTRL, HYPE_E1000_CTRL_RST);
    for (spins = 0; spins < 1000000ull; spins++) {
        if ((reg_read(HYPE_E1000_REG_CTRL) & HYPE_E1000_CTRL_RST) == 0u) {
            break;
        }
    }
    if ((reg_read(HYPE_E1000_REG_CTRL) & HYPE_E1000_CTRL_RST) != 0u) {
        hype_debug_print("e1000: CTRL.RST never self-cleared -- the device is not responding "
                         "[#80]\n");
        return -1;
    }

    /* Mask every interrupt source: this driver polls, and an unmasked NIC on a shared line would
     * assert an interrupt nothing is routed to handle. */
    reg_write(HYPE_E1000_REG_IMC, 0xFFFFFFFFu);

    /* MAC: prefer RAL/RAH, which the hardware populates from EEPROM at reset on parts that have
     * one. Fall back to reading the EEPROM directly, and refuse to come up without a usable
     * address -- transmitting from 00:00:00:00:00:00 produces a fault that reads as a cable or
     * switch problem rather than a driver one. */
    hype_e1000_mac_from_ral_rah(reg_read(HYPE_E1000_REG_RAL0), reg_read(HYPE_E1000_REG_RAH0),
                                &g_mac);
    if (!g_mac.valid) {
        uint16_t w0 = 0, w1 = 0, w2 = 0;
        if (eeprom_read(0u, &w0) == 0 && eeprom_read(1u, &w1) == 0 && eeprom_read(2u, &w2) == 0) {
            hype_e1000_mac_from_eeprom(w0, w1, w2, &g_mac);
        }
    }
    if (!g_mac.valid) {
        hype_debug_print("e1000: no usable MAC from RAL/RAH or EEPROM -- refusing to come up "
                         "[#80]\n");
        return -1;
    }

    /* Rings. Guest-physical == host-physical for hype's own memory, so the descriptor addresses
     * are the buffers' own addresses. */
    for (i = 0; i < HYPE_E1000_RX_RING; i++) {
        uint8_t *d = g_rx_ring + i * HYPE_E1000_DESC_BYTES;
        desc_set_addr(d, (uint64_t)(uintptr_t)g_rx_bufs[i]);
        desc_set_len(d, 0u);
        rxd_clear_status(d);
    }
    for (i = 0; i < HYPE_E1000_TX_RING; i++) {
        uint8_t *d = g_tx_ring + i * HYPE_E1000_DESC_BYTES;
        desc_set_addr(d, (uint64_t)(uintptr_t)g_tx_bufs[i]);
        desc_set_len(d, 0u);
        txd_set_cmd(d, 0u);
        txd_clear_status(d);
    }

    reg_write(HYPE_E1000_REG_RDBAL, (uint32_t)(uintptr_t)g_rx_ring);
    reg_write(HYPE_E1000_REG_RDBAH, (uint32_t)((uint64_t)(uintptr_t)g_rx_ring >> 32));
    reg_write(HYPE_E1000_REG_RDLEN, rlen);
    reg_write(HYPE_E1000_REG_RDH, 0u);
    /* RDT points at the last descriptor the DRIVER owns. Handing the device the whole ring means
     * tail = size - 1. */
    reg_write(HYPE_E1000_REG_RDT, HYPE_E1000_RX_RING - 1u);
    g_rx_tail = HYPE_E1000_RX_RING - 1u;

    reg_write(HYPE_E1000_REG_TDBAL, (uint32_t)(uintptr_t)g_tx_ring);
    reg_write(HYPE_E1000_REG_TDBAH, (uint32_t)((uint64_t)(uintptr_t)g_tx_ring >> 32));
    reg_write(HYPE_E1000_REG_TDLEN, tlen);
    reg_write(HYPE_E1000_REG_TDH, 0u);
    reg_write(HYPE_E1000_REG_TDT, 0u);
    g_tx_tail = 0u;

    /* Link up, full duplex, auto-speed. */
    ctrl = reg_read(HYPE_E1000_REG_CTRL);
    reg_write(HYPE_E1000_REG_CTRL, ctrl | HYPE_E1000_CTRL_SLU | HYPE_E1000_CTRL_FD |
                                       HYPE_E1000_CTRL_ASDE);

    /* Receive: enabled, broadcast accepted, CRC stripped, 2048-byte buffers. Promiscuous is set
     * deliberately -- hype NATs for its guests, so frames addressed to a guest's MAC must be
     * accepted by the host NIC even though they are not addressed to it. */
    reg_write(HYPE_E1000_REG_RCTL, HYPE_E1000_RCTL_EN | HYPE_E1000_RCTL_BAM |
                                       HYPE_E1000_RCTL_SECRC | HYPE_E1000_RCTL_UPE |
                                       HYPE_E1000_RCTL_MPE | HYPE_E1000_RCTL_BSIZE_2048);
    /* Transmit: enabled, short packets padded to the 60-byte minimum. */
    reg_write(HYPE_E1000_REG_TCTL, HYPE_E1000_TCTL_EN | HYPE_E1000_TCTL_PSP);
    reg_write(HYPE_E1000_REG_TIPG, 0x0060200Au); /* IEEE 802.3 IPG, manual 13.4.34 */

    g_ready = 1;
    /*
     * READ BACK what was programmed rather than trusting the writes. Every other place in this
     * repo that talks to real hardware verifies its own setup (the ESP builder re-reads the FAT,
     * the PM timer self-tests its rate), and an RX ring that silently did not take is
     * indistinguishable from a network with no traffic on it.
     */
    hype_debug_print("e1000: readback RCTL=0x%x TCTL=0x%x STATUS=0x%x | RDBAL=0x%x RDLEN=%u "
                     "RDH=%u RDT=%u RDBAH=0x%x | TDBAL=0x%x TDLEN=%u TDBAH=0x%x [#80]\n",
                     reg_read(HYPE_E1000_REG_RCTL), reg_read(HYPE_E1000_REG_TCTL),
                     reg_read(HYPE_E1000_REG_STATUS), reg_read(HYPE_E1000_REG_RDBAL),
                     reg_read(HYPE_E1000_REG_RDLEN), reg_read(HYPE_E1000_REG_RDH),
                     reg_read(HYPE_E1000_REG_RDT), reg_read(HYPE_E1000_REG_RDBAH),
                     reg_read(HYPE_E1000_REG_TDBAL), reg_read(HYPE_E1000_REG_TDLEN),
                     reg_read(HYPE_E1000_REG_TDBAH));
    hype_debug_print("e1000: ring at %p, first RX desc addr=0x%llx len=%u [#80]\n",
                     (void *)g_rx_ring,
                     (unsigned long long)(*(volatile uint64_t *)(g_rx_ring + 0)),
                     (unsigned)desc_get_len(g_rx_ring));
    hype_debug_print("e1000: up at BAR 0x%llx, MAC %02x:%02x:%02x:%02x:%02x:%02x, link=%s, "
                     "rings rx=%u tx=%u [#80]\n",
                     (unsigned long long)bar_phys, g_mac.addr[0], g_mac.addr[1], g_mac.addr[2],
                     g_mac.addr[3], g_mac.addr[4], g_mac.addr[5],
                     (reg_read(HYPE_E1000_REG_STATUS) & HYPE_E1000_STATUS_LU) ? "up" : "DOWN",
                     HYPE_E1000_RX_RING, HYPE_E1000_TX_RING);
    return 0;
}

int hype_e1000_ready(void) { return g_ready; }

const hype_e1000_mac_t *hype_e1000_mac(void) { return &g_mac; }

int hype_e1000_link_up(void) {
    if (!g_ready) {
        return 0;
    }
    return (reg_read(HYPE_E1000_REG_STATUS) & HYPE_E1000_STATUS_LU) != 0u;
}

int hype_e1000_tx(const uint8_t *frame, unsigned int len) {
    uint8_t *d;
    unsigned int i;
    unsigned int head;
    unsigned long long spins;

    if (!g_ready || frame == 0 || len == 0u || len > HYPE_E1000_BUF_BYTES) {
        return -1;
    }
    head = reg_read(HYPE_E1000_REG_TDH) % HYPE_E1000_TX_RING;
    if (hype_e1000_ring_full(head, g_tx_tail, HYPE_E1000_TX_RING)) {
        g_tx_full++;
        return -1;
    }

    for (i = 0; i < len; i++) {
        g_tx_bufs[g_tx_tail][i] = frame[i];
    }
    d = g_tx_ring + g_tx_tail * HYPE_E1000_DESC_BYTES;
    desc_set_addr(d, (uint64_t)(uintptr_t)g_tx_bufs[g_tx_tail]);
    desc_set_len(d, (uint16_t)len);
    txd_clear_status(d);
    /* EOP: this descriptor is the whole frame. IFCS: the device appends the CRC. RS: write the
     * status back so completion is observable rather than assumed. */
    txd_set_cmd(d, HYPE_E1000_TXD_CMD_EOP | HYPE_E1000_TXD_CMD_IFCS | HYPE_E1000_TXD_CMD_RS);

    g_tx_tail = hype_e1000_ring_next(g_tx_tail, HYPE_E1000_TX_RING);
    reg_write(HYPE_E1000_REG_TDT, g_tx_tail);

    /* Wait for write-back. Bounded: a frame the device never acknowledges must report a failure
     * rather than wedge the caller, and the count says how often it happens. */
    for (spins = 0; spins < 10000000ull; spins++) {
        if (hype_e1000_txd_done(txd_status(d))) {
            g_tx_frames++;
            return 0;
        }
    }
    return -1;
}

int hype_e1000_poll_rx(uint8_t *out, unsigned int out_cap, unsigned int *out_len) {
    unsigned int next = hype_e1000_ring_next(g_rx_tail, HYPE_E1000_RX_RING);
    uint8_t *d = g_rx_ring + next * HYPE_E1000_DESC_BYTES;
    unsigned int len;
    unsigned int i;

    if (!g_ready || out == 0 || out_len == 0) {
        return 0;
    }
    if (!hype_e1000_rxd_done(rxd_status(d))) {
        return 0;
    }
    len = desc_get_len(d);
    if (len > out_cap) {
        /* Counted, not silently truncated: a frame larger than the caller's buffer is a
         * configuration mismatch worth seeing, and half a frame is worse than none. */
        g_rx_dropped++;
        len = 0u;
    } else {
        for (i = 0; i < len; i++) {
            out[i] = g_rx_bufs[next][i];
        }
        g_rx_frames++;
    }
    *out_len = len;

    /* Hand the descriptor back and advance the tail. */
    rxd_clear_status(d);
    desc_set_len(d, 0u);
    g_rx_tail = next;
    reg_write(HYPE_E1000_REG_RDT, g_rx_tail);
    return len != 0u;
}

/*
 * #80's bar: prove TX and RX with a real exchange rather than a register dump.
 *
 * An ARP request for `target_ip`, then poll for the reply. This exercises the transmit ring, the
 * receive ring, descriptor write-back and the MAC in one observable, and needs no IP stack -- which
 * is why it is the bring-up proof rather than a ping.
 *
 * Both frames' key fields are logged, so the result is evidence rather than a claim: a reply from
 * the wrong address, or an ARP for someone else, is visible instead of counted as success.
 *
 * `spins` bounds the wait. A gateway that never answers is a network fact, not a driver failure,
 * so this reports and returns rather than hanging the boot.
 */
int hype_e1000_arp_probe(const uint8_t our_ip[4], const uint8_t target_ip[4]) {
    uint8_t frame[HYPE_E1000_BUF_BYTES];
    unsigned int len = 0;
    unsigned long long spins;
    unsigned int sent;

    if (!g_ready || our_ip == 0 || target_ip == 0) {
        return -1;
    }
    sent = hype_arp_build_request(frame, sizeof(frame), g_mac.addr, our_ip, target_ip);
    if (sent == 0u) {
        return -1;
    }
    if (hype_e1000_tx(frame, sent) != 0) {
        hype_debug_print("net: ARP request for %u.%u.%u.%u was not transmitted [#80]\n",
                         target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
        return -1;
    }
    hype_debug_print("net: ARP request sent -- who has %u.%u.%u.%u, tell "
                     "%02x:%02x:%02x:%02x:%02x:%02x (%u.%u.%u.%u) [#80]\n",
                     target_ip[0], target_ip[1], target_ip[2], target_ip[3], g_mac.addr[0],
                     g_mac.addr[1], g_mac.addr[2], g_mac.addr[3], g_mac.addr[4], g_mac.addr[5],
                     our_ip[0], our_ip[1], our_ip[2], our_ip[3]);

    for (spins = 0; spins < 2000000ull; spins++) {
        hype_arp_t a;
        /*
         * Touch a DEVICE REGISTER each pass, not just the descriptor in RAM.
         *
         * The descriptor lives in hype's own memory, so a poll loop that only reads it performs no
         * MMIO and, under a hypervisor, no VM exit -- the vCPU spins in hardware and the emulator's
         * network backend may never be scheduled to deliver the frame. The reply was provably on
         * the wire (verified by pcap) while this loop read a descriptor the device had not been
         * given the chance to write. Reading the head pointer is what a real driver's poll does
         * anyway, and it yields.
         */
        (void)reg_read(HYPE_E1000_REG_RDH);
        if (!hype_e1000_poll_rx(frame, sizeof(frame), &len)) {
            continue;
        }
        if (!hype_arp_parse(frame, len, &a)) {
            continue; /* some other traffic -- not a failure, just not ours */
        }
        if (a.op != HYPE_ARP_OP_REPLY || !hype_arp_ip_eq(a.sender_ip, target_ip)) {
            continue; /* an ARP, but not the answer to our question */
        }
        hype_debug_print("net: ARP REPLY -- %u.%u.%u.%u is at %02x:%02x:%02x:%02x:%02x:%02x. "
                         "TX and RX both work [#80]\n",
                         a.sender_ip[0], a.sender_ip[1], a.sender_ip[2], a.sender_ip[3],
                         a.sender_mac[0], a.sender_mac[1], a.sender_mac[2], a.sender_mac[3],
                         a.sender_mac[4], a.sender_mac[5]);
        return 0;
    }
    /*
     * Say WHETHER THE DEVICE WROTE ANYTHING. "no reply" has two very different causes -- nothing
     * arrived on the wire, or something arrived and this driver's ring bookkeeping missed it --
     * and the head pointer plus the first descriptors' status bytes separate them. Without this
     * the two are indistinguishable, which is the #318 lesson applied before guessing.
     */
    hype_debug_print("net: no ARP reply for %u.%u.%u.%u -- tx=%llu rx=%llu dropped=%llu | "
                     "RDH=%u RDT=%u tail=%u | status[0..3]=%02x %02x %02x %02x [#80]\n",
                     target_ip[0], target_ip[1], target_ip[2], target_ip[3], g_tx_frames,
                     g_rx_frames, g_rx_dropped, reg_read(HYPE_E1000_REG_RDH),
                     reg_read(HYPE_E1000_REG_RDT), g_rx_tail,
                     rxd_status(g_rx_ring + 0u * HYPE_E1000_DESC_BYTES),
                     rxd_status(g_rx_ring + 1u * HYPE_E1000_DESC_BYTES),
                     rxd_status(g_rx_ring + 2u * HYPE_E1000_DESC_BYTES),
                     rxd_status(g_rx_ring + 3u * HYPE_E1000_DESC_BYTES));
    return -1;
}

void hype_e1000_stats(unsigned long long *tx, unsigned long long *tx_full, unsigned long long *rx,
                      unsigned long long *rx_dropped) {
    if (tx) *tx = g_tx_frames;
    if (tx_full) *tx_full = g_tx_full;
    if (rx) *rx = g_rx_frames;
    if (rx_dropped) *rx_dropped = g_rx_dropped;
}
