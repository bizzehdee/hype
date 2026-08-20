#include <stdio.h>
#include <string.h>
#include "../nat.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

#define CHECK_TRUE(desc, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", (desc)); failures++; } } while (0)

static const uint8_t GUEST_IP[4] = {10, 0, 2, 15};
static const uint8_t OUR_IP[4] = {192, 168, 0, 42};
static const uint8_t REMOTE_IP[4] = {142, 250, 187, 100};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFFu); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

/*
 * Builds a real IPv4 packet with a correct header checksum and a correct L4 checksum, because the
 * whole point of the translation tests is that the checksums are still correct AFTERWARDS. A test
 * built on a packet whose checksum was wrong to begin with would pass whatever the code did to it.
 */
static unsigned int build(uint8_t *buf, uint8_t proto, const uint8_t src[4], const uint8_t dst[4],
                          uint16_t sport, uint16_t dport, const uint8_t *payload,
                          unsigned int payload_len) {
    unsigned int l4_len;
    unsigned int total;
    unsigned int i;

    switch (proto) {
        case HYPE_IPV4_PROTO_UDP: l4_len = 8u; break;
        case HYPE_IPV4_PROTO_TCP: l4_len = 20u; break;
        default: l4_len = 8u; break; /* ICMP echo */
    }
    total = 20u + l4_len + payload_len;

    memset(buf, 0, total);
    buf[0] = 0x45u; /* IPv4, IHL 5 */
    put16(buf + 2, (uint16_t)total);
    buf[8] = 64u;   /* TTL */
    buf[9] = proto;
    for (i = 0; i < 4u; i++) {
        buf[12 + i] = src[i];
        buf[16 + i] = dst[i];
    }
    put16(buf + 10, 0);
    put16(buf + 10, hype_inet_checksum(buf, 20u));

    if (proto == HYPE_IPV4_PROTO_ICMP) {
        buf[20] = HYPE_ICMP_TYPE_ECHO_REQUEST;
        buf[21] = 0;
        put16(buf + 24, sport); /* identifier */
        put16(buf + 26, 1u);    /* sequence */
    } else {
        put16(buf + 20, sport);
        put16(buf + 22, dport);
        if (proto == HYPE_IPV4_PROTO_UDP) {
            put16(buf + 24, (uint16_t)(l4_len + payload_len));
        } else {
            buf[32] = 0x50u; /* data offset 5 */
            buf[33] = 0x18u; /* PSH|ACK */
        }
    }
    for (i = 0; i < payload_len; i++) {
        buf[20 + l4_len + i] = payload[i];
    }

    /* L4 checksum. ICMP's covers the ICMP header and payload only; TCP/UDP add a pseudo-header of
     * src, dst, zero, proto, length. Computed the long way here so the incremental fixup under test
     * has something real to be checked against. */
    if (proto == HYPE_IPV4_PROTO_ICMP) {
        put16(buf + 22, 0);
        put16(buf + 22, hype_inet_checksum(buf + 20, l4_len + payload_len));
    } else {
        uint8_t pseudo[40];
        unsigned int n = 0;
        unsigned int csum_off = (proto == HYPE_IPV4_PROTO_UDP) ? 6u : 16u;
        put16(buf + 20 + csum_off, 0);
        for (i = 0; i < 4u; i++) pseudo[n++] = src[i];
        for (i = 0; i < 4u; i++) pseudo[n++] = dst[i];
        pseudo[n++] = 0;
        pseudo[n++] = proto;
        put16(pseudo + n, (uint16_t)(l4_len + payload_len));
        n += 2u;
        for (i = 0; i < l4_len + payload_len; i++) {
            pseudo[n++] = buf[20 + i];
        }
        put16(buf + 20 + csum_off, hype_inet_checksum(pseudo, n));
    }
    return total;
}

/* Verifies both checksums are correct as the packet now stands. A checksum over a whole packet
 * INCLUDING its own correct checksum field comes out as 0. */
