#include <stdio.h>
#include "../arp.h"

static int failures;

#define CHECK(desc, cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", (desc));                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_INT(desc, expected, actual)                                                          \
    do {                                                                                           \
        if ((long long)(expected) != (long long)(actual)) {                                        \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), (long long)(expected),            \
                   (long long)(actual));                                                           \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* QEMU's e1000 MAC and its user-networking addresses -- real values, so a byte-order mistake shows
 * up as a frame a switch would drop rather than passing against an invented fixture. */
static const uint8_t OUR_MAC[6] = {0x52u, 0x54u, 0x00u, 0x12u, 0x34u, 0x56u};
static const uint8_t OUR_IP[4] = {10u, 0u, 2u, 15u};
static const uint8_t GW_IP[4] = {10u, 0u, 2u, 2u};
static const uint8_t GW_MAC[6] = {0x52u, 0x55u, 0x0au, 0x00u, 0x02u, 0x02u};

static void test_build_request(void) {
    uint8_t f[64];
    unsigned int n = hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);

    CHECK_INT("a request is 42 bytes", 42, n);
    /* Destination MUST be broadcast -- that is what makes it a request rather than a probe nobody
     * hears. */
    CHECK("destination is broadcast", f[0] == 0xFFu && f[5] == 0xFFu);
    CHECK("source is our MAC", f[6] == 0x52u && f[11] == 0x56u);
    /* EtherType is BIG-endian on the wire. Getting this backwards produces 0x0608, which a switch
     * discards silently -- the failure mode with no symptom. */
    CHECK_INT("ethertype high byte", 0x08, f[12]);
    CHECK_INT("ethertype low byte", 0x06, f[13]);
    CHECK_INT("htype is ethernet", 1, f[15]);
    CHECK_INT("ptype is IPv4 high", 0x08, f[16]);
    CHECK_INT("hlen is 6", 6, f[18]);
    CHECK_INT("plen is 4", 4, f[19]);
    CHECK_INT("opcode is request", 1, f[21]);
    CHECK("sender IP is ours", f[28] == 10u && f[31] == 15u);
    /* RFC 826: the target hardware address in a REQUEST is zeros, not broadcast -- some stacks
     * treat broadcast there as malformed and drop it. */
    CHECK("target MAC is zeros", f[32] == 0u && f[37] == 0u);
    CHECK("target IP is the gateway", f[38] == 10u && f[41] == 2u);
}

static void test_build_reply(void) {
    uint8_t f[64];
    unsigned int n = hype_arp_build_reply(f, sizeof(f), OUR_MAC, OUR_IP, GW_MAC, GW_IP);

    CHECK_INT("a reply is 42 bytes", 42, n);
    CHECK("a reply is unicast to the target", f[0] == 0x52u && f[1] == 0x55u);
    CHECK_INT("opcode is reply", 2, f[21]);
    CHECK("the target MAC is filled in", f[32] == 0x52u && f[37] == 0x02u);

    /* A reply with no destination is not a reply. */
    CHECK_INT("a reply needs a target MAC", 0,
              hype_arp_build_reply(f, sizeof(f), OUR_MAC, OUR_IP, 0, GW_IP));
}

/* A buffer too small must yield NOTHING, not a truncated frame. */
static void test_build_refuses_short_buffer(void) {
    uint8_t f[64];
    CHECK_INT("41 bytes is refused", 0,
              hype_arp_build_request(f, 41u, OUR_MAC, OUR_IP, GW_IP));
    CHECK_INT("a null buffer is refused", 0,
              hype_arp_build_request(0, 64u, OUR_MAC, OUR_IP, GW_IP));
    /* Each required address, individually. A caller that passes one null has a bug; building a
     * frame with zeros where an address belongs would put it on the wire instead of reporting it. */
    CHECK_INT("a null sender MAC is refused", 0,
              hype_arp_build_request(f, 64u, 0, OUR_IP, GW_IP));
    CHECK_INT("a null sender IP is refused", 0,
              hype_arp_build_request(f, 64u, OUR_MAC, 0, GW_IP));
    CHECK_INT("a null target IP is refused", 0,
              hype_arp_build_request(f, 64u, OUR_MAC, OUR_IP, 0));
}

