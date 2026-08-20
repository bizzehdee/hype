/*
 * NET-3 (#82): the e1000 frontend, proven the same way and against the same network as virtio-net.
 *
 * This is netdns's twin. It resolves www.google.com through hype's NAPT, over the OTHER guest NIC.
 * Sending the same traffic through the same forwarding plane over a different device is what proves
 * the plane does not care which frontend a guest has -- which is the whole claim
 * hype_guest_nic_ops_t makes.
 *
 * WHAT ONLY THIS TEST CAN FIND, because virtio's shape hides it:
 *
 *   - the TAIL WRITE IS THE DOORBELL. virtio has a notify register; the e1000 has none, so hype has
 *     to treat a write to TDT as the kick. Miss that and the NIC accepts descriptors and never
 *     transmits, while the guest's own counters show frames queued -- which reads like hype losing
 *     them.
 *   - the EMPTINESS TEST IS INVERTED. HEAD chases TAIL here, so equal means the transmit ring is
 *     empty and, on receive, that no buffer is posted. Reading one as the other gives a NIC that
 *     either never sends or claims a buffer it does not have.
 *   - the MAC comes from RAL/RAH with an AV bit, or from an EEPROM, rather than from a device-config
 *     region a driver was pointed at.
 *
 * The VM must have `os_hint = windows`, which is what selects this frontend -- the same derivation
 * §6a uses for storage, one layer up. No config key chooses it: each OS has exactly one sensible
 * answer, so a key's only correct value would be the one hype can work out.
 */
#include "micro_pci.h"

#define NAME "e1000dns"

/* The FRAME BUILDERS, shared with the virtio tests. Sending byte-identical traffic over both NICs is
 * the point of this test, so the builders must be the same ones -- not a copy. */
#include "micro_netframes.h"
#include "micro_e1000.h"

static const uint8_t DNS_IP[4] = {8, 8, 8, 8};

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    unsigned int i;
    uint8_t frame[600];
    uint8_t gw_mac[6];
    int gw_known = 0;
    int answered = 0;
    unsigned long long spins;
    unsigned int dns_sent = 0;

    (void)zero_page_gpa;
    micro_puts("micro/" NAME ": start\n");

    if (e1000_up(NAME) != 0) {
        micro_halt();
    }
    micro_puts("micro/" NAME ": nic up, mac ");
    for (i = 0; i < 6u; i++) {
        micro_put_hex(g_e1000_mac[i]);
        micro_puts(i == 5u ? "\n" : ":");
    }

    /*
     * The shared builders take the MAC from micro_vnet.h's `g_mac`, so it is filled in from what the
     * e1000 reported. One set of builders, two drivers -- if these two diverged, the traffic would
     * differ and the comparison this test makes would be worthless.
     */
    for (i = 0; i < 6u; i++) {
        g_mac[i] = g_e1000_mac[i];
    }

    e1000_send(frame, build_arp(frame));

    for (spins = 0; spins < 60000000ull; spins++) {
        unsigned int rlen = 0;
        const uint8_t *rb;
        uint16_t et;

        __asm__ volatile("pause" ::: "memory");

        if (gw_known && !answered && dns_sent < 20u && (spins % 600000ull) == 0ull) {
            e1000_send(frame, build_dns(frame, gw_mac, DNS_IP, 0x6161u, 41000u));
            dns_sent++;
        }

        rb = e1000_recv(&rlen);
        if (rb == 0 || rlen < 14u) {
            continue;
        }
        et = be16(rb + 12);

        if (et == 0x0806u) {
            if (be16(rb + 20) == 2u && !gw_known) {
                for (i = 0; i < 6u; i++) {
                    gw_mac[i] = rb[22 + i];
                }
                gw_known = 1;
                micro_puts("micro/" NAME ": hype answered the ARP\n");
            }
            continue;
        }
        if (et != 0x0800u || rb[14 + 9] != 17u) {
            continue;
        }
        {
            unsigned int ihl = (unsigned int)(rb[14] & 0x0Fu) * 4u;
            const uint8_t *udp = rb + 14u + ihl;
            if (be16(udp) == 53u && be16(udp + 8) == 0x6161u) {
                unsigned int ancount = be16(udp + 8 + 6);
                micro_puts("micro/" NAME ": DNS reply, rcode=");
                micro_put_uint((unsigned)(udp[8 + 3] & 0x0Fu));
                micro_puts(" answers=");
                micro_put_uint(ancount);
                micro_puts("\n");
                if ((udp[8 + 3] & 0x0Fu) != 0u || ancount == 0u) {
                    micro_fail(NAME, "the round trip worked -- so hype's e1000 frontend and its "
                                     "forwarding plane are fine -- but the name did not resolve");
                    micro_halt();
                }
                answered = 1;
                break;
            }
        }
    }

    if (!gw_known) {
        micro_fail(NAME, "no ARP reply. The e1000 transmit path is the suspect: the TAIL write is "
                         "the doorbell on this device, so if hype does not drain on a TDT write "
                         "nothing is ever sent. Check `fw-1 NAT vm0: out=` -- 0 means hype never "
                         "saw the frame");
        micro_halt();
    }
    if (!answered) {
        micro_fail(NAME, "the ARP was answered, so transmit works and hype learned this guest -- but "
                         "no DNS reply arrived. That points at the RECEIVE path: on this device "
                         "RDH == RDT means no buffer is posted, the inverse of virtio's test");
        micro_halt();
    }

    micro_puts("micro/" NAME ": www.google.com resolved through hype's NAT over an e1000 -- the "
               "forwarding plane does not care which NIC the guest has.\n");
    micro_pass(NAME);
    micro_halt();
}