static int checksums_ok(const uint8_t *buf, unsigned int len) {
    unsigned int ihl = (unsigned int)(buf[0] & 0x0Fu) * 4u;
    uint8_t proto = buf[9];
    unsigned int l4_len = len - ihl;
    unsigned int i;

    if (hype_inet_checksum(buf, ihl) != 0u) {
        printf("  (IP header checksum is wrong)\n");
        return 0;
    }
    if (proto == HYPE_IPV4_PROTO_ICMP) {
        if (hype_inet_checksum(buf + ihl, l4_len) != 0u) {
            printf("  (ICMP checksum is wrong)\n");
            return 0;
        }
        return 1;
    }
    {
        uint8_t pseudo[64];
        unsigned int n = 0;
        unsigned int csum_off = (proto == HYPE_IPV4_PROTO_UDP) ? 6u : 16u;
        if (proto == HYPE_IPV4_PROTO_UDP && get16(buf + ihl + csum_off) == 0u) {
            return 1; /* "not computed" is a legal UDP checksum */
        }
        for (i = 0; i < 4u; i++) pseudo[n++] = buf[12 + i];
        for (i = 0; i < 4u; i++) pseudo[n++] = buf[16 + i];
        pseudo[n++] = 0;
        pseudo[n++] = proto;
        put16(pseudo + n, (uint16_t)l4_len);
        n += 2u;
        for (i = 0; i < l4_len; i++) pseudo[n++] = buf[ihl + i];
        if (hype_inet_checksum(pseudo, n) != 0u) {
            printf("  (L4 checksum is wrong)\n");
            return 0;
        }
    }
    return 1;
}

/* A known-value check, so a broken checksum routine cannot make every other test pass by being
 * consistently wrong in both directions. */
static void test_checksum_known_value(void) {
    /* RFC 1071's worked example bytes. */
    const uint8_t data[8] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK_HEX("RFC 1071 worked example", 0x220du, hype_inet_checksum(data, sizeof(data)));
    /* A buffer whose checksum is already correct sums to zero. */
    {
        uint8_t buf[6] = {0x11, 0x22, 0x33, 0x44, 0, 0};
        put16(buf + 4, hype_inet_checksum(buf, sizeof(buf)));
        CHECK_HEX("a correct checksum makes the whole buffer sum to 0", 0,
                  hype_inet_checksum(buf, sizeof(buf)));
    }
    /* Odd length: the trailing byte is padded on the right, so 0x01 counts as 0x0100. */
    {
        const uint8_t odd[3] = {0x00, 0x00, 0x01};
        CHECK_HEX("odd trailing byte pads right", (uint16_t)~0x0100u & 0xFFFFu,
                  hype_inet_checksum(odd, sizeof(odd)));
    }
}

static void test_udp_round_trip(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t xlate;
    uint8_t who[4] = {0, 0, 0, 0};
    const uint8_t payload[4] = {'d', 'n', 's', '?'};

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, payload,
                sizeof(payload));
    CHECK_TRUE("the packet we built is valid to begin with", checksums_ok(pkt, len));

    CHECK_HEX("outbound translated", 0, hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));
    CHECK_HEX("source address is now hype's", 192, pkt[12]);
    CHECK_HEX("...and the last octet", 42, pkt[15]);
    xlate = get16(pkt + 20);
    CHECK_TRUE("source port was substituted from the NAT range",
               xlate >= HYPE_NAT_PORT_BASE && xlate < HYPE_NAT_PORT_BASE + HYPE_NAT_PORT_COUNT);
    CHECK_HEX("destination port untouched", 53u, get16(pkt + 22));
    CHECK_TRUE("both checksums are still correct after rewriting", checksums_ok(pkt, len));
    CHECK_HEX("one mapping is live", 1, hype_nat_active(&nat));

    /* The reply, as the far end would send it: to hype's address and the translated port. */
    len = build(pkt, HYPE_IPV4_PROTO_UDP, REMOTE_IP, OUR_IP, 53u, xlate, payload, sizeof(payload));
    CHECK_HEX("inbound translated", 0, hype_nat_translate_inbound(&nat, pkt, len, who, 2ull));
    CHECK_HEX("destination is the guest again", 10, pkt[16]);
    CHECK_HEX("...and its last octet", 15, pkt[19]);
    CHECK_HEX("destination port is the guest's own again", 5353u, get16(pkt + 22));
    CHECK_TRUE("checksums correct after the return rewrite", checksums_ok(pkt, len));
    CHECK_HEX("the caller is told which guest it belongs to", 15, who[3]);
    CHECK_HEX("counted", 1, nat.in_translated);
}

