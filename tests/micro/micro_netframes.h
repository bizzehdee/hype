#ifndef HYPE_MICRO_NETFRAMES_H
#define HYPE_MICRO_NETFRAMES_H

/*
 * The frames a network test sends, independent of which NIC sends them.
 *
 * Split out of micro_vnet.h when the e1000 tests (#82) needed the builders and not the virtio
 * driver: including a header for a third of its contents made -Werror reject the other two thirds as
 * unused, and that was the right complaint. An ARP request, a DNS query and an ICMP echo are the same
 * bytes whichever device puts them on the wire, and sending byte-identical traffic over both
 * frontends is exactly how hype's forwarding plane is shown not to care which one a guest has.
 *
 * `g_mac` is filled in by the DRIVER's bring-up -- micro_vnet.h's or micro_e1000.h's -- before any
 * builder here is called.
 */

#include <stdint.h>

static uint8_t g_mac[6];

/*
 * The guest's own address and its gateway. WRITABLE, and set by the including test before
 * micro_vnet_up() -- two guests on one host need different addresses, so these cannot be constants
 * here. hype is never told either of them (it answers every ARP with its own MAC and learns the
 * guest's address from what it answered), so these only have to agree with each other.
 */
static uint8_t MY_IP[4] = {192, 168, 77, 2};
static uint8_t GW_IP[4] = {192, 168, 77, 1};

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint16_t inet_csum(const uint8_t *d, unsigned int len) {
    uint32_t sum = 0;
    unsigned int i = 0;
    while (i + 1u < len) {
        sum += (uint32_t)be16(d + i);
        i += 2u;
    }
    if (i < len) {
        sum += (uint32_t)((uint32_t)d[i] << 8);
    }
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

/*
 * `static inline`, like micro.h's own helpers: these two builders are OPTIONAL -- a test that only
 * pings does not build a DNS query and vice versa -- and a plain `static` one goes unused in that
 * test, which -Werror turns into a build failure.
 *
 * A DNS A query for www.google.com in a UDP datagram in an IPv4 packet in an Ethernet frame.
 * Hand-built because a guest this small has no stack, which is also what makes it a good test: every
 * field is one hype has to get right on the way through.
 */
static inline unsigned int build_dns(uint8_t *f, const uint8_t gw_mac[6], const uint8_t dns_ip[4],
                              uint16_t txid, uint16_t sport) {
    static const char *labels[] = {"www", "google", "com"};
    unsigned int i;
    unsigned int n;
    unsigned int q;
    unsigned int udp_off = 14u + 20u;
    unsigned int dns_off = udp_off + 8u;
    unsigned int dns_len;

    for (i = 0; i < 6u; i++) {
        f[i] = gw_mac[i];
        f[6 + i] = g_mac[i];
    }
    put_be16(f + 12, 0x0800u);

    /* DNS message. */
    n = dns_off;
    put_be16(f + n, txid);
    put_be16(f + n + 2, 0x0100u); /* standard query, recursion desired */
    put_be16(f + n + 4, 1u);      /* one question */
    put_be16(f + n + 6, 0u);
    put_be16(f + n + 8, 0u);
    put_be16(f + n + 10, 0u);
    n += 12u;
    for (q = 0; q < 3u; q++) {
        unsigned int l = 0;
        while (labels[q][l] != '\0') {
            l++;
        }
        f[n++] = (uint8_t)l;
        for (i = 0; i < l; i++) {
            f[n++] = (uint8_t)labels[q][i];
        }
    }
    f[n++] = 0u;          /* root label */
    put_be16(f + n, 1u);  /* QTYPE A */
    n += 2u;
    put_be16(f + n, 1u);  /* QCLASS IN */
    n += 2u;
    dns_len = n - dns_off;

    /* UDP. */
    put_be16(f + udp_off, sport);
    put_be16(f + udp_off + 2, 53u);
    put_be16(f + udp_off + 4, (uint16_t)(8u + dns_len));
    put_be16(f + udp_off + 6, 0u);

    /* IPv4. */
    f[14] = 0x45u;
    f[15] = 0u;
    put_be16(f + 16, (uint16_t)(20u + 8u + dns_len));
    put_be16(f + 18, 0u);
    put_be16(f + 20, 0u);
    f[22] = 64u; /* TTL */
    f[23] = 17u; /* UDP */
    put_be16(f + 24, 0u);
    for (i = 0; i < 4u; i++) {
        f[26 + i] = MY_IP[i];
        f[30 + i] = dns_ip[i];
    }
    put_be16(f + 24, inet_csum(f + 14, 20u));

    /*
     * The UDP checksum is left at 0, which means "not computed" and is legal for IPv4 UDP. That is
     * deliberate: it also exercises hype's rule that a zero checksum must stay zero through
     * translation rather than being adjusted into a nonzero value that is wrong.
     */
    return n;
}

/* An ICMP echo, type 8 for a request and 0 for a reply, in an IPv4 packet in an Ethernet frame. */
static inline unsigned int build_echo(uint8_t *f, const uint8_t gw_mac[6], const uint8_t dst_ip[4],
                               uint8_t type, uint16_t id, uint16_t seq) {
    unsigned int i;
    unsigned int icmp_off = 14u + 20u;
    unsigned int icmp_len = 8u + 16u; /* header plus a small payload */

    for (i = 0; i < 6u; i++) {
        f[i] = gw_mac[i];
        f[6 + i] = g_mac[i];
    }
    put_be16(f + 12, 0x0800u);

    f[14] = 0x45u;
    f[15] = 0u;
    put_be16(f + 16, (uint16_t)(20u + icmp_len));
    put_be16(f + 18, 0u);
    put_be16(f + 20, 0u);
    f[22] = 64u;
    f[23] = 1u; /* ICMP */
    put_be16(f + 24, 0u);
    for (i = 0; i < 4u; i++) {
        f[26 + i] = MY_IP[i];
        f[30 + i] = dst_ip[i];
    }
    put_be16(f + 24, inet_csum(f + 14, 20u));

    f[icmp_off] = type;
    f[icmp_off + 1] = 0u;
    put_be16(f + icmp_off + 2, 0u);
    put_be16(f + icmp_off + 4, id);
    put_be16(f + icmp_off + 6, seq);
    for (i = 0; i < 16u; i++) {
        f[icmp_off + 8u + i] = (uint8_t)(0xA0u + i);
    }
    put_be16(f + icmp_off + 2, inet_csum(f + icmp_off, icmp_len));
    return icmp_off + icmp_len;
}


/* Builds an ARP request for the gateway, which is what makes hype learn this guest's address. */
static inline unsigned int build_arp(uint8_t *f) {
    unsigned int i;
    for (i = 0; i < 6u; i++) {
        f[i] = 0xFFu;
        f[6 + i] = g_mac[i];
    }
    put_be16(f + 12, 0x0806u);
    put_be16(f + 14, 1u);      /* Ethernet */
    put_be16(f + 16, 0x0800u); /* IPv4 */
    f[18] = 6u;
    f[19] = 4u;
    put_be16(f + 20, 1u); /* request */
    for (i = 0; i < 6u; i++) {
        f[22 + i] = g_mac[i];
        f[32 + i] = 0u;
    }
    for (i = 0; i < 4u; i++) {
        f[28 + i] = MY_IP[i];
        f[38 + i] = GW_IP[i];
    }
    return 42u;
}

#endif /* HYPE_MICRO_NETFRAMES_H */
