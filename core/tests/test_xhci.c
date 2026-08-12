#include <stdio.h>
#include "../xhci.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_reg_offsets(void) {
    CHECK_HEX("op base = caplength", 0x20u, hype_xhci_op_base(0x20));
    CHECK_HEX("portsc port 1", 0x400u, hype_xhci_portsc_offset(1));
    CHECK_HEX("portsc port 2", 0x410u, hype_xhci_portsc_offset(2));
    CHECK_HEX("portsc port 5", 0x440u, hype_xhci_portsc_offset(5));
    /* DBOFF low 2 bits are RsvdZ; slot 0 = command doorbell. */
    CHECK_HEX("doorbell slot 0", 0x3000u, hype_xhci_doorbell_offset(0x3000, 0));
    CHECK_HEX("doorbell slot 1", 0x3004u, hype_xhci_doorbell_offset(0x3000, 1));
    CHECK_HEX("doorbell masks low bits", 0x3000u, hype_xhci_doorbell_offset(0x3002, 0));
    /* RTSOFF low 5 bits RsvdZ; IR0 at +0x20; ERDP at +0x18. */
    CHECK_HEX("ir0 IMAN", 0x1020u, hype_xhci_ir0_offset(0x1000, HYPE_XHCI_IR_IMAN));
    CHECK_HEX("ir0 ERDP", 0x1038u, hype_xhci_ir0_offset(0x1000, HYPE_XHCI_IR_ERDP));
    CHECK_HEX("ir0 masks low bits", 0x1020u, hype_xhci_ir0_offset(0x101F, HYPE_XHCI_IR_IMAN));
}

static void test_cap_fields(void) {
    /* MaxSlots=32(0x20), MaxIntrs=8(0x008<<8), MaxPorts=4(0x04<<24). */
    uint32_t hcs1 = 0x04000820u;
    CHECK_HEX("max slots", 0x20u, hype_xhci_max_slots(hcs1));
    CHECK_HEX("max intrs", 0x08u, hype_xhci_max_intrs(hcs1));
    CHECK_HEX("max ports", 0x04u, hype_xhci_max_ports(hcs1));

    /* Max Scratchpad: Hi[25:21]=0, Lo[31:27]=3 -> 3. */
    CHECK_HEX("scratchpads lo only", 3u, hype_xhci_max_scratchpads(3u << 27));
    /* Hi=1 (<<21), Lo=0 -> 32. */
    CHECK_HEX("scratchpads hi", 32u, hype_xhci_max_scratchpads(1u << 21));

    CHECK_HEX("ac64 set", 1, hype_xhci_ac64(0x1u));
    CHECK_HEX("ac64 clear", 0, hype_xhci_ac64(0x2u));
    CHECK_HEX("ctx size 64 (CSZ set)", 64u, hype_xhci_context_size(1u << 2));
    CHECK_HEX("ctx size 32 (CSZ clear)", 32u, hype_xhci_context_size(0u));
    CHECK_HEX("xecp dword offset", 0x1234u, hype_xhci_xecp_offset(0x12340000u));
}

static void test_cmd_trbs(void) {
    uint32_t t[4];

    hype_xhci_trb_noop_cmd(t, 1);
    CHECK_HEX("noop type", HYPE_XHCI_TRB_NOOP_CMD, hype_xhci_trb_type(t));
    CHECK_HEX("noop cycle 1", 1, hype_xhci_trb_cycle(t));
    hype_xhci_trb_noop_cmd(t, 0);
    CHECK_HEX("noop cycle 0", 0, hype_xhci_trb_cycle(t));

    hype_xhci_trb_enable_slot(t, 1);
    CHECK_HEX("enable slot type", HYPE_XHCI_TRB_ENABLE_SLOT, hype_xhci_trb_type(t));

    hype_xhci_trb_link(t, 0x1234000ull, 1);
    CHECK_HEX("link type", HYPE_XHCI_TRB_LINK, hype_xhci_trb_type(t));
    CHECK_HEX("link ptr low", 0x1234000u, t[0]);
    CHECK_HEX("link ptr high", 0u, t[1]);
    CHECK_HEX("link toggle-cycle bit", 1u, (t[3] >> 1) & 1u);

    hype_xhci_trb_address_device(t, 0x2000ull, 7, 0, 1);
    CHECK_HEX("addrdev type", HYPE_XHCI_TRB_ADDRESS_DEVICE, hype_xhci_trb_type(t));
    CHECK_HEX("addrdev ctx ptr", 0x2000u, t[0]);
    CHECK_HEX("addrdev slot id", 7u, (t[3] >> 24) & 0xFFu);
    CHECK_HEX("addrdev bsr clear", 0u, (t[3] >> 9) & 1u);
    hype_xhci_trb_address_device(t, 0x2000ull, 7, 1, 1);
    CHECK_HEX("addrdev bsr set", 1u, (t[3] >> 9) & 1u);

    hype_xhci_trb_configure_endpoint(t, 0x3000ull, 7, 1);
    CHECK_HEX("configep type", HYPE_XHCI_TRB_CONFIG_EP, hype_xhci_trb_type(t));
    CHECK_HEX("configep ctx ptr", 0x3000u, t[0]);
    CHECK_HEX("configep slot id", 7u, (t[3] >> 24) & 0xFFu);

    hype_xhci_trb_disable_slot(t, 5, 1);
    CHECK_HEX("disable-slot type", HYPE_XHCI_TRB_DISABLE_SLOT, hype_xhci_trb_type(t));
    CHECK_HEX("disable-slot id", 5u, (t[3] >> 24) & 0xFFu);
}