static void test_icmp_echo_round_trip(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t xlate;
    uint8_t who[4] = {0, 0, 0, 0};

    hype_nat_reset(&nat);
    /* This is the shape of `ping www.google.com`: an echo request whose identifier is the mapping
     * key, because ICMP has no ports. */
    len = build(pkt, HYPE_IPV4_PROTO_ICMP, GUEST_IP, REMOTE_IP, 0x1234u, 0u, 0, 0);
    CHECK_TRUE("valid to begin with", checksums_ok(pkt, len));
    CHECK_HEX("translated", 0, hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));
    xlate = get16(pkt + 24);
    CHECK_TRUE("the echo identifier was substituted", xlate != 0x1234u);
    CHECK_TRUE("checksums still correct", checksums_ok(pkt, len));

    /* The echo REPLY comes back with the same identifier and type 0. */
    len = build(pkt, HYPE_IPV4_PROTO_ICMP, REMOTE_IP, OUR_IP, xlate, 0u, 0, 0);
    pkt[20] = HYPE_ICMP_TYPE_ECHO_REPLY;
    put16(pkt + 22, 0);
    put16(pkt + 22, hype_inet_checksum(pkt + 20, len - 20u));
    CHECK_HEX("reply translated", 0, hype_nat_translate_inbound(&nat, pkt, len, who, 2ull));
    CHECK_HEX("the identifier is the guest's own again", 0x1234u, get16(pkt + 24));
    CHECK_HEX("addressed to the guest", 15, pkt[19]);
    CHECK_TRUE("checksums correct", checksums_ok(pkt, len));
}

/*
 * THE PROPERTY THAT MAKES THIS A NAT RATHER THAN A HOLE: an inbound packet with no mapping is
 * dropped. Without it, anyone who guessed a translated port would reach a guest, and "outbound plus
 * established return" would mean "outbound plus whatever anyone sends".
 */
static void test_unsolicited_inbound_is_dropped(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint8_t who[4] = {9, 9, 9, 9};

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, REMOTE_IP, OUR_IP, 53u, HYPE_NAT_PORT_BASE, 0, 0);
    CHECK_HEX("refused", -1, hype_nat_translate_inbound(&nat, pkt, len, who, 1ull));
    CHECK_HEX("counted as unsolicited, not as an error", 1, nat.in_dropped_no_mapping);
    CHECK_HEX("the out parameter was not written", 9, who[0]);
}

/*
 * A mapping is to ONE remote host. A reply arriving on the right translated port from a DIFFERENT
 * source must not be delivered -- otherwise the port alone is the credential.
 */
static void test_reply_from_the_wrong_host_is_dropped(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t xlate;
    const uint8_t impostor[4] = {203, 0, 113, 7};

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    xlate = get16(pkt + 20);

    len = build(pkt, HYPE_IPV4_PROTO_UDP, impostor, OUR_IP, 53u, xlate, 0, 0);
    CHECK_HEX("a reply from another host on the right port is refused", -1,
              hype_nat_translate_inbound(&nat, pkt, len, 0, 2ull));
    CHECK_HEX("counted as having no mapping", 1, nat.in_dropped_no_mapping);

    /* And the same host on the wrong PORT is equally not a match. */
    len = build(pkt, HYPE_IPV4_PROTO_UDP, REMOTE_IP, OUR_IP, 54u, xlate, 0, 0);
    CHECK_HEX("a reply from the right host but the wrong port is refused", -1,
              hype_nat_translate_inbound(&nat, pkt, len, 0, 2ull));
}

