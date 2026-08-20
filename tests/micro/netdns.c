/*
 * NET-4 (#83): the whole forwarding plane, end to end, from inside a guest.
 *
 * This is the test that says whether hype is a network. `virtionet` proves the DEVICE -- discovery,
 * negotiation, both rings, a descriptor completed. It deliberately does not claim a packet went
 * anywhere. This one sends a real DNS query for www.google.com and requires a real answer back, so
 * passing means every layer worked:
 *
 *   guest virtio-net TX ring
 *     -> hype's ring walker (core/virtio_net_ring.c)
 *       -> the forwarding plane: proxy ARP, address learning, NAPT source rewrite, checksum fixup
 *         -> the host e1000 driver (core/e1000_hw.c)
 *           -> the physical network (QEMU user-mode networking in the rig)
 *             -> a real DNS server
 *               -> back through NAPT's reverse translation, keyed on the mapping this guest made
 *                 -> an Ethernet header addressed to the MAC hype LEARNED from this guest
 *                   -> the guest's virtio-net RX ring
 *
 * WHY DNS AND NOT PING. An ICMP echo would be the more direct reading of "can this guest reach the
 * internet", and NAPT handles echo identifiers (core/tests/test_nat.c proves the translation). But
 * QEMU's user-mode networking can only forward ICMP if the host permits unprivileged ICMP sockets,
 * which is a property of the DEVELOPER'S MACHINE rather than of hype -- a test that fails on a
 * host's sysctl tells nobody anything. A DNS query is UDP, which slirp always forwards, so a
 * failure here is hype's.
 *
 * The query is for www.google.com specifically because that is the name in the goal this work
 * serves; resolving it is the half of "ping www.google.com" that hype is responsible for.
 *
 * ADDRESSES. The guest gives itself 192.168.77.2/24 with a gateway of 192.168.77.1, and hype has
 * never been told about that subnet. It does not need to be: hype answers every ARP a guest sends
 * with its own MAC (proxy ARP), so whatever the guest is configured with resolves to hype, and hype
 * learns the guest's address from the ARP it answered. If this test starts failing after a change
 * to that, the interesting question is whether hype answered the ARP -- the log line is
 * `arp_answered=` in the per-VM NAT diagnostic.
 */
#include "micro_pci.h"

#define NAME "netdns"

#include "micro_vnet.h"

/* Google Public DNS. A fixed, globally-routable resolver, so the test does not depend on whatever
 * the developer's own resolver happens to be. */
static const uint8_t DNS_IP[4] = {8, 8, 8, 8};