static void test_control_transfer_trbs(void) {
    uint32_t t[4];

    /* GET_DESCRIPTOR(device): bmRequestType=0x80, bRequest=6, wValue=0x0100,
     * wIndex=0, wLength=18, IN data. */
    hype_xhci_trb_setup_stage(t, 0x80, 6, 0x0100, 0, 18, HYPE_XHCI_TRT_IN, 1);
    CHECK_HEX("setup type", HYPE_XHCI_TRB_SETUP_STAGE, hype_xhci_trb_type(t));
    CHECK_HEX("setup bmReqType+bReq+wValue", 0x01000680u, t[0]);
    CHECK_HEX("setup wIndex+wLength", (18u << 16) | 0u, t[1]);
    CHECK_HEX("setup xfer len 8", 8u, t[2]);
    CHECK_HEX("setup IDT set", 1u, (t[3] >> 6) & 1u);
    CHECK_HEX("setup TRT=IN", HYPE_XHCI_TRT_IN, (t[3] >> 16) & 0x3u);

    hype_xhci_trb_data_stage(t, 0xABCD000ull, 18, 1, 1);
    CHECK_HEX("data type", HYPE_XHCI_TRB_DATA_STAGE, hype_xhci_trb_type(t));
    CHECK_HEX("data buf low", 0xABCD000u, t[0]);
    CHECK_HEX("data length", 18u, t[2] & 0x1FFFFu);
    CHECK_HEX("data dir IN", 1u, (t[3] >> 16) & 1u);

    hype_xhci_trb_status_stage(t, 0, 1, 1);
    CHECK_HEX("status type", HYPE_XHCI_TRB_STATUS_STAGE, hype_xhci_trb_type(t));
    CHECK_HEX("status dir OUT", 0u, (t[3] >> 16) & 1u);
    CHECK_HEX("status IOC set", 1u, (t[3] >> 5) & 1u);

    /* opposite branches: OUT data stage, and status stage dir-IN with no IOC */
    hype_xhci_trb_data_stage(t, 0x5000ull, 64, 0, 0);
    CHECK_HEX("data dir OUT", 0u, (t[3] >> 16) & 1u);
    CHECK_HEX("data cycle 0", 0, hype_xhci_trb_cycle(t));
    hype_xhci_trb_status_stage(t, 1, 0, 1);
    CHECK_HEX("status dir IN", 1u, (t[3] >> 16) & 1u);
    CHECK_HEX("status IOC clear", 0u, (t[3] >> 5) & 1u);

    hype_xhci_trb_normal(t, 0xCC00ull, 512, 1);
    CHECK_HEX("normal type", HYPE_XHCI_TRB_NORMAL, hype_xhci_trb_type(t));
    CHECK_HEX("normal buf", 0xCC00u, t[0]);
    CHECK_HEX("normal len", 512u, t[2] & 0x1FFFFu);
    CHECK_HEX("normal IOC", 1u, (t[3] >> 5) & 1u);
    CHECK_HEX("normal ISP", 1u, (t[3] >> 2) & 1u);

    /* link with cycle 0 (opposite of the cmd-trb test's cycle 1) */
    hype_xhci_trb_link(t, 0x8000ull, 0);
    CHECK_HEX("link cycle 0", 0, hype_xhci_trb_cycle(t));
}

static void test_event_decode(void) {
    uint32_t t[4] = {0, 0, 0, 0};
    /* Command Completion Event: TRB ptr in dw0/1, CC in status[31:24],
     * slot in control[31:24], type in [15:10]. */
    t[0] = 0x9000u;
    t[1] = 0x1u;
    t[2] = (HYPE_XHCI_CC_SUCCESS << 24) | 0u;
    t[3] = ((uint32_t)HYPE_XHCI_TRB_CMD_COMPLETION << 10) | (5u << 24) | 1u;
    CHECK_HEX("event type", HYPE_XHCI_TRB_CMD_COMPLETION, hype_xhci_trb_type(t));
    CHECK_HEX("event cc success", HYPE_XHCI_CC_SUCCESS, hype_xhci_event_cc(t));
    CHECK_HEX("event slot id", 5u, hype_xhci_event_slot_id(t));
    CHECK_HEX("event cycle", 1, hype_xhci_trb_cycle(t));
    CHECK_HEX("event trb ptr", 0x100009000ull, hype_xhci_event_trb_ptr(t));

    /* Port Status Change Event: Port ID in param[31:24]. */
    t[0] = (3u << 24);
    t[3] = ((uint32_t)HYPE_XHCI_TRB_PORT_STATUS << 10);
    CHECK_HEX("port status type", HYPE_XHCI_TRB_PORT_STATUS, hype_xhci_trb_type(t));
    CHECK_HEX("port id", 3u, hype_xhci_event_port_id(t));

    /* Transfer Event residue in status[23:0]. */
    t[2] = 0x000004u | (HYPE_XHCI_CC_SHORT_PACKET << 24);
    CHECK_HEX("xfer residue", 4u, hype_xhci_event_xfer_residue(t));
    CHECK_HEX("xfer short-packet cc", HYPE_XHCI_CC_SHORT_PACKET, hype_xhci_event_cc(t));
}

static void test_context_encoders(void) {
    uint32_t c[8];

    hype_xhci_input_ctrl_ctx(c, HYPE_XHCI_ADD_SLOT | HYPE_XHCI_ADD_EP0, 0);
    CHECK_HEX("icc drop flags 0", 0u, c[0]);
    CHECK_HEX("icc add flags A0|A1", 0x3u, c[1]);

    /* route 0, speed 4 (SS), ctx entries 1, root port 3, no TT */
    hype_xhci_slot_ctx(c, 0, 4, 1, 3, 0, 0);
    CHECK_HEX("slot speed field", 4u, (c[0] >> 20) & 0xFu);
    CHECK_HEX("slot ctx entries", 1u, (c[0] >> 27) & 0x1Fu);
    CHECK_HEX("slot root port", 3u, (c[1] >> 16) & 0xFFu);
    CHECK_HEX("slot no TT", 0u, c[2]);
    /* route 0x21 (tier2 port2, tier1 port1), FS behind HS hub slot 4 port 3 */
    hype_xhci_slot_ctx(c, 0x21, 1, 1, 7, 4, 3);
    CHECK_HEX("slot route string", 0x21u, c[0] & 0xFFFFFu);
    CHECK_HEX("slot TT hub slot", 4u, c[2] & 0xFFu);
    CHECK_HEX("slot TT port", 3u, (c[2] >> 8) & 0xFFu);

    CHECK_HEX("route tier1 port5", 0x5u, hype_xhci_route_append(0, 1, 5));
    CHECK_HEX("route tier2 port2 on 0x5", 0x25u, hype_xhci_route_append(0x5u, 2, 2));
    CHECK_HEX("route tier0 ignored", 0x5u, hype_xhci_route_append(0x5u, 0, 9));

    /* EP0: MPS 512, TR dequeue 0x9000, DCS 1 */
    hype_xhci_ep0_ctx(c, 512, 0x9000ull, 1);
    CHECK_HEX("ep0 CErr=3", 3u, (c[1] >> 1) & 0x3u);
    CHECK_HEX("ep0 type=Control(4)", 4u, (c[1] >> 3) & 0x7u);
    CHECK_HEX("ep0 max packet 512", 512u, (c[1] >> 16) & 0xFFFFu);
    CHECK_HEX("ep0 TR dequeue + DCS", 0x9001u, c[2]);
    CHECK_HEX("ep0 avg trb len 8", 8u, c[4]);

    /* generic EP context: bulk IN, MPS 1024 */
    hype_xhci_ep_ctx(c, HYPE_XHCI_EP_TYPE_BULK_IN, 1024, 0x7000ull, 1);
    CHECK_HEX("bulk-in ep type=6", 6u, (c[1] >> 3) & 0x7u);
    CHECK_HEX("bulk-in mps 1024", 1024u, (c[1] >> 16) & 0xFFFFu);
    CHECK_HEX("bulk-in TR dq + DCS", 0x7001u, c[2]);

    CHECK_HEX("mps SuperSpeed", 512u, hype_xhci_default_mps(4));
    CHECK_HEX("mps Low", 8u, hype_xhci_default_mps(2));
    CHECK_HEX("mps High", 64u, hype_xhci_default_mps(3));
    CHECK_HEX("mps Full default", 64u, hype_xhci_default_mps(1));
}