/* One guest, one source port, two different services on one host: two mappings, because the remote
 * port is part of the key. Collapsing them would cross two conversations' replies. */
static void test_same_source_port_to_two_services(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t x80;
    uint16_t x443;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40000u, 80u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    x80 = get16(pkt + 20);

    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40000u, 443u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    x443 = get16(pkt + 20);

    CHECK_HEX("two mappings", 2, hype_nat_active(&nat));
    CHECK_TRUE("and two distinct translated ports", x80 != x443);
}

/* Two guests may legitimately use the SAME source port to the same service. They must get different
 * translated ports, or their replies go to the wrong VM -- which is a cross-VM data leak, not just a
 * bug. */
static void test_two_guests_same_port_do_not_collide(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t xa;
    uint16_t xb;
    const uint8_t guest_b[4] = {10, 0, 2, 16};
    uint8_t who[4] = {0, 0, 0, 0};

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    xa = get16(pkt + 20);

    len = build(pkt, HYPE_IPV4_PROTO_UDP, guest_b, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    xb = get16(pkt + 20);

    CHECK_TRUE("distinct translated ports for distinct guests", xa != xb);

    len = build(pkt, HYPE_IPV4_PROTO_UDP, REMOTE_IP, OUR_IP, 53u, xb, 0, 0);
    CHECK_HEX("translated", 0, hype_nat_translate_inbound(&nat, pkt, len, who, 2ull));
    CHECK_HEX("and it went to the SECOND guest, not the first", 16, who[3]);
}

static void test_fragments_are_dropped_with_their_own_reason(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    put16(pkt + 6, 0x2000u); /* MF set: more fragments follow */
    put16(pkt + 10, 0);
    put16(pkt + 10, hype_inet_checksum(pkt, 20u));
    CHECK_HEX("refused", -1, hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));
    CHECK_HEX("counted as a fragment, not as malformed", 1, nat.out_dropped_fragment);
    CHECK_HEX("and not as malformed", 0, nat.out_dropped_malformed);

    /* A non-first fragment, which has no L4 header at all. */
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    put16(pkt + 6, 185u); /* nonzero offset */
    put16(pkt + 10, 0);
    put16(pkt + 10, hype_inet_checksum(pkt, 20u));
    CHECK_HEX("also refused", -1, hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));
    CHECK_HEX("counted", 2, nat.out_dropped_fragment);
}

/*
 * A header whose own total_length exceeds the frame is the classic way to walk a parser off the end
 * of its buffer. Refused, not clamped.
 */
