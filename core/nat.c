#include "nat.h"

#define IPV4_MIN_HDR 20u
#define ICMP_MIN_HDR 8u
#define UDP_HDR 8u
#define TCP_MIN_HDR 20u

static uint16_t rd16be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void wr16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static int ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void ip_copy(uint8_t dst[4], const uint8_t src[4]) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
}

void hype_nat_reset(hype_nat_t *nat) {
    unsigned int i;

    if (nat == 0) {
        return;
    }
    for (i = 0; i < HYPE_NAT_MAX_CONN; i++) {
        nat->conn[i].in_use = 0;
    }
    nat->next_port = 0;
    nat->out_translated = 0;
    nat->out_dropped_malformed = 0;
    nat->out_dropped_fragment = 0;
    nat->out_dropped_unsupported = 0;
    nat->out_dropped_no_slot = 0;
    nat->in_translated = 0;
    nat->in_dropped_no_mapping = 0;
    nat->in_dropped_malformed = 0;
    nat->conns_opened = 0;
    nat->conns_expired = 0;
}

uint16_t hype_inet_checksum(const uint8_t *data, unsigned int len) {
    uint32_t sum = 0;
    unsigned int i = 0;

    while (i + 1u < len) {
        sum += (uint32_t)rd16be(data + i);
        i += 2u;
    }
    if (i < len) {
        /* Odd trailing byte is padded on the RIGHT with zero, not the left. */
        sum += (uint32_t)((uint32_t)data[i] << 8);
    }
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

/*
 * What a parsed IPv4 packet looks like once it has been checked. Nothing here is taken on trust:
 * ihl, total_len and the L4 header all have to fit inside the frame the caller actually has.
 */
typedef struct {
    unsigned int ihl;      /* header length in bytes */
    unsigned int total;    /* total length from the header, validated against `len` */
    uint8_t proto;
    uint8_t *src;          /* into the packet */
    uint8_t *dst;
    uint8_t *l4;           /* start of the transport header */
    unsigned int l4_len;   /* how much of it is inside the frame */
} ipv4_view_t;

/*
 * Returns 0 on a usable packet, -1 on malformed, -2 on a fragment (which is a separate outcome
 * because it has its own counter and its own reason: there is nothing to translate in a non-first
 * fragment, and stitching them back together is reassembly, which is out of scope).
 */
static int parse_ipv4(uint8_t *pkt, unsigned int len, ipv4_view_t *v) {
    unsigned int ihl;
    unsigned int total;
    uint16_t frag;

    if (pkt == 0 || len < IPV4_MIN_HDR) {
        return -1;
    }
    if ((pkt[0] >> 4) != 4u) {
        return -1; /* not IPv4. IPv6 is not in this scope and must not be half-handled. */
    }
    ihl = (unsigned int)(pkt[0] & 0x0Fu) * 4u;
    if (ihl < IPV4_MIN_HDR || ihl > len) {
        return -1;
    }
    total = rd16be(pkt + 2);
    if (total < ihl || total > len) {
        /*
         * A total_length larger than the frame is the classic way to make a parser read past its
         * buffer. It is refused rather than clamped: a packet whose own header disagrees with its
         * size is not a packet hype should be repairing.
         */
        return -1;
    }
    frag = rd16be(pkt + 6);
    /* Bits 12:0 are the fragment offset; bit 13 is MF. Either a nonzero offset or MF set means this
     * is part of a fragmented datagram. */
    if ((frag & 0x1FFFu) != 0u || (frag & 0x2000u) != 0u) {
        return -2;
    }
    v->ihl = ihl;
    v->total = total;
    v->proto = pkt[9];
    v->src = pkt + 12;
    v->dst = pkt + 16;
    v->l4 = pkt + ihl;
    v->l4_len = total - ihl;
    return 0;
}

static void ip_header_checksum(uint8_t *pkt, unsigned int ihl) {
    /* The field must be zero while it is computed, then holds the result. */
    pkt[10] = 0;
    pkt[11] = 0;
    wr16be(pkt + 10, hype_inet_checksum(pkt, ihl));
}

/*
 * Recomputing an L4 checksum from scratch needs the whole payload, which the caller may not have if
 * the frame was truncated. Incremental fixup needs only what changed (RFC 1624): adjust the sum by
 * the difference between the old and new 16-bit words.
 *
 * `sum` is the field as it appears on the wire (already one's complement).
 */
static uint16_t checksum_adjust(uint16_t sum, uint16_t old_word, uint16_t new_word) {
    uint32_t s = (uint32_t)(~sum & 0xFFFFu);

    s += (uint32_t)(~old_word & 0xFFFFu);
    s += (uint32_t)new_word;
    while ((s >> 16) != 0u) {
        s = (s & 0xFFFFu) + (s >> 16);
    }
    return (uint16_t)(~s & 0xFFFFu);
}

/* Applies a 4-byte address change to an L4 checksum, two 16-bit words at a time. */
static uint16_t checksum_adjust_ip(uint16_t sum, const uint8_t old_ip[4], const uint8_t new_ip[4]) {
    uint16_t s = sum;
    s = checksum_adjust(s, rd16be(old_ip), rd16be(new_ip));
    s = checksum_adjust(s, rd16be(old_ip + 2), rd16be(new_ip + 2));
    return s;
}

/*
 * Where the L4 identifier lives, and whether the protocol has a checksum covering the IP addresses.
 * Returns 0 if this protocol is translatable, -1 otherwise.
 *
 * `id_off` is the offset within the L4 header of the field hype rewrites: the source port for
 * TCP/UDP, the echo identifier for ICMP. `csum_off` is the checksum field, or -1 for "none".
 */
static int l4_layout(uint8_t proto, const uint8_t *l4, unsigned int l4_len, unsigned int *id_off,
                     int *csum_off, int *pseudo_hdr) {
    if (proto == HYPE_IPV4_PROTO_UDP) {
        if (l4_len < UDP_HDR) {
            return -1;
        }
        *id_off = 0u;  /* source port */
        *csum_off = 6;
        *pseudo_hdr = 1; /* UDP's checksum covers the IP addresses */
        return 0;
    }
    if (proto == HYPE_IPV4_PROTO_TCP) {
        if (l4_len < TCP_MIN_HDR) {
            return -1;
        }
        *id_off = 0u;
        *csum_off = 16;
        *pseudo_hdr = 1;
        return 0;
    }
    if (proto == HYPE_IPV4_PROTO_ICMP) {
        if (l4_len < ICMP_MIN_HDR) {
            return -1;
        }
        /* Only echo request/reply are translatable: they carry an identifier hype can use as the
         * mapping key. An ICMP error (unreachable, TTL exceeded) is ABOUT another packet and would
         * need that packet's header read out of the payload to be routed back to the right guest --
         * a separate piece of work, and getting it wrong means delivering one guest's errors to
         * another. Dropped for now rather than guessed at. */
        if (l4[0] != HYPE_ICMP_TYPE_ECHO_REQUEST && l4[0] != HYPE_ICMP_TYPE_ECHO_REPLY) {
            return -1;
        }
        *id_off = 4u; /* the echo identifier */
        *csum_off = 2;
        *pseudo_hdr = 0; /* ICMP's checksum does NOT cover the IP header */
        return 0;
    }
    return -1;
}

static hype_nat_conn_t *find_out(hype_nat_t *nat, uint8_t proto, const uint8_t guest_ip[4],
                                 uint16_t guest_id, const uint8_t remote_ip[4],
                                 uint16_t remote_id) {
    unsigned int i;

    for (i = 0; i < HYPE_NAT_MAX_CONN; i++) {
        hype_nat_conn_t *c = &nat->conn[i];
        if (!c->in_use || c->proto != proto || c->guest_id != guest_id ||
            c->remote_id != remote_id) {
            continue;
        }
        if (ip_eq(c->guest_ip, guest_ip) && ip_eq(c->remote_ip, remote_ip)) {
            return c;
        }
    }
    return 0;
}

static int port_in_use(const hype_nat_t *nat, uint8_t proto, uint16_t port) {
    unsigned int i;

    for (i = 0; i < HYPE_NAT_MAX_CONN; i++) {
        if (nat->conn[i].in_use && nat->conn[i].proto == proto &&
            nat->conn[i].xlate_id == port) {
            return 1;
        }
    }
    return 0;
}

static hype_nat_conn_t *alloc_conn(hype_nat_t *nat, uint8_t proto) {
    unsigned int i;
    unsigned int tries;
    hype_nat_conn_t *slot = 0;

    for (i = 0; i < HYPE_NAT_MAX_CONN; i++) {
        if (!nat->conn[i].in_use) {
            slot = &nat->conn[i];
            break;
        }
    }
    if (slot == 0) {
        /*
         * The table is full. NOT reclaiming the oldest entry: that would silently break whichever
         * connection happened to be quietest, and the guest would see a working connection die for
         * no reason it could observe. Refusing the NEW flow is the honest failure -- the guest's
         * own stack retries, and the counter says why.
         */
        return 0;
    }
    /*
     * Find a free translated port. Scanning from a moving cursor rather than from the base means
     * successive flows do not all reuse the port a just-expired one had, which matters because a
     * remote end may still be sending to it.
     *
     * The nesting reads worse than it is: HYPE_NAT_PORT_COUNT iterations each doing an
     * O(HYPE_NAT_MAX_CONN) scan. But at most HYPE_NAT_MAX_CONN ports can be in use, and the slot
     * check above already returned if none were free -- so a free port is found within
     * HYPE_NAT_MAX_CONN + 1 candidates. The bound is ~257 * 256 comparisons in the pathological
     * case, once, on connection SETUP; the steady-state cost is one candidate. The loop still runs
     * to PORT_COUNT rather than to that tighter bound because the bound depends on an invariant two
     * functions apart, and a loop that terminates for its own visible reason is worth more here
     * than the iterations it saves.
     */
    for (tries = 0; tries < HYPE_NAT_PORT_COUNT; tries++) {
        uint16_t candidate = (uint16_t)(HYPE_NAT_PORT_BASE +
                                        ((unsigned int)nat->next_port % HYPE_NAT_PORT_COUNT));
        nat->next_port = (uint16_t)((nat->next_port + 1u) % HYPE_NAT_PORT_COUNT);
        if (!port_in_use(nat, proto, candidate)) {
            slot->xlate_id = candidate;
            return slot;
        }
    }
    return 0; /* every port in the range is mapped for this protocol */
}

static unsigned long long idle_limit(uint8_t proto) {
    if (proto == HYPE_IPV4_PROTO_TCP) {
        return HYPE_NAT_IDLE_TICKS_TCP;
    }
    if (proto == HYPE_IPV4_PROTO_ICMP) {
        return HYPE_NAT_IDLE_TICKS_ICMP;
    }
    return HYPE_NAT_IDLE_TICKS_UDP;
}

/* TCP flag bits at offset 13 of the header. */
#define TCP_FIN 0x01u
#define TCP_RST 0x04u

int hype_nat_translate_outbound(hype_nat_t *nat, uint8_t *pkt, unsigned int len,
                                const uint8_t our_ip[4], unsigned long long tick) {
    ipv4_view_t v;
    unsigned int id_off;
    int csum_off;
    int pseudo;
    hype_nat_conn_t *c;
    uint8_t old_src[4];
    uint16_t guest_id;
    uint16_t remote_id;
    int rc;

    if (nat == 0 || our_ip == 0) {
        return -1;
    }
    rc = parse_ipv4(pkt, len, &v);
    if (rc == -2) {
        nat->out_dropped_fragment++;
        return -1;
    }
    if (rc != 0) {
        nat->out_dropped_malformed++;
        return -1;
    }
    if (l4_layout(v.proto, v.l4, v.l4_len, &id_off, &csum_off, &pseudo) != 0) {
        nat->out_dropped_unsupported++;
        return -1;
    }

    guest_id = rd16be(v.l4 + id_off);
    /* The remote identifier is the DESTINATION port for TCP/UDP, and nothing for ICMP. Including it
     * in the key is what lets one guest hold separate mappings to two different services on the
     * same host from the same source port. */
    remote_id = (v.proto == HYPE_IPV4_PROTO_ICMP) ? 0u : rd16be(v.l4 + 2u);

    c = find_out(nat, v.proto, v.src, guest_id, v.dst, remote_id);
    if (c == 0) {
        c = alloc_conn(nat, v.proto);
        if (c == 0) {
            nat->out_dropped_no_slot++;
            return -1;
        }
        c->in_use = 1;
        c->proto = v.proto;
        ip_copy(c->guest_ip, v.src);
        ip_copy(c->remote_ip, v.dst);
        c->guest_id = guest_id;
        c->remote_id = remote_id;
        c->fin_from_guest = 0;
        c->fin_from_remote = 0;
        nat->conns_opened++;
    }
    c->last_tick = tick;

    if (v.proto == HYPE_IPV4_PROTO_TCP) {
        uint8_t flags = v.l4[13];
        if ((flags & TCP_RST) != 0u) {
            /* A reset ends the connection now. The mapping is released after this packet is sent,
             * not before -- the packet still has to be translated to reach the far end. */
            c->fin_from_guest = 1;
            c->fin_from_remote = 1;
        } else if ((flags & TCP_FIN) != 0u) {
            c->fin_from_guest = 1;
        }
    }

    ip_copy(old_src, v.src);
    ip_copy(v.src, our_ip);
    ip_header_checksum(pkt, v.ihl);

    /* The L4 checksum: the identifier changed, and for TCP/UDP the source address is in the
     * pseudo-header too. ICMP's checksum covers neither IP address, so only the identifier. */
    {
        uint16_t sum = rd16be(v.l4 + csum_off);
        /*
         * A UDP checksum of 0 means "not computed" and must stay 0 -- rewriting it to a real value
         * is legal but rewriting it to a nonzero value that is WRONG is not, and adjusting from a
         * zero that never was a checksum produces exactly that.
         */
        int skip = (v.proto == HYPE_IPV4_PROTO_UDP && sum == 0u);
        if (!skip) {
            if (pseudo) {
                sum = checksum_adjust_ip(sum, old_src, our_ip);
            }
            sum = checksum_adjust(sum, guest_id, c->xlate_id);
            wr16be(v.l4 + csum_off, sum);
        }
    }
    wr16be(v.l4 + id_off, c->xlate_id);

    if (v.proto == HYPE_IPV4_PROTO_TCP && c->fin_from_guest && c->fin_from_remote) {
        c->in_use = 0;
        nat->conns_expired++;
    }
    nat->out_translated++;
    return 0;
}

int hype_nat_translate_inbound(hype_nat_t *nat, uint8_t *pkt, unsigned int len,
                               uint8_t out_guest_ip[4], unsigned long long tick) {
    ipv4_view_t v;
    unsigned int id_off;
    int csum_off;
    int pseudo;
    unsigned int i;
    hype_nat_conn_t *c = 0;
    uint16_t xlate_id;
    uint16_t remote_id;
    uint8_t old_dst[4];
    int rc;

    if (nat == 0) {
        return -1;
    }
    rc = parse_ipv4(pkt, len, &v);
    if (rc != 0) {
        /* A fragment arriving from the wire is counted as malformed rather than getting its own
         * counter: outbound fragments say something about the guest, inbound ones say something
         * about the network, and neither is translatable. */
        nat->in_dropped_malformed++;
        return -1;
    }
    if (l4_layout(v.proto, v.l4, v.l4_len, &id_off, &csum_off, &pseudo) != 0) {
        nat->in_dropped_malformed++;
        return -1;
    }

    /* The field hype substituted is now the DESTINATION port (or the echo id of a reply). */
    xlate_id = (v.proto == HYPE_IPV4_PROTO_ICMP) ? rd16be(v.l4 + 4u) : rd16be(v.l4 + 2u);
    remote_id = (v.proto == HYPE_IPV4_PROTO_ICMP) ? 0u : rd16be(v.l4 + 0u);

    for (i = 0; i < HYPE_NAT_MAX_CONN; i++) {
        hype_nat_conn_t *k = &nat->conn[i];
        if (!k->in_use || k->proto != v.proto || k->xlate_id != xlate_id) {
            continue;
        }
        /*
         * The SOURCE must be the host this mapping was created towards. Without this check any host
         * on the internet could reach a guest by guessing a translated port, which would make
         * "outbound plus established return" mean "outbound plus whatever anyone sends".
         */
        if (!ip_eq(k->remote_ip, v.src)) {
            continue;
        }
        if (k->remote_id != remote_id) {
            continue;
        }
        c = k;
        break;
    }
    if (c == 0) {
        /* Unsolicited. The ordinary case for background internet noise, so it is counted and
         * dropped rather than reported as a fault. */
        nat->in_dropped_no_mapping++;
        return -1;
    }
    c->last_tick = tick;

    if (v.proto == HYPE_IPV4_PROTO_TCP) {
        uint8_t flags = v.l4[13];
        if ((flags & TCP_RST) != 0u) {
            c->fin_from_guest = 1;
            c->fin_from_remote = 1;
        } else if ((flags & TCP_FIN) != 0u) {
            c->fin_from_remote = 1;
        }
    }

    ip_copy(old_dst, v.dst);
    ip_copy(v.dst, c->guest_ip);
    ip_header_checksum(pkt, v.ihl);

    {
        uint16_t sum = rd16be(v.l4 + csum_off);
        int skip = (v.proto == HYPE_IPV4_PROTO_UDP && sum == 0u);
        if (!skip) {
            if (pseudo) {
                sum = checksum_adjust_ip(sum, old_dst, c->guest_ip);
            }
            sum = checksum_adjust(sum, xlate_id, c->guest_id);
            wr16be(v.l4 + csum_off, sum);
        }
    }
    wr16be(v.l4 + ((v.proto == HYPE_IPV4_PROTO_ICMP) ? 4u : 2u), c->guest_id);

    if (out_guest_ip != 0) {
        ip_copy(out_guest_ip, c->guest_ip);
    }
    if (v.proto == HYPE_IPV4_PROTO_TCP && c->fin_from_guest && c->fin_from_remote) {
        c->in_use = 0;
        nat->conns_expired++;
    }
    nat->in_translated++;
    return 0;
}

unsigned int hype_nat_expire(hype_nat_t *nat, unsigned long long tick) {
    unsigned int i;
    unsigned int dropped = 0;

    if (nat == 0) {
        return 0;
    }
    for (i = 0; i < HYPE_NAT_MAX_CONN; i++) {
        hype_nat_conn_t *c = &nat->conn[i];
        if (!c->in_use) {
            continue;
        }
        /*
         * `tick` going BACKWARDS is treated as "not idle" rather than as a huge age. The caller's
         * clock is a counter hype maintains, and a wrapped or reset one must not flush every
         * mapping at once -- that would drop every live connection on every guest simultaneously,
         * which is far worse than holding a stale entry for one extra sweep.
         */
        if (tick < c->last_tick) {
            c->last_tick = tick;
            continue;
        }
        if (tick - c->last_tick >= idle_limit(c->proto)) {
            c->in_use = 0;
            nat->conns_expired++;
            dropped++;
        }
    }
    return dropped;
}

unsigned int hype_nat_active(const hype_nat_t *nat) {
    unsigned int i;
    unsigned int n = 0;

    if (nat == 0) {
        return 0;
    }
    for (i = 0; i < HYPE_NAT_MAX_CONN; i++) {
        if (nat->conn[i].in_use) {
            n++;
        }
    }
    return n;
}