void micro_main(void) {
    unsigned int i;
    uint8_t frame[600];
    uint8_t gw_mac[6];
    int gw_known = 0;
    volatile uint16_t *ru = (volatile uint16_t *)(uintptr_t)RX_USED_GPA;
    uint16_t seen_used = 0;
    unsigned long long spins;
    int answered = 0;

    micro_puts("micro/" NAME ": start\n");
    if (find_and_bring_up() != 0) {
        micro_halt();
    }
    micro_puts("micro/" NAME ": nic up, mac ");
    for (i = 0; i < 6u; i++) {
        micro_put_hex(g_mac[i]);
        micro_puts(i == 5u ? "\n" : ":");
    }

    /* Step 1: ARP the gateway. hype answers with its own MAC and learns ours from the request. */
    send_frame(frame, build_arp(frame));

    /*
     * Step 2: wait for the ARP reply, then send the query and wait for the answer. One loop for
     * both, because the receive path is the same.
     *
     * PAUSE, not HLT: tests/micro/ps2.c records that a hlt-based wait loop on this hypervisor does
     * not reliably make forward progress (#553), so a bound expressed inside one can never fire.
     */
    for (spins = 0; spins < 40000000ull; spins++) {
        uint16_t now;
        __asm__ volatile("pause" ::: "memory");
        now = ru[1];
        if (now == seen_used) {
            continue;
        }
        /* Walk every newly-completed receive descriptor. */
        while (seen_used != now) {
            uint32_t elem_off = 4u + 8u * (uint32_t)(seen_used % QUEUE_SIZE);
            uint32_t id = *(volatile uint32_t *)(uintptr_t)(RX_USED_GPA + elem_off);
            uint32_t got = *(volatile uint32_t *)(uintptr_t)(RX_USED_GPA + elem_off + 4u);
            const uint8_t *rb;
            uint16_t et;

            seen_used++;
            if (id >= QUEUE_SIZE || got <= NET_HDR_LEN) {
                continue;
            }
            rb = (const uint8_t *)(uintptr_t)(RX_BUF_BASE + (uint64_t)id * RX_BUF_STRIDE +
                                              NET_HDR_LEN);
            et = be16(rb + 12);

            if (et == 0x0806u && !gw_known) {
                /* ARP reply: opcode 2, and the sender is the address we asked about. */
                if (be16(rb + 20) == 2u) {
                    for (i = 0; i < 6u; i++) {
                        gw_mac[i] = rb[22 + i];
                    }
                    gw_known = 1;
                    micro_puts("micro/" NAME ": gateway answered, mac ");
                    for (i = 0; i < 6u; i++) {
                        micro_put_hex(gw_mac[i]);
                        micro_puts(i == 5u ? "\n" : ":");
                    }
                    send_frame(frame, build_dns(frame, gw_mac, DNS_IP, 0x4242u, 40000u));
                }
                continue;
            }
            if (et != 0x0800u) {
                continue;
            }
            /* IPv4 + UDP from port 53 is the answer. */
            if (rb[14 + 9] == 17u) {
                unsigned int ihl = (unsigned int)(rb[14] & 0x0Fu) * 4u;
                const uint8_t *udp = rb + 14u + ihl;
                if (be16(udp) == 53u && be16(udp + 8) == 0x4242u) {
                    unsigned int ancount = be16(udp + 8 + 6);
                    micro_puts("micro/" NAME ": DNS reply, rcode=");
                    micro_put_uint((unsigned)(udp[8 + 3] & 0x0Fu));
                    micro_puts(" answers=");
                    micro_put_uint(ancount);
                    micro_puts(" from ");
                    for (i = 0; i < 4u; i++) {
                        micro_put_uint(rb[14 + 12 + i]);
                        micro_puts(i == 3u ? "\n" : ".");
                    }
                    if ((udp[8 + 3] & 0x0Fu) != 0u) {
                        micro_fail(NAME, "the resolver answered with an error rcode -- the packet "
                                         "made the whole round trip, so hype's forwarding works; "
                                         "the name did not resolve");
                        micro_halt();
                    }
                    if (ancount == 0u) {
                        micro_fail(NAME, "the reply carried no answer records -- the round trip "
                                         "worked but nothing resolved");
                        micro_halt();
                    }
                    answered = 1;
                }
            }
        }
        if (answered) {
            break;
        }
    }

    if (!gw_known) {
        micro_fail(NAME, "no ARP reply from the gateway -- hype did not answer the guest's ARP, so "
                         "nothing the guest sends can be addressed. Check `arp_answered=` in the "
                         "per-VM NAT diagnostic");
        micro_halt();
    }
    if (!answered) {
        micro_fail(NAME, "the query went out and nothing came back -- read the fw-1 NAT and UPLINK "
                         "diagnostic lines: out=/sent= says whether hype transmitted, gw_mac= "
                         "whether it could address the wire, rx=/unclaimed= whether a reply arrived "
                         "and was matched to a mapping");
        micro_halt();
    }

    micro_puts("micro/" NAME ": www.google.com resolved through hype's NAT -- guest -> virtio-net "
               "-> NAPT -> e1000 -> the network -> back again.\n");
    micro_pass(NAME);
    micro_halt();
}