static void test_hub_helpers(void) {
    uint8_t devdesc[18] = {0};
    uint8_t hubdesc[8] = {0};

    /* Device-descriptor class byte (offset 4) drives hub detection. */
    devdesc[4] = HYPE_USB_CLASS_HUB;
    CHECK_HEX("dev is hub", 1u, (unsigned)hype_xhci_dev_is_hub(devdesc));
    devdesc[4] = HYPE_USB_CLASS_MSC;
    CHECK_HEX("dev not hub", 0u, (unsigned)hype_xhci_dev_is_hub(devdesc));

    /* bNbrPorts (hub descriptor offset 2), clamped at 15. */
    hubdesc[2] = 4u;
    CHECK_HEX("hub 4 ports", 4u, hype_xhci_hub_nbr_ports(hubdesc));
    hubdesc[2] = 200u;
    CHECK_HEX("hub ports clamp", 15u, hype_xhci_hub_nbr_ports(hubdesc));

    /* TT is required only for a LS/FS device behind a HS hub. */
    CHECK_HEX("TT HS->FS", 1u,
              (unsigned)hype_xhci_tt_required(HYPE_USB_SPEED_HIGH, HYPE_USB_SPEED_FULL));
    CHECK_HEX("TT HS->LS", 1u,
              (unsigned)hype_xhci_tt_required(HYPE_USB_SPEED_HIGH, HYPE_USB_SPEED_LOW));
    CHECK_HEX("TT HS->HS no", 0u,
              (unsigned)hype_xhci_tt_required(HYPE_USB_SPEED_HIGH, HYPE_USB_SPEED_HIGH));
    CHECK_HEX("TT SS->FS no", 0u,
              (unsigned)hype_xhci_tt_required(4u, HYPE_USB_SPEED_FULL));
}

static void test_msc_config_parse(void) {
    /* config(9) + interface(9, MSC/SCSI/BOT) + EP OUT(7, bulk) + EP IN(7, bulk) */
    uint8_t cfg[] = {
        9, 0x02, 32, 0, 1, 1, 0, 0x80, 50,          /* config: total len 32, cfgValue 1 */
        9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50, 0,       /* interface 0: MSC/SCSI/BOT, 2 EPs */
        7, 0x05, 0x81, 0x02, 0x00, 0x02, 0,          /* EP 0x81 IN bulk, MPS 512 */
        7, 0x05, 0x02, 0x02, 0x00, 0x02, 0           /* EP 0x02 OUT bulk, MPS 512 */
    };
    hype_xhci_msc_eps_t m;
    CHECK_HEX("msc parse ok", 0, hype_xhci_msc_find_endpoints(cfg, sizeof(cfg), &m));
    CHECK_HEX("msc found", 1, m.found);
    CHECK_HEX("msc config value", 1u, m.config_value);
    CHECK_HEX("msc iface num", 0u, m.interface_num);
    CHECK_HEX("bulk in ep", 0x81u, m.bulk_in_ep);
    CHECK_HEX("bulk out ep", 0x02u, m.bulk_out_ep);
    CHECK_HEX("bulk in mps", 512u, m.bulk_in_mps);

    /* a non-MSC interface -> not found (endpoints ignored) */
    {
        uint8_t cfg2[] = {
            9, 0x02, 25, 0, 1, 1, 0, 0x80, 50,
            9, 0x04, 0, 0, 1, 0x03, 0x00, 0x00, 0,   /* HID interface, not MSC */
            7, 0x05, 0x81, 0x03, 0x08, 0x00, 10      /* interrupt EP */
        };
        hype_xhci_msc_eps_t m2;
        CHECK_HEX("non-msc not found", -1, hype_xhci_msc_find_endpoints(cfg2, sizeof(cfg2), &m2));
    }

    /* DCI mapping */
    CHECK_HEX("dci ep0-in style (0x81)", 3u, hype_xhci_ep_dci(0x81)); /* num1 IN -> 2*1+1 */
    CHECK_HEX("dci 0x02 OUT", 4u, hype_xhci_ep_dci(0x02));            /* num2 OUT -> 2*2+0 */

    /* malformed: a zero-length descriptor stops the walk -> not found */
    {
        uint8_t bad[] = { 9, 0x02, 20, 0, 1, 1, 0, 0x80, 50, 0, 0, 0 };
        hype_xhci_msc_eps_t mb;
        CHECK_HEX("zero-len desc -> not found", -1,
                  hype_xhci_msc_find_endpoints(bad, sizeof(bad), &mb));
    }
    /* truncated: bLength claims more than the buffer holds -> stop */
    {
        uint8_t trunc[] = { 9, 0x02, 32, 0, 1, 1, 0, 0x80, 50,
                            9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50 }; /* iface cut short */
        hype_xhci_msc_eps_t mt;
        CHECK_HEX("truncated -> not found", -1,
                  hype_xhci_msc_find_endpoints(trunc, sizeof(trunc), &mt));
    }
    /* endpoint before any interface + a non-bulk EP + only a bulk IN (no OUT) */
    {
        uint8_t partial[] = {
            9, 0x02, 39, 0, 1, 1, 0, 0x80, 50,
            7, 0x05, 0x83, 0x02, 0x00, 0x02, 0,       /* bulk EP before any interface -> ignored */
            9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50, 0,    /* MSC interface */
            7, 0x05, 0x84, 0x03, 0x08, 0x00, 10,      /* interrupt EP (not bulk) -> ignored */
            7, 0x05, 0x81, 0x02, 0x00, 0x02, 0        /* bulk IN only, no OUT */
        };
        hype_xhci_msc_eps_t mp;
        CHECK_HEX("only bulk-in -> not found", -1,
                  hype_xhci_msc_find_endpoints(partial, sizeof(partial), &mp));
        CHECK_HEX("bulk-in recorded", 0x81u, mp.bulk_in_ep);
        CHECK_HEX("no bulk-out", 0u, mp.bulk_out_ep);
    }
    /* short config descriptor (blen<6): config value left 0, still parses iface */
    {
        uint8_t shortcfg[] = {
            5, 0x02, 0, 0, 1,                          /* config desc too short for cfgValue */
            9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50, 0,
            7, 0x05, 0x81, 0x02, 0x00, 0x02, 0,
            7, 0x05, 0x02, 0x02, 0x00, 0x02, 0
        };
        hype_xhci_msc_eps_t ms;
        CHECK_HEX("short cfg still finds eps", 0,
                  hype_xhci_msc_find_endpoints(shortcfg, sizeof(shortcfg), &ms));
        CHECK_HEX("short cfg value stays 0", 0u, ms.config_value);
    }
}


