#ifndef HYPE_CORE_L2SWITCH_H
#define HYPE_CORE_L2SWITCH_H

#include <stdint.h>

/*
 * NET-6 (#223, plan.md §6e + §10 decision 53): the opt-in virtual switch -- one named, isolated
 * L2 broadcast domain shared by the NICs configured onto it.
 *
 * This module is the DECISION half, pure and unit-tested: membership, MAC learning, and "given
 * this frame from this member, who receives it". Moving the bytes (mailboxes, ring delivery,
 * the NAT uplink) stays in the forwarding plane, which consults the verdict this returns.
 *
 * Semantics are a learning bridge, deliberately minimal:
 *   - a frame's SOURCE MAC is learned against the sending member (moving a MAC re-learns it);
 *   - a unicast destination that is learned goes to exactly that member;
 *   - broadcast/multicast, and unicast the table has not learned, FLOOD to every member but the
 *     sender;
 *   - traffic never crosses switches, and a VM on no switch never appears here at all -- the
 *     default-deny isolation of §6e is untouched for everyone not configured onto a switch.
 *
 * Frames between members keep their real source MAC (a bridge, not the router hype is between
 * isolated segments) -- the members OPTED IN to sharing a segment, and seeing each other's MACs
 * is the semantics they asked for.
 */

/* Was chosen to match HYPE_CFG_MAX_VMS back when that was also 16 (one NIC per VM in v1, #583
 * warns above); #606 re-derived HYPE_CFG_MAX_VMS from decision 33's own physical-core reasoning,
 * which a shared L2 segment's useful member count has no reason to track -- left independent. */
#define HYPE_L2SW_MAX_MEMBERS 16u
#define HYPE_L2SW_MAC_TABLE 32u

typedef struct {
    uint8_t mac[6];
    uint8_t member; /* index into members[] */
    uint8_t used;
} hype_l2sw_entry_t;

typedef struct {
    /* VM indexes of this switch's members, fixed at startup from hype.cfg. */
    unsigned int members[HYPE_L2SW_MAX_MEMBERS];
    unsigned int member_count;
    int uplink_nat; /* 1 = uplink=nat (non-member IPv4 may take the NAT path), 0 = fully private */

    hype_l2sw_entry_t table[HYPE_L2SW_MAC_TABLE];
    unsigned int next_evict; /* round-robin victim when the table is full */

    /* Diagnostics -- a dropped frame with no counter is invisible (#84's lesson). */
    uint64_t learned;
    uint64_t moved;   /* a known MAC re-learned against a different member */
    uint64_t flooded; /* frames delivered by flooding */
    uint64_t forwarded; /* frames delivered by table hit */
} hype_l2switch_t;

/* The verdict for one frame. */
typedef struct {
    /* bit i set = deliver a copy to this switch's members[i]. The sender's own bit is never set. */
    uint32_t deliver_mask;
    /* 1 = this frame may ALSO take the uplink path (uplink=nat and dst is not a member's MAC).
     * The forwarding plane still applies its own NAT/on-link rules on top. */
    int to_uplink;
} hype_l2sw_verdict_t;

void hype_l2sw_init(hype_l2switch_t *sw, int uplink_nat);

/* Add a member VM. Returns the member slot index, or -1 when full. */
int hype_l2sw_add_member(hype_l2switch_t *sw, unsigned int vm_index);

/* The member slot of vm_index, or -1 when it is not on this switch. */
int hype_l2sw_member_slot(const hype_l2switch_t *sw, unsigned int vm_index);

/*
 * Classify one frame sent by members[src_slot]: learn its source MAC, and decide who receives it.
 * `frame` must hold at least the 14-byte Ethernet header; shorter frames get an empty verdict.
 * Pure apart from the learning-table update; no frame bytes are copied or modified.
 */
hype_l2sw_verdict_t hype_l2sw_classify(hype_l2switch_t *sw, unsigned int src_slot,
                                       const uint8_t *frame, unsigned int len);

#endif /* HYPE_CORE_L2SWITCH_H */
