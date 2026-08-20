/*
 * NET-1 (#80): Ethernet + ARP frame construction and parsing, pure.
 *
 * Its own module rather than part of the e1000 driver, because ARP is not device-specific: #83's
 * NAT needs to answer ARP for the addresses it represents, and #84/#85 need to NOT answer it
 * across an isolation boundary. A driver-local helper would have to be moved the moment the second
 * caller appeared.
 *
 * Pure: no allocation, no I/O, no globals, and every function bounds-checks its buffer. These
 * frames come off a wire hype does not control, so parsing is a trust boundary (AGENTS.md: validate
 * at boundaries) -- a short or malformed frame must be refused, never read past.
 *
 * Wire layouts from RFC 826 (ARP) and IEEE 802.3. All multi-byte fields are BIG-endian on the
 * wire, which is the opposite of the host, and getting that wrong produces frames a switch
 * silently discards.
 */
#ifndef HYPE_CORE_ARP_H
#define HYPE_CORE_ARP_H

#include <stdint.h>

#define HYPE_ETH_ALEN 6u
#define HYPE_ETH_HDR_LEN 14u
#define HYPE_ETHERTYPE_ARP 0x0806u
#define HYPE_ETHERTYPE_IPV4 0x0800u

#define HYPE_ARP_HTYPE_ETHERNET 1u
#define HYPE_ARP_OP_REQUEST 1u
#define HYPE_ARP_OP_REPLY 2u
#define HYPE_ARP_PACKET_LEN 28u
/* An ARP frame is 14 + 28 = 42 bytes; the wire minimum is 60, and the NIC pads (TCTL.PSP). */
#define HYPE_ARP_FRAME_LEN (HYPE_ETH_HDR_LEN + HYPE_ARP_PACKET_LEN)

typedef struct {
    uint16_t op;
    uint8_t sender_mac[HYPE_ETH_ALEN];
    uint8_t sender_ip[4];
    uint8_t target_mac[HYPE_ETH_ALEN];
    uint8_t target_ip[4];
} hype_arp_t;

/*
 * Build an ARP request into `out` (needs HYPE_ARP_FRAME_LEN bytes). Destination is the broadcast
 * address, which is what makes a request a request. Returns the frame length, or 0 if the buffer
 * is too small -- never a partial frame.
 */
unsigned int hype_arp_build_request(uint8_t *out, unsigned int cap, const uint8_t sender_mac[6],
                                   const uint8_t sender_ip[4], const uint8_t target_ip[4]);

/*
 * Build an ARP reply -- what hype sends when it answers on a guest's behalf (#83). Same buffer
 * rules.
 */
unsigned int hype_arp_build_reply(uint8_t *out, unsigned int cap, const uint8_t sender_mac[6],
                                 const uint8_t sender_ip[4], const uint8_t target_mac[6],
                                 const uint8_t target_ip[4]);

/*
 * Parse an Ethernet frame as ARP. Returns 1 and fills *out on success, 0 otherwise.
 *
 * Refuses anything that is not exactly what it claims to be: too short, not EtherType 0x0806, not
 * Ethernet/IPv4 hardware and protocol types, or the wrong address lengths. A frame arriving from
 * the network is untrusted input, so every field that indexes into it is checked before use.
 */
int hype_arp_parse(const uint8_t *frame, unsigned int len, hype_arp_t *out);

/* 1 when the two addresses are equal. */
int hype_arp_mac_eq(const uint8_t a[6], const uint8_t b[6]);
int hype_arp_ip_eq(const uint8_t a[4], const uint8_t b[4]);

#endif /* HYPE_CORE_ARP_H */