/* #254: the endpoint-recovery command TRBs and the endpoint-ID decoder. */
static void test_recovery_trbs(void) {
    uint32_t t[4];
    uint32_t evt[4] = {0, 0, 0, 0};

    hype_xhci_trb_stop_endpoint(t, 5u, 3u, 1);
    CHECK_HEX("stop-ep type", HYPE_XHCI_TRB_STOP_EP, (t[3] >> 10) & 0x3Fu);
    CHECK_HEX("stop-ep slot", 5u, (t[3] >> 24) & 0xFFu);
    CHECK_HEX("stop-ep dci", 3u, (t[3] >> 16) & 0x1Fu);
    CHECK_HEX("stop-ep cycle", 1u, t[3] & 1u);
    CHECK_HEX("stop-ep params zero", 0u, t[0] | t[1] | t[2]);

    hype_xhci_trb_reset_endpoint(t, 63u, 31u, 0);
    CHECK_HEX("reset-ep type", HYPE_XHCI_TRB_RESET_EP, (t[3] >> 10) & 0x3Fu);
    CHECK_HEX("reset-ep slot", 63u, (t[3] >> 24) & 0xFFu);
    CHECK_HEX("reset-ep dci", 31u, (t[3] >> 16) & 0x1Fu);
    CHECK_HEX("reset-ep cycle", 0u, t[3] & 1u);

    hype_xhci_trb_set_tr_dequeue(t, 0x123456789ABCD001ull, 2u, 4u, 1);
    CHECK_HEX("set-deq type", HYPE_XHCI_TRB_SET_TR_DEQUEUE, (t[3] >> 10) & 0x3Fu);
    CHECK_HEX("set-deq ptr lo (incl. DCS)", 0x9ABCD001u, t[0]);
    CHECK_HEX("set-deq ptr hi", 0x12345678u, t[1]);
    CHECK_HEX("set-deq slot", 2u, (t[3] >> 24) & 0xFFu);
    CHECK_HEX("set-deq dci", 4u, (t[3] >> 16) & 0x1Fu);

    /* The Transfer Event endpoint-ID field (dword3 bits 20:16). */
    evt[3] = (7u << 24) | (3u << 16);
    CHECK_HEX("event ep id", 3u, hype_xhci_event_ep_id(evt));
    CHECK_HEX("event slot id", 7u, hype_xhci_event_slot_id(evt));
    evt[3] = (1u << 24) | (31u << 16);
    CHECK_HEX("event ep id max", 31u, hype_xhci_event_ep_id(evt));
}

static void test_parked_events(void) {
    hype_xhci_parked_t p;
    uint32_t cc = 0;
    uint32_t residue = 0;

    hype_xhci_parked_reset(&p);
    /* #266: nothing parked yet. */
    CHECK_HEX("empty table takes nothing", 0,
              hype_xhci_parked_take(&p, 1, 4, 0x1000, &cc, &residue));

    /* The observed case: a completion for ep=3 arrives while ep=4 is awaited. It must be
     * REMEMBERED, not discarded -- discarding is what stranded the BOT state machine. */
    hype_xhci_parked_put(&p, 1, 3, 0x140425000ull, 1, 7);
    CHECK_HEX("wrong endpoint does not claim it", 0,
              hype_xhci_parked_take(&p, 1, 4, 0x140425000ull, &cc, &residue));
    CHECK_HEX("wrong trb does not claim it", 0,
              hype_xhci_parked_take(&p, 1, 3, 0x999, &cc, &residue));
    CHECK_HEX("wrong slot does not claim it", 0,
              hype_xhci_parked_take(&p, 2, 3, 0x140425000ull, &cc, &residue));
    /* Only the exact (slot,dci,trb) may claim it -- that is what keeps data out of the
     * wrong buffer, which is #254's original corruption. */
    CHECK_HEX("exact match claims it", 1,
              hype_xhci_parked_take(&p, 1, 3, 0x140425000ull, &cc, &residue));
    CHECK_HEX("completion code carried through", 1, cc);
    CHECK_HEX("transfer residue carried through", 7, residue);
    /* Consumed: one event must never satisfy two waits. */
    CHECK_HEX("not claimable twice", 0,
              hype_xhci_parked_take(&p, 1, 3, 0x140425000ull, &cc, &residue));
}

static void test_parked_drop_exact(void) {
    hype_xhci_parked_t p;
    uint32_t cc = 0;
    uint32_t residue = 0;
    hype_xhci_parked_reset(&p);
    hype_xhci_parked_put(&p, 1, 3, 0x5000, 1, 0);
    hype_xhci_parked_put(&p, 1, 4, 0x5000, 1, 0);
    CHECK_HEX("stale exact completion dropped", 1,
              hype_xhci_parked_drop_exact(&p, 1, 3, 0x5000));
    CHECK_HEX("dropped completion cannot satisfy reused TRB", 0,
              hype_xhci_parked_take(&p, 1, 3, 0x5000, &cc, &residue));
    CHECK_HEX("other endpoint remains parked", 1,
              hype_xhci_parked_take(&p, 1, 4, 0x5000, &cc, &residue));
    CHECK_HEX("missing exact completion reports nothing", 0,
              hype_xhci_parked_drop_exact(&p, 1, 3, 0x5000));
}