static void test_malformed_packets_are_refused(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);

    put16(pkt + 2, 4000u); /* total_length far past the frame */
    CHECK_HEX("a total_length past the frame is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));

    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    pkt[0] = 0x40u; /* IHL 0 */
    CHECK_HEX("an IHL below the minimum is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));

    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    pkt[0] = 0x65u; /* version 6 */
    CHECK_HEX("a non-IPv4 version is refused rather than half-handled", -1,
              hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));

    CHECK_HEX("a frame shorter than an IPv4 header is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, 12u, OUR_IP, 1ull));
    CHECK_HEX("all four counted as malformed", 4, nat.out_dropped_malformed);

    CHECK_HEX("a null table is refused", -1,
              hype_nat_translate_outbound(0, pkt, len, OUR_IP, 1ull));
    CHECK_HEX("a null address is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, len, 0, 1ull));
    CHECK_HEX("a null table inbound is refused", -1, hype_nat_translate_inbound(0, pkt, len, 0, 1ull));
    CHECK_HEX("a truncated L4 header is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, 22u, OUR_IP, 1ull));
}

static void test_unsupported_protocols_are_dropped(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    pkt[9] = 47u; /* GRE */
    put16(pkt + 10, 0);
    put16(pkt + 10, hype_inet_checksum(pkt, 20u));
    CHECK_HEX("a protocol with no translatable identifier is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));
    CHECK_HEX("counted as unsupported, distinct from malformed", 1, nat.out_dropped_unsupported);

    /* An ICMP error is ABOUT another packet; routing it back needs that packet's header read out of
     * the payload, which is separate work. Dropped rather than guessed at. */
    len = build(pkt, HYPE_IPV4_PROTO_ICMP, GUEST_IP, REMOTE_IP, 1u, 0u, 0, 0);
    pkt[20] = 3u; /* destination unreachable */
    put16(pkt + 22, 0);
    put16(pkt + 22, hype_inet_checksum(pkt + 20, len - 20u));
    CHECK_HEX("an ICMP error is refused rather than mis-delivered", -1,
              hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));
    CHECK_HEX("counted", 2, nat.out_dropped_unsupported);
}

/* A UDP checksum of 0 means "not computed". Adjusting from it would produce a nonzero value that is
 * WRONG, which is worse than leaving it absent. */
static void test_zero_udp_checksum_is_left_alone(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    put16(pkt + 26, 0); /* UDP checksum field = "not computed" */
    CHECK_HEX("translated", 0, hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull));
    CHECK_HEX("the absent checksum is still absent, not a wrong number", 0, get16(pkt + 26));
    CHECK_TRUE("the IP header checksum is still correct", checksums_ok(pkt, len));
}

static void test_the_table_fills_and_refuses_new_flows(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    unsigned int i;
    unsigned int refused = 0;

    hype_nat_reset(&nat);
    for (i = 0; i < HYPE_NAT_MAX_CONN + 4u; i++) {
        len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, (uint16_t)(20000u + i), 53u, 0,
                    0);
        if (hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull) != 0) {
            refused++;
        }
    }
    CHECK_HEX("the table filled to its capacity", HYPE_NAT_MAX_CONN, hype_nat_active(&nat));
    CHECK_HEX("and the excess flows were refused", 4, refused);
    CHECK_HEX("counted with their own reason", 4, nat.out_dropped_no_slot);

    /* An EXISTING flow still works with the table full -- refusing new ones must not break the ones
     * already established, which is the point of not reclaiming the oldest entry. */
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 20000u, 53u, 0, 0);
    CHECK_HEX("an established flow is unaffected by a full table", 0,
              hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 2ull));
}

static void test_idle_mappings_expire_by_protocol(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_ICMP, GUEST_IP, REMOTE_IP, 1u, 0u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 100ull);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 100ull);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40000u, 80u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 100ull);
    CHECK_HEX("three mappings", 3, hype_nat_active(&nat));

    CHECK_HEX("nothing expires while all are fresh", 0, hype_nat_expire(&nat, 110ull));

    /* ICMP has the shortest timeout: an echo is a single exchange, and a mapping outliving it is a
     * port held for nothing. */
    CHECK_HEX("the ICMP mapping goes first", 1,
              hype_nat_expire(&nat, 100ull + HYPE_NAT_IDLE_TICKS_ICMP));
    CHECK_HEX("two left", 2, hype_nat_active(&nat));

    CHECK_HEX("then UDP", 1, hype_nat_expire(&nat, 100ull + HYPE_NAT_IDLE_TICKS_UDP));
    CHECK_HEX("one left", 1, hype_nat_active(&nat));

    /* TCP survives far longer, because an established connection may legitimately idle. */
    CHECK_HEX("TCP is still alive well past the UDP timeout", 1, hype_nat_active(&nat));
    CHECK_HEX("and expires on its own schedule", 1,
              hype_nat_expire(&nat, 100ull + HYPE_NAT_IDLE_TICKS_TCP));
    CHECK_HEX("empty", 0, hype_nat_active(&nat));
    CHECK_HEX("a null table expires nothing", 0, hype_nat_expire(0, 1ull));
    CHECK_HEX("a null table has no active mappings", 0, hype_nat_active(0));
}

