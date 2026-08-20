/*
 * The whole thing, from inside a guest: resolve www.google.com, PING it, and ping the other guest.
 *
 * netdns proves a guest can reach a resolver through NAPT. netpeer proves two guests can reach each
 * other. This is both at once plus the step neither takes -- an ICMP echo to a real internet host --
 * run as TWO guests so the result is about a hypervisor with several VMs rather than one.
 *
 * WHY ICMP TO THE INTERNET IS TESTABLE HERE, when netdns says it is not. It depends on the host:
 * QEMU's user networking forwards ICMP only if the machine permits unprivileged ICMP sockets
 * (`net.ipv4.ping_group_range`). netdns avoids ICMP deliberately so it can never fail for that
 * reason. This test DOES use it, so a failure at the ping step may be the host rather than hype --
 * and the test says so in its own failure message rather than leaving the reader to guess. The DNS
 * step passing while the ping step fails is the signature of exactly that.
 *
 * WHAT EACH STEP PROVES, because they fail differently:
 *
 *   ARP        hype answers the guest's ARP and learns its address. Nothing else can work first.
 *   DNS        NAPT rewrites a UDP source port and fixes the checksum, both ways. A reply means the
 *              mapping was found on the way back, keyed on this guest's own entry.
 *   ping WAN   NAPT rewrites an ICMP echo IDENTIFIER -- a different field in a different protocol
 *              with a checksum that does NOT cover the IP header. Passing DNS says nothing about it.
 *   ping peer  the frame never touches the physical network: hype forwards it between two isolated
 *              segments after checking the pair is allowed (#84/#85).
 *
 * Both guests do all four, so the two VMs' logs each show the whole sequence independently.
 */
#include "micro_pci.h"

#define NAME "netgoal"

#include "micro_vnet.h"

static uint8_t PEER_IP[4] = {192, 168, 77, 3};
static const uint8_t DNS_IP[4] = {8, 8, 8, 8};

static unsigned int octet_of(const char *cmdline, const char *key) {
    const char *v = micro_cmdline_value(cmdline, key);
    unsigned int n = 0;

    if (v == 0 || *v < '0' || *v > '9') {
        return 0u;
    }
    while (*v >= '0' && *v <= '9') {
        n = n * 10u + (unsigned int)(*v - '0');
        v++;
    }
    return (n > 255u) ? 0u : n;
}

/*
 * Pull the first A record's address out of a DNS answer.
 *
 * SCANNED rather than parsed, and that is a deliberate limit. A correct walk of the answer section
 * has to follow name-compression pointers, and a compression-pointer loop in a guest with no stack
 * and no allocator is a hang rather than an error. Instead this looks for the fixed 12-byte shape a
 * type-A/class-IN record always has -- 00 01 00 01, a 4-byte TTL, then 00 04 -- inside the message,
 * and takes the four bytes after it.
 *
 * The risk is a false positive from payload bytes that happen to match, which is why the caller
 * PINGS the result: an address that answers an echo is the address. Nothing here depends on the
 * scan being a correct DNS parser, only on it producing a candidate.
 */