static void test_exact_transfer_result(void) {
    CHECK_HEX("successful complete transfer", 1,
              hype_xhci_xfer_exact_ok(HYPE_XHCI_CC_SUCCESS, 0));
    CHECK_HEX("success with residue rejected", 0,
              hype_xhci_xfer_exact_ok(HYPE_XHCI_CC_SUCCESS, 1));
    CHECK_HEX("short completion with no residue is complete", 1,
              hype_xhci_xfer_exact_ok(HYPE_XHCI_CC_SHORT_PACKET, 0));
    CHECK_HEX("short incomplete transfer rejected", 0,
              hype_xhci_xfer_exact_ok(HYPE_XHCI_CC_SHORT_PACKET, 64));
    CHECK_HEX("failed transfer rejected", 0, hype_xhci_xfer_exact_ok(4, 0));
}

static void test_parked_no_duplicates(void) {
    hype_xhci_parked_t p;
    uint32_t cc = 0;
    unsigned i;
    hype_xhci_parked_reset(&p);
    /* A controller that re-reports the same completion must not fill the table. */
    for (i = 0; i < HYPE_XHCI_PARKED_MAX * 3u; i++) {
        hype_xhci_parked_put(&p, 1, 3, 0x2000, 1, 0);
    }
    CHECK_HEX("re-reported event stored once", 1,
              hype_xhci_parked_take(&p, 1, 3, 0x2000, &cc, 0));
    CHECK_HEX("and only once", 0, hype_xhci_parked_take(&p, 1, 3, 0x2000, &cc, 0));
}

static void test_parked_overflow_keeps_newest(void) {
    hype_xhci_parked_t p;
    uint32_t cc = 0;
    unsigned i;
    hype_xhci_parked_reset(&p);
    for (i = 0; i < HYPE_XHCI_PARKED_MAX + 2u; i++) {
        hype_xhci_parked_put(&p, 1, 3, 0x3000ull + i, i, 0);
    }
    /* The newest must still be there: silently refusing to record it would recreate the
     * discard bug this whole table exists to fix. */
    CHECK_HEX("newest survives overflow", 1,
              hype_xhci_parked_take(&p, 1, 3, 0x3000ull + HYPE_XHCI_PARKED_MAX + 1u,
                                    &cc, 0));
}

static void test_parked_drop_slot(void) {
    hype_xhci_parked_t p;
    uint32_t cc = 0;
    hype_xhci_parked_reset(&p);
    hype_xhci_parked_put(&p, 1, 3, 0x4000, 1, 0);
    hype_xhci_parked_put(&p, 2, 3, 0x4000, 1, 0);
    /*
     * After a reset the rings restart at index 0, so a parked event for the torn-down
     * state carries a TRB address the RETRY is about to reuse -- and would be claimed by
     * the wrong transfer. Dropping the slot's parked events is what stops the fix from
     * reintroducing #254's mis-attribution.
     */
    hype_xhci_parked_drop_slot(&p, 1);
    CHECK_HEX("slot 1 dropped", 0, hype_xhci_parked_take(&p, 1, 3, 0x4000, &cc, 0));
    CHECK_HEX("other slot untouched", 1, hype_xhci_parked_take(&p, 2, 3, 0x4000, &cc, 0));
}


/* --- USB-7 (#241): device inventory --- */

static hype_usb_devinfo_t mk_dev(unsigned ctrl, unsigned port, unsigned route, uint16_t vid,
                                 uint8_t cls) {
    hype_usb_devinfo_t d;
    d.controller = ctrl; d.root_port = port; d.route = route; d.slot = 1u; d.speed = 3u;
    d.vid = vid; d.pid = 0x1234u; d.dev_class = cls; d.dev_subclass = 0; d.dev_protocol = 0;
    d.owner = (uint8_t)HYPE_USB_OWNER_NONE;
    return d;
}

static void test_inventory_add_and_find(void) {
    hype_usb_inventory_t inv;
    hype_usb_devinfo_t a = mk_dev(0u, 1u, 0u, 0x0781u, HYPE_USB_CLASS_MSC);
    hype_usb_devinfo_t b = mk_dev(0u, 2u, 0u, 0x046Du, HYPE_USB_CLASS_HID);

    hype_usb_inventory_reset(&inv);
    CHECK_HEX("empty", 0, (int)inv.count);
    CHECK_HEX("add a -> index 0", 0, hype_usb_inventory_add(&inv, &a));
    CHECK_HEX("add b -> index 1", 1, hype_usb_inventory_add(&inv, &b));
    CHECK_HEX("count 2", 2, (int)inv.count);
    CHECK_HEX("find a by position", 0, hype_usb_inventory_find(&inv, 0u, 1u, 0u));
    CHECK_HEX("find b by position", 1, hype_usb_inventory_find(&inv, 0u, 2u, 0u));
    CHECK_HEX("absent position", -1, hype_usb_inventory_find(&inv, 0u, 9u, 0u));
    /* Same port on a DIFFERENT controller is a different device -- the whole point
     * of keying on position is that it must include which controller. */
    CHECK_HEX("other controller absent", -1, hype_usb_inventory_find(&inv, 1u, 1u, 0u));
}

static void test_inventory_dedupes_by_position_not_identity(void) {
    hype_usb_inventory_t inv;
    /* TWO IDENTICAL sticks in two ports are two devices; the same stick re-read at
     * one position is one. Keying on VID/PID would get both of these wrong. */
    hype_usb_devinfo_t p1 = mk_dev(0u, 1u, 0u, 0x0781u, HYPE_USB_CLASS_MSC);
    hype_usb_devinfo_t p2 = mk_dev(0u, 2u, 0u, 0x0781u, HYPE_USB_CLASS_MSC);
    hype_usb_devinfo_t p1_again = mk_dev(0u, 1u, 0u, 0x0781u, HYPE_USB_CLASS_MSC);

    hype_usb_inventory_reset(&inv);
    (void)hype_usb_inventory_add(&inv, &p1);
    (void)hype_usb_inventory_add(&inv, &p2);
    CHECK_HEX("identical devices in two ports are two entries", 2, (int)inv.count);
    CHECK_HEX("re-adding one position updates it", 0, hype_usb_inventory_add(&inv, &p1_again));
    CHECK_HEX("still two entries", 2, (int)inv.count);
}

