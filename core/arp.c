#include "arp.h"

static const uint8_t BROADCAST[HYPE_ETH_ALEN] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static uint16_t get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void copy_n(uint8_t *dst, const uint8_t *src, unsigned int n) {
    unsigned int i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static unsigned int build(uint8_t *out, unsigned int cap, uint16_t op, const uint8_t *dst_mac,
                          const uint8_t sender_mac[6], const uint8_t sender_ip[4],
                          const uint8_t target_mac[6], const uint8_t target_ip[4]) {
    uint8_t *a;

    if (out == 0 || sender_mac == 0 || sender_ip == 0 || target_ip == 0 ||
        cap < HYPE_ARP_FRAME_LEN) {
        return 0u; /* never a partial frame: a short one on the wire is worse than none */
    }

    /* Ethernet header. */
    copy_n(out + 0, dst_mac, HYPE_ETH_ALEN);
    copy_n(out + 6, sender_mac, HYPE_ETH_ALEN);
    put_be16(out + 12, HYPE_ETHERTYPE_ARP);

    /* ARP packet. */
    a = out + HYPE_ETH_HDR_LEN;
    put_be16(a + 0, HYPE_ARP_HTYPE_ETHERNET);
    put_be16(a + 2, HYPE_ETHERTYPE_IPV4); /* protocol type is the IPv4 EtherType */
    a[4] = (uint8_t)HYPE_ETH_ALEN;
    a[5] = 4u;
    put_be16(a + 6, op);
    copy_n(a + 8, sender_mac, HYPE_ETH_ALEN);
    copy_n(a + 14, sender_ip, 4u);
    /* A request has no target hardware address yet -- zeros, per RFC 826, and NOT the broadcast
     * address, which some stacks treat as malformed. */
    if (target_mac != 0) {
        copy_n(a + 18, target_mac, HYPE_ETH_ALEN);
    } else {
        unsigned int i;
        for (i = 0; i < HYPE_ETH_ALEN; i++) {
            a[18 + i] = 0u;
        }
    }
    copy_n(a + 24, target_ip, 4u);
    return HYPE_ARP_FRAME_LEN;
}

unsigned int hype_arp_build_request(uint8_t *out, unsigned int cap, const uint8_t sender_mac[6],
                                   const uint8_t sender_ip[4], const uint8_t target_ip[4]) {
    return build(out, cap, HYPE_ARP_OP_REQUEST, BROADCAST, sender_mac, sender_ip, 0, target_ip);
}

unsigned int hype_arp_build_reply(uint8_t *out, unsigned int cap, const uint8_t sender_mac[6],
                                 const uint8_t sender_ip[4], const uint8_t target_mac[6],
                                 const uint8_t target_ip[4]) {
    if (target_mac == 0) {
        return 0u; /* a reply with no destination is not a reply */
    }
    return build(out, cap, HYPE_ARP_OP_REPLY, target_mac, sender_mac, sender_ip, target_mac,
                 target_ip);
}

int hype_arp_parse(const uint8_t *frame, unsigned int len, hype_arp_t *out) {
    const uint8_t *a;

    if (frame == 0 || out == 0 || len < HYPE_ARP_FRAME_LEN) {
        return 0;
    }
    if (get_be16(frame + 12) != HYPE_ETHERTYPE_ARP) {
        return 0;
    }
    a = frame + HYPE_ETH_HDR_LEN;
    /*
     * Every one of these is a real refusal, not defensive noise. A frame claiming a hardware
     * length other than 6, or a protocol length other than 4, would make the offsets below point
     * somewhere else entirely -- and this data came off a wire hype does not control.
     */
    if (get_be16(a + 0) != HYPE_ARP_HTYPE_ETHERNET) {
        return 0;
    }
    if (get_be16(a + 2) != HYPE_ETHERTYPE_IPV4) {
        return 0;
    }
    if (a[4] != HYPE_ETH_ALEN || a[5] != 4u) {
        return 0;
    }
    out->op = get_be16(a + 6);
    if (out->op != HYPE_ARP_OP_REQUEST && out->op != HYPE_ARP_OP_REPLY) {
        return 0; /* RARP and the rest are not this stack's business */
    }
    copy_n(out->sender_mac, a + 8, HYPE_ETH_ALEN);
    copy_n(out->sender_ip, a + 14, 4u);
    copy_n(out->target_mac, a + 18, HYPE_ETH_ALEN);
    copy_n(out->target_ip, a + 24, 4u);
    return 1;
}

int hype_arp_mac_eq(const uint8_t a[6], const uint8_t b[6]) {
    unsigned int i;
    if (a == 0 || b == 0) {
        return 0;
    }
    for (i = 0; i < HYPE_ETH_ALEN; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int hype_arp_ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    unsigned int i;
    if (a == 0 || b == 0) {
        return 0;
    }
    for (i = 0; i < 4u; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}