/*
 * A clock that went BACKWARDS must not look like enormous age. Treating it that way would flush
 * every mapping on every guest at once -- far worse than holding one stale entry for a sweep.
 */
static void test_a_backwards_clock_does_not_flush_everything(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40000u, 80u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 100000ull);
    CHECK_HEX("nothing expired by a clock that went backwards", 0, hype_nat_expire(&nat, 5ull));
    CHECK_HEX("the mapping survives", 1, hype_nat_active(&nat));
    /* And it is now aged from the new clock, so it expires normally from here. */
    CHECK_HEX("it expires on the new clock", 1,
              hype_nat_expire(&nat, 5ull + HYPE_NAT_IDLE_TICKS_TCP));
}

/* TCP teardown releases the mapping instead of holding a translated port for the full idle timeout,
 * and a RST ends it immediately from either side. */
static void test_tcp_teardown_releases_the_mapping(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t xlate;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40000u, 80u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    xlate = get16(pkt + 20);
    CHECK_HEX("live", 1, hype_nat_active(&nat));

    /* FIN from the guest: half closed, mapping still needed for the other direction. */
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40000u, 80u, 0, 0);
    pkt[33] = 0x11u; /* FIN|ACK */
    put16(pkt + 36, 0);
    CHECK_HEX("translated", 0, hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 2ull));
    CHECK_HEX("a half close keeps the mapping", 1, hype_nat_active(&nat));

    /* FIN from the remote closes the other half, and the mapping goes. */
    len = build(pkt, HYPE_IPV4_PROTO_TCP, REMOTE_IP, OUR_IP, 80u, xlate, 0, 0);
    pkt[33] = 0x11u;
    put16(pkt + 36, 0);
    CHECK_HEX("translated", 0, hype_nat_translate_inbound(&nat, pkt, len, 0, 3ull));
    CHECK_HEX("both halves closed releases it", 0, hype_nat_active(&nat));

    /* RST ends it in one packet. */
    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40001u, 80u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40001u, 80u, 0, 0);
    pkt[33] = 0x04u; /* RST */
    put16(pkt + 36, 0);
    CHECK_HEX("translated -- the reset still has to reach the far end", 0,
              hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 2ull));
    CHECK_HEX("and the mapping is gone", 0, hype_nat_active(&nat));
}

/* An inbound RST from the remote does the same. */
static void test_inbound_rst_releases_the_mapping(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t xlate;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, GUEST_IP, REMOTE_IP, 40002u, 80u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    xlate = get16(pkt + 20);
    len = build(pkt, HYPE_IPV4_PROTO_TCP, REMOTE_IP, OUR_IP, 80u, xlate, 0, 0);
    pkt[33] = 0x04u;
    put16(pkt + 36, 0);
    CHECK_HEX("translated", 0, hype_nat_translate_inbound(&nat, pkt, len, 0, 2ull));
    CHECK_HEX("released", 0, hype_nat_active(&nat));
}

/* The same flow translated twice must reuse its mapping rather than allocating a second one -- one
 * mapping per flow is what makes the table's capacity mean anything. */
static void test_the_same_flow_reuses_its_mapping(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t first;
    uint16_t second;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    first = get16(pkt + 20);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 2ull);
    second = get16(pkt + 20);
    CHECK_HEX("same translated port", first, second);
    CHECK_HEX("one mapping, not two", 1, hype_nat_active(&nat));
    CHECK_HEX("one connection opened", 1, nat.conns_opened);
}