static void test_inventory_update_cannot_unclaim(void) {
    hype_usb_inventory_t inv;
    hype_usb_devinfo_t d = mk_dev(0u, 1u, 0u, 0x0781u, HYPE_USB_CLASS_MSC);
    int i;

    hype_usb_inventory_reset(&inv);
    i = hype_usb_inventory_add(&inv, &d);
    hype_usb_inventory_claim(&inv, i, HYPE_USB_OWNER_HYPE);
    /* A re-scan re-adds with owner=NONE. That must NOT silently release hype's own
     * boot medium -- which would make it a passthrough candidate and let a guest be
     * handed the disk the log is being written to. */
    (void)hype_usb_inventory_add(&inv, &d);
    CHECK_HEX("re-add preserves hype's claim", (int)HYPE_USB_OWNER_HYPE, (int)inv.dev[0].owner);
}

static void test_inventory_next_unclaimed_class(void) {
    hype_usb_inventory_t inv;
    hype_usb_devinfo_t msc = mk_dev(0u, 1u, 0u, 0x0781u, HYPE_USB_CLASS_MSC);
    hype_usb_devinfo_t kbd = mk_dev(0u, 2u, 0u, 0x046Du, HYPE_USB_CLASS_HID);
    hype_usb_devinfo_t kbd2 = mk_dev(0u, 3u, 0u, 0x1234u, HYPE_USB_CLASS_HID);
    int first;

    hype_usb_inventory_reset(&inv);
    (void)hype_usb_inventory_add(&inv, &msc);
    (void)hype_usb_inventory_add(&inv, &kbd);
    (void)hype_usb_inventory_add(&inv, &kbd2);

    first = hype_usb_inventory_next_unclaimed_class(&inv, HYPE_USB_CLASS_HID, -1);
    CHECK_HEX("first HID is index 1", 1, first);
    CHECK_HEX("second HID is index 2", 2,
              hype_usb_inventory_next_unclaimed_class(&inv, HYPE_USB_CLASS_HID, first));
    CHECK_HEX("no third HID", -1,
              hype_usb_inventory_next_unclaimed_class(&inv, HYPE_USB_CLASS_HID, 2));

    /* A claimed device is skipped -- that is how #217 avoids taking the boot medium. */
    hype_usb_inventory_claim(&inv, 1, HYPE_USB_OWNER_HYPE);
    CHECK_HEX("claimed HID skipped", 2,
              hype_usb_inventory_next_unclaimed_class(&inv, HYPE_USB_CLASS_HID, -1));
    CHECK_HEX("one owned by hype", 1,
              (int)hype_usb_inventory_count_owner(&inv, HYPE_USB_OWNER_HYPE));
    CHECK_HEX("two still free", 2,
              (int)hype_usb_inventory_count_owner(&inv, HYPE_USB_OWNER_NONE));
}

static void test_inventory_overflow_is_counted_not_silent(void) {
    hype_usb_inventory_t inv;
    unsigned i;

    hype_usb_inventory_reset(&inv);
    for (i = 0; i < HYPE_USB_INVENTORY_MAX + 3u; i++) {
        hype_usb_devinfo_t d = mk_dev(0u, i + 1u, 0u, 0x1u, HYPE_USB_CLASS_HID);
        int r = hype_usb_inventory_add(&inv, &d);
        if (i < HYPE_USB_INVENTORY_MAX) {
            CHECK_HEX("in-capacity add succeeds", (int)i, r);
        } else {
            CHECK_HEX("over-capacity add refused", -1, r);
        }
    }
    CHECK_HEX("count capped", (int)HYPE_USB_INVENTORY_MAX, (int)inv.count);
    /* A truncating inventory that said nothing would report a complete topology
     * while hiding ports -- worse than admitting it. */
    CHECK_HEX("overflow counted", 3, (int)inv.overflow);
}

static void test_inventory_null_safe(void) {
    hype_usb_devinfo_t d = mk_dev(0u, 1u, 0u, 1u, HYPE_USB_CLASS_HID);
    hype_usb_inventory_t inv;
    hype_usb_inventory_reset(0);
    CHECK_HEX("add to NULL", -1, hype_usb_inventory_add(0, &d));
    hype_usb_inventory_reset(&inv);
    CHECK_HEX("add NULL dev", -1, hype_usb_inventory_add(&inv, 0));
    CHECK_HEX("find in NULL", -1, hype_usb_inventory_find(0, 0u, 1u, 0u));
    CHECK_HEX("class scan in NULL", -1, hype_usb_inventory_next_unclaimed_class(0, 3u, -1));
    CHECK_HEX("count in NULL", 0, (int)hype_usb_inventory_count_owner(0, HYPE_USB_OWNER_HYPE));
    hype_usb_inventory_claim(0, 0, HYPE_USB_OWNER_HYPE);      /* must not crash */
    hype_usb_inventory_claim(&inv, -1, HYPE_USB_OWNER_HYPE);  /* out of range */
    hype_usb_inventory_claim(&inv, 99, HYPE_USB_OWNER_HYPE);
}


static void test_inventory_owner_strings_and_explicit_owner(void) {
    hype_usb_inventory_t inv;
    hype_usb_devinfo_t d = mk_dev(0u, 1u, 0u, 1u, HYPE_USB_CLASS_HID);

    CHECK_HEX("free", 1, hype_usb_owner_str(HYPE_USB_OWNER_NONE)[0] == 'f');
    CHECK_HEX("hype", 1, hype_usb_owner_str(HYPE_USB_OWNER_HYPE)[0] == 'h');
    CHECK_HEX("guest", 1, hype_usb_owner_str(HYPE_USB_OWNER_GUEST)[0] == 'g');
    CHECK_HEX("unknown owner", 1, hype_usb_owner_str((hype_usb_owner_t)99)[0] == '?');

    /* An add that arrives ALREADY owned must set that owner, not be treated as the
     * "do not un-claim" case -- the guard exists to protect an existing claim from a
     * neutral re-scan, not to ignore a deliberate one. */
    hype_usb_inventory_reset(&inv);
    (void)hype_usb_inventory_add(&inv, &d);
    hype_usb_inventory_claim(&inv, 0, HYPE_USB_OWNER_HYPE);
    d.owner = (uint8_t)HYPE_USB_OWNER_GUEST;
    (void)hype_usb_inventory_add(&inv, &d);
    CHECK_HEX("explicit new owner wins over the old claim", (int)HYPE_USB_OWNER_GUEST,
              (int)inv.dev[0].owner);
}


