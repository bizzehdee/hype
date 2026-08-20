/*
 * NET-4a/4b (#84/#85): two guests pinging each other across hype, with real ICMP.
 *
 * This is the other half of what a hypervisor's network has to do. `netdns` proves a guest can reach
 * the outside world through NAPT; this proves two guests on the same host can reach EACH OTHER, and
 * only when an operator said they may.
 *
 * WHY ICMP HERE AND NOT DNS. netdns avoids ICMP because QEMU's user networking can only forward it
 * if the developer's host permits unprivileged ICMP sockets -- a property of the machine, not of
 * hype. Guest-to-guest traffic never leaves the host: hype forwards the frame itself, so there is no
 * slirp and no host sysctl in the path. ICMP echo is therefore both available AND the most direct
 * reading of "these two can ping each other".
 *
 * WHAT HYPE HAS TO GET RIGHT for this to pass:
 *
 *   - proxy ARP, so each guest's ARP for the other resolves to hype rather than going unanswered
 *   - address learning, so hype knows which guest owns which IP -- nothing configures that
 *   - the peer check (#84's default-deny / #85's opt-in), so the frame is forwarded at all
 *   - the MAILBOX. The transmit path holds the sending VM's device lock, so hype cannot write into
 *     the peer's receive ring there; it queues, and the pump delivers. Two guests transmitting to
 *     each other simultaneously is exactly the case that would deadlock a direct handoff, and it is
 *     exactly what this test does.
 *   - rewriting the Ethernet source to hype's own router MAC, because hype is a ROUTER between two
 *     isolated segments and not a bridge across one.
 *
 * BOTH SIDES RUN THE SAME BINARY and are told apart by their kernel command line (#546):
 *
 *   cmdline = self=2 peer=3
 *
 * Both send echo requests and both answer any they receive, so each side passes only when it has
 * sent a request AND seen a reply to its own. A one-directional test would pass with the return path
 * broken.
 */
#include "micro_pci.h"

#define NAME "netpeer"

#include "micro_vnet.h"

static uint8_t PEER_IP[4] = {192, 168, 77, 3};

/*
 * `self=N peer=M` -- the last octet of each address, read with micro.h's own cmdline helpers rather
 * than a private parser: micro_cmdline_value() already matches at a word boundary, so `self=2` is
 * not found by looking for `elf=`.
 *
 * Parsed rather than derived from the VM index, because a guest cannot see its own index and
 * deriving it from anything else would couple this test to hype's VM ordering.
 */
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

void micro_main(uint64_t zero_page_gpa);