/* Traffic refreshes a mapping's idle timer, or a busy connection would be torn down mid-use. */
static void test_traffic_refreshes_the_timer(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 100ull);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 200ull);
    CHECK_HEX("not expired at what would have been the original deadline", 0,
              hype_nat_expire(&nat, 100ull + HYPE_NAT_IDLE_TICKS_UDP));
    CHECK_HEX("still live", 1, hype_nat_active(&nat));
    CHECK_HEX("expires from the refreshed time", 1,
              hype_nat_expire(&nat, 200ull + HYPE_NAT_IDLE_TICKS_UDP));
}

/* A packet with IP options (IHL > 5) must have its L4 header located from the real IHL, not from a
 * hardcoded 20 -- otherwise the port rewrite lands in the options. */
static void test_ip_options_shift_the_l4_header(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int i;
    unsigned int total = 24u + 8u; /* IHL 6 (one 4-byte option) + UDP */

    hype_nat_reset(&nat);
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x46u; /* IPv4, IHL 6 */
    put16(pkt + 2, (uint16_t)total);
    pkt[8] = 64u;
    pkt[9] = HYPE_IPV4_PROTO_UDP;
    for (i = 0; i < 4u; i++) {
        pkt[12 + i] = GUEST_IP[i];
        pkt[16 + i] = REMOTE_IP[i];
    }
    pkt[20] = 0x01u; /* a NOP option, then padding */
    put16(pkt + 10, hype_inet_checksum(pkt, 24u));
    put16(pkt + 24, 5353u); /* source port, at IHL 6 */
    put16(pkt + 26, 53u);
    put16(pkt + 28, 8u);
    put16(pkt + 30, 0);     /* checksum "not computed", so no pseudo-header maths needed here */

    CHECK_HEX("translated", 0, hype_nat_translate_outbound(&nat, pkt, total, OUR_IP, 1ull));
    CHECK_TRUE("the port at the real L4 offset was rewritten",
               get16(pkt + 24) >= HYPE_NAT_PORT_BASE);
    CHECK_HEX("the option bytes were not touched", 0x01u, pkt[20]);
    CHECK_HEX("the IP header checksum covers the options too", 0,
              hype_inet_checksum(pkt, 24u));
}

/*
 * Headers that are SELF-CONSISTENT but too short for the transport header they claim. These are
 * distinct from the "total_length past the frame" case: here the packet fits, so the outer bounds
 * check passes and it is the L4 length that has to catch it. Left uncovered, these are exactly the
 * paths that read a port out of bytes that are not there.
 */