/* #241: composite devices report bDeviceClass 0 and put the class in the interface
 * descriptor. Measured on QEMU: a usb-kbd and a usb-storage both inventoried as
 * class 00/00/00, so a HID lookup found neither. */
static void test_first_iface_class(void) {
    /* config(9) + interface(9) -- HID boot keyboard. */
    static const uint8_t cfg_kbd[] = {
        9, HYPE_USB_DESC_CONFIG, 18, 0, 1, 1, 0, 0xA0, 50,
        9, HYPE_USB_DESC_INTERFACE, 0, 0, 1, HYPE_USB_CLASS_HID, HYPE_USB_SUBCLASS_BOOT,
        HYPE_USB_PROTO_KEYBOARD, 0
    };
    uint8_t c = 0xFF, sc = 0xFF, pr = 0xFF;

    CHECK_HEX("found an interface", 1,
              hype_usb_first_iface_class(cfg_kbd, (unsigned)sizeof(cfg_kbd), &c, &sc, &pr));
    CHECK_HEX("HID class", HYPE_USB_CLASS_HID, c);
    CHECK_HEX("boot subclass", HYPE_USB_SUBCLASS_BOOT, sc);
    CHECK_HEX("keyboard protocol", HYPE_USB_PROTO_KEYBOARD, pr);

    /* Skips non-interface descriptors to reach the interface. */
    {
        static const uint8_t cfg_skip[] = {
            9, HYPE_USB_DESC_CONFIG, 25, 0, 1, 1, 0, 0xA0, 50,
            7, 0x0B, 0, 2, 0, 0, 0,                          /* interface association */
            9, HYPE_USB_DESC_INTERFACE, 0, 0, 2, HYPE_USB_CLASS_MSC, HYPE_USB_SUBCLASS_SCSI,
            HYPE_USB_PROTO_BOT, 0
        };
        c = 0;
        CHECK_HEX("skips to the interface", 1,
                  hype_usb_first_iface_class(cfg_skip, (unsigned)sizeof(cfg_skip), &c, 0, 0));
        CHECK_HEX("MSC class", HYPE_USB_CLASS_MSC, c);
    }

    /* No interface descriptor at all. */
    {
        static const uint8_t cfg_none[] = {9, HYPE_USB_DESC_CONFIG, 9, 0, 1, 1, 0, 0xA0, 50};
        CHECK_HEX("no interface -> 0", 0,
                  hype_usb_first_iface_class(cfg_none, (unsigned)sizeof(cfg_none), &c, 0, 0));
    }

    /*
     * Malformed input must TERMINATE. The buffer is device-supplied, so a zero-length
     * descriptor is a hostile-input case, not a theoretical one: without the guard the
     * walk advances by 0 and loops forever inside hype during host enumeration.
     */
    {
        static const uint8_t cfg_zero[] = {0, HYPE_USB_DESC_INTERFACE, 0, 0, 0, 3, 1, 1, 0};
        CHECK_HEX("zero-length descriptor refused, does not hang", 0,
                  hype_usb_first_iface_class(cfg_zero, (unsigned)sizeof(cfg_zero), &c, 0, 0));
    }
    /* A descriptor claiming to run past the buffer end. */
    {
        static const uint8_t cfg_over[] = {200, HYPE_USB_DESC_INTERFACE, 0, 0, 0, 3, 1, 1, 0};
        CHECK_HEX("overlong descriptor refused", 0,
                  hype_usb_first_iface_class(cfg_over, (unsigned)sizeof(cfg_over), &c, 0, 0));
    }
    /* An interface descriptor too short to hold the class triple. */
    {
        static const uint8_t cfg_short[] = {4, HYPE_USB_DESC_INTERFACE, 0, 0};
        CHECK_HEX("short interface refused", 0,
                  hype_usb_first_iface_class(cfg_short, (unsigned)sizeof(cfg_short), &c, 0, 0));
    }
    CHECK_HEX("NULL cfg", 0, hype_usb_first_iface_class(0, 9u, &c, 0, 0));
}


/* #217: interrupt-endpoint Interval encoding. The two speed families encode
 * DIFFERENTLY and conflating them is the easy mistake -- and getting it wrong yields an
 * endpoint that configures cleanly and never reports, which is indistinguishable from
 * a dead keyboard. */
static void test_interval_encode(void) {
    /* High speed and above: bInterval is already an exponent -> Interval = bInterval-1. */
    CHECK_HEX("HS bInterval 10 -> 9", 9, hype_xhci_interval_encode(HYPE_USB_SPEED_HIGH, 10));
    CHECK_HEX("HS bInterval 1 -> 0", 0, hype_xhci_interval_encode(HYPE_USB_SPEED_HIGH, 1));
    CHECK_HEX("SS bInterval 4 -> 3", 3, hype_xhci_interval_encode(4u, 4));
    /* Clamped to the field's legal range rather than wrapping. */
    CHECK_HEX("HS oversized clamps to 15", 15, hype_xhci_interval_encode(HYPE_USB_SPEED_HIGH, 99));
    CHECK_HEX("HS zero -> 0", 0, hype_xhci_interval_encode(HYPE_USB_SPEED_HIGH, 0));

    /* Full/low speed: bInterval is a FRAME COUNT -> log2 + 3. */
    CHECK_HEX("FS 1 frame -> 3", 3, hype_xhci_interval_encode(HYPE_USB_SPEED_FULL, 1));
    CHECK_HEX("FS 8 frames -> 6", 6, hype_xhci_interval_encode(HYPE_USB_SPEED_FULL, 8));
    CHECK_HEX("LS 10 frames -> 6", 6, hype_xhci_interval_encode(HYPE_USB_SPEED_LOW, 10));
    CHECK_HEX("FS 128 frames -> 10", 10, hype_xhci_interval_encode(HYPE_USB_SPEED_FULL, 128));
    CHECK_HEX("FS 255 frames clamps to 10", 10, hype_xhci_interval_encode(HYPE_USB_SPEED_FULL, 255));
    /* An illegal 0 must not reach log2(0) -- polling too fast still works, never
     * polling does not. */
    CHECK_HEX("FS zero -> fastest legal", 3, hype_xhci_interval_encode(HYPE_USB_SPEED_FULL, 0));

    /* The same bInterval means different things at different speeds -- the assertion
     * that a single shared formula would fail. */
    if (hype_xhci_interval_encode(HYPE_USB_SPEED_HIGH, 8) ==
        hype_xhci_interval_encode(HYPE_USB_SPEED_FULL, 8)) {
        printf("FAIL: HS and FS bInterval 8 must not encode identically\n");
        failures++;
    }
}