/* Round trip: what we build must be what we parse. */
static void test_round_trip(void) {
    uint8_t f[64];
    hype_arp_t a;

    (void)hype_arp_build_reply(f, sizeof(f), GW_MAC, GW_IP, OUR_MAC, OUR_IP);
    CHECK_INT("a built reply parses", 1, hype_arp_parse(f, 42u, &a));
    CHECK_INT("op survives", HYPE_ARP_OP_REPLY, a.op);
    CHECK("sender MAC survives", hype_arp_mac_eq(a.sender_mac, GW_MAC));
    CHECK("sender IP survives", hype_arp_ip_eq(a.sender_ip, GW_IP));
    CHECK("target MAC survives", hype_arp_mac_eq(a.target_mac, OUR_MAC));
    CHECK("target IP survives", hype_arp_ip_eq(a.target_ip, OUR_IP));
}

/*
 * Parsing is a TRUST BOUNDARY -- these frames come off a wire hype does not control. Every field
 * that indexes into the buffer is checked before use, and a frame claiming a hardware length other
 * than 6 would move every offset below it.
 */
static void test_parse_refuses_malformed(void) {
    uint8_t f[64];
    hype_arp_t a;
    unsigned int i;

    (void)hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);
    CHECK_INT("a good frame parses", 1, hype_arp_parse(f, 42u, &a));

    /* One byte short of a complete ARP frame. */
    CHECK_INT("41 bytes is refused", 0, hype_arp_parse(f, 41u, &a));
    CHECK_INT("zero length is refused", 0, hype_arp_parse(f, 0u, &a));
    CHECK_INT("a null frame is refused", 0, hype_arp_parse(0, 42u, &a));
    CHECK_INT("a null output is refused", 0, hype_arp_parse(f, 42u, 0));

    /* Not ARP at all. */
    (void)hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);
    f[12] = 0x08u; f[13] = 0x00u; /* IPv4 */
    CHECK_INT("a non-ARP ethertype is refused", 0, hype_arp_parse(f, 42u, &a));

    /* Lying about the hardware address length -- the dangerous one. */
    (void)hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);
    f[18] = 8u;
    CHECK_INT("a wrong hlen is refused", 0, hype_arp_parse(f, 42u, &a));

    (void)hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);
    f[19] = 16u;
    CHECK_INT("a wrong plen is refused", 0, hype_arp_parse(f, 42u, &a));

    /* A hardware type that is not Ethernet. */
    (void)hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);
    f[15] = 6u;
    CHECK_INT("a non-ethernet htype is refused", 0, hype_arp_parse(f, 42u, &a));

    /* A protocol type that is not IPv4. */
    (void)hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);
    f[17] = 0x06u;
    CHECK_INT("a non-IPv4 ptype is refused", 0, hype_arp_parse(f, 42u, &a));

    /* An opcode this stack does not handle (RARP request is 3). */
    (void)hype_arp_build_request(f, sizeof(f), OUR_MAC, OUR_IP, GW_IP);
    f[21] = 3u;
    CHECK_INT("an unhandled opcode is refused", 0, hype_arp_parse(f, 42u, &a));

    /* Every single-byte corruption of the header must either parse or refuse -- never crash. This
     * is the cheap fuzz that catches a missing bound. */
    for (i = 0; i < 42u; i++) {
        uint8_t g[64];
        unsigned int k;
        for (k = 0; k < 64u; k++) g[k] = 0u;
        (void)hype_arp_build_request(g, sizeof(g), OUR_MAC, OUR_IP, GW_IP);
        g[i] = (uint8_t)~g[i];
        (void)hype_arp_parse(g, 42u, &a); /* must return, whatever it decides */
    }
}

static void test_compare_helpers(void) {
    CHECK("equal MACs compare equal", hype_arp_mac_eq(OUR_MAC, OUR_MAC));
    CHECK("different MACs do not", !hype_arp_mac_eq(OUR_MAC, GW_MAC));
    CHECK("a null MAC never matches", !hype_arp_mac_eq(OUR_MAC, 0));
    CHECK("equal IPs compare equal", hype_arp_ip_eq(OUR_IP, OUR_IP));
    CHECK("different IPs do not", !hype_arp_ip_eq(OUR_IP, GW_IP));
    CHECK("a null IP never matches", !hype_arp_ip_eq(0, GW_IP));
    CHECK("a null second MAC never matches", !hype_arp_mac_eq(0, GW_MAC));
    CHECK("a null second IP never matches", !hype_arp_ip_eq(OUR_IP, 0));
}

int main(void) {
    test_build_request();
    test_build_reply();
    test_build_refuses_short_buffer();
    test_round_trip();
    test_parse_refuses_malformed();
    test_compare_helpers();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