static void test_headers_too_short_for_their_own_protocol(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int i;

    hype_nat_reset(&nat);

    /* total_length = 24: a full IPv4 header plus 4 bytes, which is not a UDP header. */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x45u;
    put16(pkt + 2, 24u);
    pkt[9] = HYPE_IPV4_PROTO_UDP;
    for (i = 0; i < 4u; i++) { pkt[12 + i] = GUEST_IP[i]; pkt[16 + i] = REMOTE_IP[i]; }
    put16(pkt + 10, hype_inet_checksum(pkt, 20u));
    CHECK_HEX("a UDP packet with only 4 bytes of transport header is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, 24u, OUR_IP, 1ull));

    /* Same, for TCP, which needs 20. */
    pkt[9] = HYPE_IPV4_PROTO_TCP;
    put16(pkt + 2, 32u);
    put16(pkt + 10, 0);
    put16(pkt + 10, hype_inet_checksum(pkt, 20u));
    CHECK_HEX("a TCP packet with 12 bytes of header is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, 32u, OUR_IP, 1ull));

    /* And ICMP, which needs 8. */
    pkt[9] = HYPE_IPV4_PROTO_ICMP;
    put16(pkt + 2, 24u);
    put16(pkt + 10, 0);
    put16(pkt + 10, hype_inet_checksum(pkt, 20u));
    pkt[20] = HYPE_ICMP_TYPE_ECHO_REQUEST;
    CHECK_HEX("an ICMP echo with 4 bytes of header is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, 24u, OUR_IP, 1ull));

    /* total_length SMALLER than the header it declares. */
    pkt[0] = 0x46u; /* IHL 6 = 24 bytes */
    put16(pkt + 2, 20u); /* but total says 20 */
    put16(pkt + 10, 0);
    put16(pkt + 10, hype_inet_checksum(pkt, 24u));
    CHECK_HEX("a total_length below the header length is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, 32u, OUR_IP, 1ull));

    /* An IHL that runs past the frame, even though the frame is longer than a minimum header. */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x4Fu; /* IHL 15 = 60 bytes */
    put16(pkt + 2, 60u);
    pkt[9] = HYPE_IPV4_PROTO_UDP;
    CHECK_HEX("an IHL past the end of the frame is refused", -1,
              hype_nat_translate_outbound(&nat, pkt, 24u, OUR_IP, 1ull));

    /* And the same checks on the inbound path, which has its own parse call. */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x45u;
    put16(pkt + 2, 24u);
    pkt[9] = HYPE_IPV4_PROTO_UDP;
    put16(pkt + 10, hype_inet_checksum(pkt, 20u));
    CHECK_HEX("inbound refuses a short transport header too", -1,
              hype_nat_translate_inbound(&nat, pkt, 24u, 0, 1ull));
    CHECK_HEX("an inbound fragment is refused", -1,
              (put16(pkt + 6, 0x2000u), hype_nat_translate_inbound(&nat, pkt, 24u, 0, 1ull)));
}

static void test_null_arguments(void) {
    hype_nat_t nat;

    hype_nat_reset(0); /* must not fault */
    hype_nat_reset(&nat);
    CHECK_HEX("a null packet outbound is refused", -1,
              hype_nat_translate_outbound(&nat, 0, 64u, OUR_IP, 1ull));
    CHECK_HEX("a null packet inbound is refused", -1,
              hype_nat_translate_inbound(&nat, 0, 64u, 0, 1ull));
}

/* One address differing in a middle octet must not match. The four-octet compare short-circuits, so
 * a difference in the first octet exercises a different path from one in the last. */
static void test_address_compare_checks_every_octet(void) {
    hype_nat_t nat;
    uint8_t pkt[128];
    unsigned int len;
    uint16_t xlate;
    const uint8_t near_miss[4] = {142, 250, 187, 101}; /* REMOTE_IP with the last octet changed */

    hype_nat_reset(&nat);
    len = build(pkt, HYPE_IPV4_PROTO_UDP, GUEST_IP, REMOTE_IP, 5353u, 53u, 0, 0);
    (void)hype_nat_translate_outbound(&nat, pkt, len, OUR_IP, 1ull);
    xlate = get16(pkt + 20);

    len = build(pkt, HYPE_IPV4_PROTO_UDP, near_miss, OUR_IP, 53u, xlate, 0, 0);
    CHECK_HEX("a host differing only in the last octet is not a match", -1,
              hype_nat_translate_inbound(&nat, pkt, len, 0, 2ull));
}

int main(void) {
    test_checksum_known_value();
    test_udp_round_trip();
    test_icmp_echo_round_trip();
    test_unsolicited_inbound_is_dropped();
    test_reply_from_the_wrong_host_is_dropped();
    test_same_source_port_to_two_services();
    test_two_guests_same_port_do_not_collide();
    test_fragments_are_dropped_with_their_own_reason();
    test_malformed_packets_are_refused();
    test_unsupported_protocols_are_dropped();
    test_zero_udp_checksum_is_left_alone();
    test_the_table_fills_and_refuses_new_flows();
    test_idle_mappings_expire_by_protocol();
    test_a_backwards_clock_does_not_flush_everything();
    test_tcp_teardown_releases_the_mapping();
    test_inbound_rst_releases_the_mapping();
    test_the_same_flow_reuses_its_mapping();
    test_traffic_refreshes_the_timer();
    test_ip_options_shift_the_l4_header();
    test_headers_too_short_for_their_own_protocol();
    test_null_arguments();
    test_address_compare_checks_every_octet();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