static void test_ep_ctx_interval_field(void) {
    uint32_t ep[8];
    hype_xhci_ep_ctx_interval(ep, HYPE_XHCI_EP_TYPE_INT_IN, 8u, 0x1000u, 1, 9u);
    CHECK_HEX("Interval lands in dword0 bits 23:16", 9u, (ep[0] >> 16) & 0xFFu);
    /* And must not disturb what the bulk builder already set. */
    CHECK_HEX("EP type preserved", HYPE_XHCI_EP_TYPE_INT_IN, (ep[1] >> 3) & 0x7u);
    CHECK_HEX("max packet preserved", 8u, (ep[1] >> 16) & 0xFFFFu);
    CHECK_HEX("dequeue ptr preserved", 0x1001u, ep[2]);
}

/* --- USB-7 (#241): endpoint-set collection --- */

static void test_collect_endpoints_walks_all_interfaces(void) {
    /* A composite device: two interfaces, three endpoints between them. All three belong
     * to the DEVICE, so a passthrough entry must list all three -- stopping at the first
     * interface would hand a guest a device missing half its endpoints. */
    static const uint8_t cfg[] = {
        9, 0x02, 0, 0, 2, 1, 0, 0x80, 50,          /* configuration */
        9, 0x04, 0, 0, 2, 0x08, 0x06, 0x50, 0,     /* interface 0: MSC */
        7, 0x05, 0x81, 0x02, 0x00, 0x02, 0,        /* bulk IN, mps 512 */
        7, 0x05, 0x02, 0x02, 0x00, 0x02, 0,        /* bulk OUT, mps 512 */
        9, 0x04, 1, 0, 1, 0x03, 0x01, 0x01, 0,     /* interface 1: HID keyboard */
        7, 0x05, 0x83, 0x03, 0x08, 0x00, 10,       /* interrupt IN, mps 8, interval 10 */
    };
    hype_usb_ep_t eps[HYPE_USB_MAX_ENDPOINTS];
    unsigned int n = hype_usb_collect_endpoints(cfg, (unsigned)sizeof(cfg), eps,
                                                HYPE_USB_MAX_ENDPOINTS);

    CHECK_HEX("three endpoints across two interfaces", 3u, n);
    CHECK_HEX("ep0 addr", 0x81u, eps[0].addr);
    CHECK_HEX("ep0 is bulk", 0x02u, eps[0].attributes & 0x03u);
    CHECK_HEX("ep0 mps", 512u, eps[0].mps);
    CHECK_HEX("ep1 addr", 0x02u, eps[1].addr);
    CHECK_HEX("ep2 addr", 0x83u, eps[2].addr);
    CHECK_HEX("ep2 is interrupt", 0x03u, eps[2].attributes & 0x03u);
    CHECK_HEX("ep2 mps", 8u, eps[2].mps);
    CHECK_HEX("ep2 interval", 10u, eps[2].interval);
}

static void test_collect_endpoints_stops_at_capacity(void) {
    /* Reported short rather than overflowing into whatever follows the array. */
    static const uint8_t cfg[] = {
        9, 0x02, 0, 0, 1, 1, 0, 0x80, 50,
        9, 0x04, 0, 0, 3, 0xFF, 0, 0, 0,
        7, 0x05, 0x81, 0x02, 0x40, 0x00, 0,
        7, 0x05, 0x82, 0x02, 0x40, 0x00, 0,
        7, 0x05, 0x83, 0x02, 0x40, 0x00, 0,
    };
    hype_usb_ep_t eps[2];

    CHECK_HEX("capped at the caller's capacity", 2u,
              hype_usb_collect_endpoints(cfg, (unsigned)sizeof(cfg), eps, 2u));
    CHECK_HEX("first two are the first two", 0x81u, eps[0].addr);
    CHECK_HEX("second", 0x82u, eps[1].addr);
}

static void test_collect_endpoints_rejects_bad_input(void) {
    hype_usb_ep_t eps[4];
    /* A zero descriptor length would spin forever if trusted. */
    static const uint8_t bad[] = {0, 0x02, 0, 0};
    static const uint8_t overlong[] = {9, 0x02, 0, 0, 1, 1, 0, 0x80, 50, 40, 0x05, 0x81};

    CHECK_HEX("NULL cfg", 0u, hype_usb_collect_endpoints(0, 10u, eps, 4u));
    CHECK_HEX("NULL out", 0u, hype_usb_collect_endpoints(bad, 4u, 0, 4u));
    CHECK_HEX("zero-length descriptor ends the walk", 0u,
              hype_usb_collect_endpoints(bad, (unsigned)sizeof(bad), eps, 4u));
    CHECK_HEX("descriptor claiming to run past the buffer ends the walk", 0u,
              hype_usb_collect_endpoints(overlong, (unsigned)sizeof(overlong), eps, 4u));
}

int main(void) {
    test_collect_endpoints_walks_all_interfaces();
    test_collect_endpoints_stops_at_capacity();
    test_collect_endpoints_rejects_bad_input();
    test_interval_encode();
    test_ep_ctx_interval_field();
    test_first_iface_class();
    test_inventory_add_and_find();
    test_inventory_dedupes_by_position_not_identity();
    test_inventory_update_cannot_unclaim();
    test_inventory_next_unclaimed_class();
    test_inventory_overflow_is_counted_not_silent();
    test_inventory_null_safe();
    test_inventory_owner_strings_and_explicit_owner();
    test_recovery_trbs();
    test_reg_offsets();
    test_context_encoders();
    test_hub_helpers();
    test_msc_config_parse();
    test_cap_fields();
    test_cmd_trbs();
    test_control_transfer_trbs();
    test_event_decode();

    test_parked_events();
    test_parked_drop_exact();
    test_exact_transfer_result();
    test_parked_no_duplicates();
    test_parked_overflow_keeps_newest();
    test_parked_drop_slot();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