void micro_main(uint64_t zero_page_gpa) {
    const char *cmdline = micro_cmdline(zero_page_gpa);
    unsigned int self_octet;
    unsigned int peer_octet;
    unsigned int i;
    uint8_t frame[600];
    uint8_t gw_mac[6];
    int gw_known = 0;
    volatile uint16_t *ru = (volatile uint16_t *)(uintptr_t)RX_USED_GPA;
    uint16_t seen_used = 0;
    unsigned long long spins;
    unsigned int requests_seen = 0;
    unsigned int replies_seen = 0;
    unsigned int requests_sent = 0;
    uint16_t my_id;

    micro_puts("micro/" NAME ": start, cmdline '");
    micro_puts(cmdline != 0 ? cmdline : "(none)");
    micro_puts("'\n");

    self_octet = octet_of(cmdline, "self");
    peer_octet = octet_of(cmdline, "peer");
    if (self_octet == 0u || peer_octet == 0u || self_octet == peer_octet) {
        micro_fail(NAME, "the cmdline must carry `self=N peer=M` with two different non-zero last "
                         "octets -- without them both guests would claim the same address and "
                         "hype's address learning would map one IP to two MACs");
        micro_halt();
    }
    MY_IP[3] = (uint8_t)self_octet;
    PEER_IP[3] = (uint8_t)peer_octet;
    /* The identifier is derived from our own address so the two sides cannot collide, and so a reply
     * can be told apart from an echo of our own request coming back. */
    my_id = (uint16_t)(0x1000u + self_octet);

    if (find_and_bring_up() != 0) {
        micro_halt();
    }
    micro_puts("micro/" NAME ": up as 192.168.77.");
    micro_put_uint(self_octet);
    micro_puts(", peer .");
    micro_put_uint(peer_octet);
    micro_puts("\n");

    /* ARP the gateway. This is also what makes hype learn OUR address, which it needs before it can
     * forward anything TO us -- so both sides must do it before either can be reached. */
    send_frame(frame, build_arp(frame));

    for (spins = 0; spins < 60000000ull; spins++) {
        uint16_t now;
        __asm__ volatile("pause" ::: "memory");

        /* Once the gateway is known, keep sending requests: the peer may not have finished its own
         * bring-up yet, and a single request sent too early would be dropped for a reason that has
         * nothing to do with what is being tested. */
        if (gw_known && requests_sent < 40u && (spins % 400000ull) == 0ull) {
            send_frame(frame, build_echo(frame, gw_mac, PEER_IP, 8u, my_id,
                                         (uint16_t)requests_sent));
            requests_sent++;
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
                    micro_puts("micro/" NAME ": gateway answered\n");
                }
                continue;
            }
            if (et != 0x0800u || rb[14 + 9] != 1u) {
                continue; /* not ICMP */
            }
            {
                unsigned int ihl = (unsigned int)(rb[14] & 0x0Fu) * 4u;
                const uint8_t *icmp = rb + 14u + ihl;
                uint16_t rid = be16(icmp + 4);

                if (icmp[0] == 8u) {
                    /* An echo request FROM the peer. Answer it -- and check it really came from the
                     * peer's address, because a request from anywhere else would mean hype forwarded
                     * something it should not have. */
                    if (rb[14 + 12 + 3] != (uint8_t)peer_octet) {
                        micro_puts("micro/" NAME ": echo request from an unexpected address .");
                        micro_put_uint(rb[14 + 12 + 3]);
                        micro_puts("\n");
                        continue;
                    }
                    requests_seen++;
                    if (gw_known) {
                        send_frame(frame, build_echo(frame, gw_mac, PEER_IP, 0u, rid, be16(icmp + 6)));
                    }
                } else if (icmp[0] == 0u && rid == my_id) {
                    /* A reply to OUR request: the identifier is ours, so this is the return leg of
                     * a packet we sent, not an echo of somebody else's. */
                    replies_seen++;
                }
            }
        }
        if (requests_seen > 0u && replies_seen > 0u) {
            break;
        }
    }

    micro_puts("micro/" NAME ": sent=");
    micro_put_uint(requests_sent);
    micro_puts(" requests_from_peer=");
    micro_put_uint(requests_seen);
    micro_puts(" replies_to_us=");
    micro_put_uint(replies_seen);
    micro_puts("\n");

    if (!gw_known) {
        micro_fail(NAME, "no ARP reply -- hype did not answer this guest's ARP, so it never learned "
                         "the address and cannot forward anything here. Check `arp_answered=`");
        micro_halt();
    }
    if (replies_seen == 0u && requests_seen == 0u) {
        micro_fail(NAME, "nothing arrived from the peer at all -- if `fw-1 PEER` shows DENIED then "
                         "the pair is not in net_peers, which is the DEFAULT and correct behaviour "
                         "for VMs an operator did not connect");
        micro_halt();
    }
    if (replies_seen == 0u) {
        micro_fail(NAME, "the peer's requests arrive but our replies never come back -- forwarding "
                         "works in one direction only, so look at the OTHER guest's PEER counters");
        micro_halt();
    }
    if (requests_seen == 0u) {
        micro_fail(NAME, "our requests are answered but the peer's own never arrive -- one guest's "
                         "traffic is being forwarded and the other's is not");
        micro_halt();
    }

    micro_puts("micro/" NAME ": ping both ways across hype's network -- this guest and its peer each "
               "reached the other.\n");
    micro_pass(NAME);
    micro_halt();
}
