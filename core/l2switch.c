#include "l2switch.h"

static int mac_eq(const uint8_t *a, const uint8_t *b) {
    unsigned int i;
    for (i = 0; i < 6u; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

void hype_l2sw_init(hype_l2switch_t *sw, int uplink_nat) {
    unsigned char *b = (unsigned char *)sw;
    unsigned long long i;
    for (i = 0; i < sizeof(*sw); i++) {
        b[i] = 0;
    }
    sw->uplink_nat = uplink_nat ? 1 : 0;
}

int hype_l2sw_add_member(hype_l2switch_t *sw, unsigned int vm_index) {
    if (sw->member_count >= HYPE_L2SW_MAX_MEMBERS) {
        return -1;
    }
    sw->members[sw->member_count] = vm_index;
    return (int)sw->member_count++;
}

int hype_l2sw_member_slot(const hype_l2switch_t *sw, unsigned int vm_index) {
    unsigned int i;
    for (i = 0; i < sw->member_count; i++) {
        if (sw->members[i] == vm_index) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Learn src_mac against src_slot. A multicast/broadcast SOURCE is never learned -- it is not a
 * station address, and a guest emitting one is either broken or hostile; learning it would let
 * that guest capture every flooded frame afterwards.
 */
static void l2sw_learn(hype_l2switch_t *sw, unsigned int src_slot, const uint8_t *src_mac) {
    unsigned int i;
    hype_l2sw_entry_t *free_e = 0;

    if ((src_mac[0] & 0x01u) != 0u) {
        return;
    }
    for (i = 0; i < HYPE_L2SW_MAC_TABLE; i++) {
        hype_l2sw_entry_t *e = &sw->table[i];
        if (e->used && mac_eq(e->mac, src_mac)) {
            if (e->member != (uint8_t)src_slot) {
                e->member = (uint8_t)src_slot;
                sw->moved++;
            }
            return;
        }
        if (!e->used && free_e == 0) {
            free_e = e;
        }
    }
    if (free_e == 0) {
        /* Full: evict round-robin. A bridge that refuses to learn floods forever, which is
         * correct but slow; cycling keeps the common stations resident. */
        free_e = &sw->table[sw->next_evict];
        sw->next_evict = (sw->next_evict + 1u) % HYPE_L2SW_MAC_TABLE;
    }
    for (i = 0; i < 6u; i++) {
        free_e->mac[i] = src_mac[i];
    }
    free_e->member = (uint8_t)src_slot;
    free_e->used = 1;
    sw->learned++;
}

hype_l2sw_verdict_t hype_l2sw_classify(hype_l2switch_t *sw, unsigned int src_slot,
                                       const uint8_t *frame, unsigned int len) {
    hype_l2sw_verdict_t v;
    const uint8_t *dst;
    unsigned int i;

    v.deliver_mask = 0;
    v.to_uplink = 0;
    if (frame == 0 || len < 14u || src_slot >= sw->member_count) {
        return v;
    }
    dst = frame;
    l2sw_learn(sw, src_slot, frame + 6);

    if ((dst[0] & 0x01u) != 0u) {
        /* Broadcast/multicast: every member but the sender, and (uplink=nat) the uplink side may
         * also care -- the plane's own ARP/NAT rules decide what to do with it there. */
        for (i = 0; i < sw->member_count; i++) {
            if (i != src_slot) {
                v.deliver_mask |= (1u << i);
            }
        }
        v.to_uplink = sw->uplink_nat;
        sw->flooded++;
        return v;
    }

    for (i = 0; i < HYPE_L2SW_MAC_TABLE; i++) {
        const hype_l2sw_entry_t *e = &sw->table[i];
        if (e->used && mac_eq(e->mac, dst)) {
            if (e->member != (uint8_t)src_slot) {
                v.deliver_mask = 1u << e->member;
                sw->forwarded++;
            }
            /* dst learned against the SENDER itself: a guest talking to its own MAC -- deliver
             * nowhere (a real switch would not hairpin it either). */
            return v;
        }
    }

    /* Unknown unicast: flood, and offer the uplink (a NAT gateway's MAC is not a member's). */
    for (i = 0; i < sw->member_count; i++) {
        if (i != src_slot) {
            v.deliver_mask |= (1u << i);
        }
    }
    v.to_uplink = sw->uplink_nat;
    sw->flooded++;
    return v;
}
