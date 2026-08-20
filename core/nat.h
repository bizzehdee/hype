#ifndef HYPE_CORE_NAT_H
#define HYPE_CORE_NAT_H

#include <stdint.h>

/*
 * NET-4 (#83): host-level NAPT, guest -> WAN plus established return traffic.
 *
 * WHAT THIS IS AND IS NOT (plan.md 6e, decision 36). hype is a FORWARDING PLANE, never an
 * endpoint. This module rewrites addresses and ports in packets passing between a guest and the
 * wire. It owns no socket, drives no TCP state machine, and is never the address a packet is sent
 * to. There is no listening anything here.
 *
 * Consequences of that rule which show up directly in this file:
 *
 *   - TCP is tracked, not terminated. hype watches SYN/FIN/RST to decide when a mapping is live,
 *     because a NAT has to; it never generates a segment, retransmits, or reassembles.
 *   - FRAGMENTS ARE DROPPED, not reassembled. A non-first fragment has no L4 header, so there is
 *     nothing to translate and no way to know which mapping it belongs to without holding state
 *     across packets -- which is reassembly. Dropping is the stated scope.
 *   - INBOUND IS DEFAULT-DENY. A packet from the wire is translated only if it matches an existing
 *     mapping this guest created. Nothing else reaches a guest, which is what makes port forwarding
 *     a separate, opt-in thing (NET-8) rather than something that falls out of NAT by accident.
 *
 * EVERY BYTE HERE ARRIVES FROM SOMEWHERE UNTRUSTED -- a guest on one side, the network on the
 * other -- so this is a validating parser, not a struct cast. Header lengths, total lengths and
 * offsets are all checked against the actual frame before anything is read or written.
 */

#define HYPE_IPV4_PROTO_ICMP 1u
#define HYPE_IPV4_PROTO_TCP 6u
#define HYPE_IPV4_PROTO_UDP 17u

#define HYPE_ICMP_TYPE_ECHO_REPLY 0u
#define HYPE_ICMP_TYPE_ECHO_REQUEST 8u

/*
 * Mapping capacity, per VM.
 *
 * PER VM, not per host, and that is the isolation property rather than a sizing convenience: one
 * guest opening connections must not be able to exhaust another guest's ability to open any. A
 * shared table would make that a supported denial of service between VMs that are meant to be
 * isolated (6e), and it would be invisible -- the victim would simply see connections fail.
 */
#define HYPE_NAT_MAX_CONN 256u

/*
 * The port/id range hype substitutes from. Deliberately high and away from the registered range so
 * a translated source port cannot be mistaken for a service, and sized so the cursor wraps long
 * before it collides with anything a host stack would pick for itself.
 */
#define HYPE_NAT_PORT_BASE 49152u
#define HYPE_NAT_PORT_COUNT 16384u

/* How long a mapping survives without traffic, in whatever tick unit the caller passes. Separate
 * for TCP because a live TCP connection can legitimately sit idle far longer than a UDP flow or an
 * ICMP echo, and expiring it early would drop an established connection mid-use. */
#define HYPE_NAT_IDLE_TICKS_UDP 120u
#define HYPE_NAT_IDLE_TICKS_ICMP 30u
#define HYPE_NAT_IDLE_TICKS_TCP 3600u

typedef struct {
    unsigned int in_use;
    uint8_t proto;
    uint8_t guest_ip[4];
    uint8_t remote_ip[4];
    uint16_t guest_id;  /* the guest's own source port, or its ICMP echo identifier */
    uint16_t remote_id; /* the remote port; 0 for ICMP, which has no ports */
    uint16_t xlate_id;  /* what hype substituted, and what the far end will reply to */
    unsigned long long last_tick;
    /* #83: TCP teardown seen from both directions. A mapping is dropped once both sides have
     * finished, or immediately on RST -- keeping it alive after that would hold a translated port
     * hostage for the full TCP idle timeout. */
    unsigned int fin_from_guest;
    unsigned int fin_from_remote;
} hype_nat_conn_t;

typedef struct {
    hype_nat_conn_t conn[HYPE_NAT_MAX_CONN];
    /* Allocation cursor, so successive flows do not all get the same port after an expiry. */
    uint16_t next_port;
    unsigned long long out_translated;
    unsigned long long out_dropped_malformed;
    unsigned long long out_dropped_fragment;
    unsigned long long out_dropped_unsupported;
    unsigned long long out_dropped_no_slot;
    unsigned long long in_translated;
    unsigned long long in_dropped_no_mapping;
    unsigned long long in_dropped_malformed;
    unsigned long long conns_opened;
    unsigned long long conns_expired;
} hype_nat_t;

void hype_nat_reset(hype_nat_t *nat);

/* The standard 16-bit one's-complement checksum over `len` bytes. Exposed because both the IPv4
 * header and ICMP use it directly and the tests check it against known values. */
uint16_t hype_inet_checksum(const uint8_t *data, unsigned int len);

/*
 * Rewrites an outbound IPv4 packet in place so it appears to come from `our_ip`, allocating or
 * reusing a mapping. `pkt` points at the IPv4 header (the Ethernet header is the caller's).
 *
 * Returns 0 when the packet was translated and may be sent, -1 when it must be dropped. Every
 * drop reason has its own counter, because "NAT dropped it" is not a diagnosis.
 */
int hype_nat_translate_outbound(hype_nat_t *nat, uint8_t *pkt, unsigned int len,
                                const uint8_t our_ip[4], unsigned long long tick);

/*
 * Rewrites an inbound IPv4 packet in place so it is addressed to the guest that owns the mapping,
 * and reports which guest that is via `out_guest_ip`.
 *
 * Returns 0 when translated, -1 when there is no mapping -- which is the ordinary case for
 * unsolicited traffic and is not an error. The caller drops it.
 */
int hype_nat_translate_inbound(hype_nat_t *nat, uint8_t *pkt, unsigned int len,
                               uint8_t out_guest_ip[4], unsigned long long tick);

/* Drops mappings idle past their protocol's timeout. Called from the dispatch loop; separate from
 * translation so a busy path never pays for a table sweep. Returns how many were dropped. */
unsigned int hype_nat_expire(hype_nat_t *nat, unsigned long long tick);

/* How many mappings are live, for the diagnostic line. */
unsigned int hype_nat_active(const hype_nat_t *nat);

#endif /* HYPE_CORE_NAT_H */