static int first_a_record(const uint8_t *dns, unsigned int len, uint8_t out[4]) {
    unsigned int i;

    if (len < 12u + 12u + 4u) {
        return 0;
    }
    for (i = 12u; i + 14u <= len; i++) {
        if (dns[i] == 0x00u && dns[i + 1] == 0x01u && dns[i + 2] == 0x00u && dns[i + 3] == 0x01u &&
            dns[i + 8] == 0x00u && dns[i + 9] == 0x04u) {
            out[0] = dns[i + 10];
            out[1] = dns[i + 11];
            out[2] = dns[i + 12];
            out[3] = dns[i + 13];
            /* 0.0.0.0 is not an address anything answers, so it is a match on padding. */
            if (out[0] == 0u && out[1] == 0u && out[2] == 0u && out[3] == 0u) {
                continue;
            }
            return 1;
        }
    }
    return 0;
}

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cmdline = micro_cmdline(zero_page_gpa);
    unsigned int self_octet;
    unsigned int peer_octet;
    unsigned int i;
    uint8_t frame[600];
    uint8_t gw_mac[6];
    uint8_t wan_ip[4] = {0, 0, 0, 0};
    int gw_known = 0;
    int resolved = 0;
    int wan_pong = 0;
    unsigned int peer_req_seen = 0;
    unsigned int peer_pong = 0;
    unsigned int dns_sent = 0;
    unsigned int wan_pings = 0;
    unsigned int peer_pings = 0;
    volatile uint16_t *ru = (volatile uint16_t *)(uintptr_t)RX_USED_GPA;
    uint16_t seen_used = 0;
    unsigned long long spins;
    uint16_t my_id;

    micro_puts("micro/" NAME ": start, cmdline '");
    micro_puts(cmdline != 0 ? cmdline : "(none)");
    micro_puts("'\n");

    self_octet = octet_of(cmdline, "self");
    peer_octet = octet_of(cmdline, "peer");
    if (self_octet == 0u || peer_octet == 0u || self_octet == peer_octet) {
        micro_fail(NAME, "the cmdline must carry `self=N peer=M` with two different non-zero last "
                         "octets -- two guests sharing an address would make hype's address "
                         "learning map one IP to two MACs");
        micro_halt();
    }
    MY_IP[3] = (uint8_t)self_octet;
    PEER_IP[3] = (uint8_t)peer_octet;
    my_id = (uint16_t)(0x2000u + self_octet);

    if (find_and_bring_up() != 0) {
        micro_halt();
    }
    micro_puts("micro/" NAME ": up as 192.168.77.");
    micro_put_uint(self_octet);
    micro_puts("\n");

    send_frame(frame, build_arp(frame));

    for (spins = 0; spins < 120000000ull; spins++) {
        uint16_t now;
        __asm__ volatile("pause" ::: "memory");

        /*
         * Retry each outstanding step periodically. A single attempt would be a race against the
         * other guest's boot and against DNS, and a lost first packet would read as a hype fault.
         */
        if (gw_known && (spins % 600000ull) == 0ull) {
            if (!resolved && dns_sent < 20u) {
                send_frame(frame, build_dns(frame, gw_mac, DNS_IP, 0x5150u, 40000u));
                dns_sent++;
            }
            if (resolved && !wan_pong && wan_pings < 30u) {
                send_frame(frame,
                           build_echo(frame, gw_mac, wan_ip, 8u, my_id, (uint16_t)wan_pings));
                wan_pings++;
            }
            if (peer_pong == 0u && peer_pings < 40u) {
                send_frame(frame, build_echo(frame, gw_mac, PEER_IP, 8u,
                                             (uint16_t)(my_id + 1u), (uint16_t)peer_pings));
                peer_pings++;
            }
        }

        now = ru[1];
        if (now == seen_used) {
            continue;
        }
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

            if (et == 0x0806u) {
                if (be16(rb + 20) == 2u && !gw_known) {
                    for (i = 0; i < 6u; i++) {
                        gw_mac[i] = rb[22 + i];
                    }
                    gw_known = 1;
                    micro_puts("micro/" NAME ": ARP answered by hype\n");
                }
                continue;
            }
            if (et != 0x0800u) {
                continue;
            }
            {
                unsigned int ihl = (unsigned int)(rb[14] & 0x0Fu) * 4u;
                uint8_t proto = rb[14 + 9];
                unsigned int total = be16(rb + 16);

                if (proto == 17u && !resolved) {
                    const uint8_t *udp = rb + 14u + ihl;
                    if (be16(udp) == 53u && be16(udp + 8) == 0x5150u) {
                        unsigned int dns_len = (total > ihl + 8u) ? (total - ihl - 8u) : 0u;
                        if (first_a_record(udp + 8, dns_len, wan_ip)) {
                            resolved = 1;
                            micro_puts("micro/" NAME ": www.google.com resolves to ");
                            for (i = 0; i < 4u; i++) {
                                micro_put_uint(wan_ip[i]);
                                micro_puts(i == 3u ? "\n" : ".");
                            }
                        } else {
                            micro_puts("micro/" NAME ": DNS answered but no A record was found in "
                                       "it\n");
                        }
                    }
                } else if (proto == 1u) {
                    const uint8_t *icmp = rb + 14u + ihl;
                    uint16_t rid = be16(icmp + 4);

                    if (icmp[0] == 8u) {
                        /* An echo request. Only the peer should be able to send us one -- anything
                         * else means hype forwarded traffic it should not have. */
                        if (rb[14 + 12 + 3] != (uint8_t)peer_octet) {
                            micro_puts("micro/" NAME ": UNEXPECTED echo request from .");
                            micro_put_uint(rb[14 + 12 + 3]);
                            micro_puts("\n");
                            continue;
                        }
                        peer_req_seen++;
                        if (gw_known) {
                            send_frame(frame, build_echo(frame, gw_mac, PEER_IP, 0u, rid,
                                                         be16(icmp + 6)));
                        }
                    } else if (icmp[0] == 0u) {
                        if (rid == my_id && resolved && !wan_pong) {
                            wan_pong = 1;
                            micro_puts("micro/" NAME ": PING REPLY from ");
                            for (i = 0; i < 4u; i++) {
                                micro_put_uint(rb[14 + 12 + i]);
                                micro_puts(i == 3u ? " -- www.google.com is reachable\n" : ".");
                            }
                        } else if (rid == (uint16_t)(my_id + 1u)) {
                            peer_pong++;
                        }
                    }
                }
            }
        }
        if (resolved && wan_pong && peer_pong > 0u && peer_req_seen > 0u) {
            break;
        }
    }

    micro_puts("micro/" NAME ": resolved=");
    micro_put_uint((unsigned)resolved);
    micro_puts(" wan_ping_reply=");
    micro_put_uint((unsigned)wan_pong);
    micro_puts(" peer_replies=");
    micro_put_uint(peer_pong);
    micro_puts(" peer_requests_seen=");
    micro_put_uint(peer_req_seen);
    micro_puts("\n");

    if (!gw_known) {
        micro_fail(NAME, "hype never answered this guest's ARP, so nothing else could work -- check "
                         "`arp_answered=` in the per-VM NAT diagnostic");
        micro_halt();
    }
    if (!resolved) {
        micro_fail(NAME, "www.google.com did not resolve -- the DNS query is UDP through NAPT, so "
                         "read out=/sent= (did hype transmit) and rx=/unclaimed= (did a reply come "
                         "back and match a mapping) on the fw-1 lines");
        micro_halt();
    }
    if (!wan_pong) {
        micro_fail(NAME, "resolved but the ICMP echo got no reply. NOTE: unlike the DNS step this "
                         "one needs the HOST to permit unprivileged ICMP sockets for QEMU's user "
                         "networking to forward it (net.ipv4.ping_group_range) -- so on a host that "
                         "forbids it this failure is the environment, not hype. Resolution having "
                         "worked is what tells the two apart");
        micro_halt();
    }
    if (peer_req_seen == 0u || peer_pong == 0u) {
        micro_fail(NAME, "the internet works but the other guest does not -- if `fw-1 PEER` shows "
                         "DENIED the pair is not in net_peers, which is the correct DEFAULT; if it "
                         "shows sent>0 recv=0 then forwarding works one way only");
        micro_halt();
    }

    micro_puts("micro/" NAME ": THIS GUEST pinged www.google.com AND its peer, both through hype.\n");
    micro_pass(NAME);
    micro_halt();
}
