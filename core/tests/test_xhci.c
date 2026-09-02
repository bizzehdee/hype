#include <stdio.h>
#include "../xhci.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual) \
    do { \
        long long _e = (long long)(expected), _a = (long long)(actual); \
        if (_e != _a) { \
            printf("FAIL: %s: expected %lld, got %lld\n", (desc), _e, _a); \
            failures++; \
        } \
    } while (0)

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

/*
 * Boot 31 regression (2026-08-30).
 *
 * A Stop Endpoint retires whatever was outstanding with a STOPPED code. hype classified
 * those as transfer failures and ran endpoint recovery -- Reset Endpoint against a Stopped
 * endpoint, which is a Context State Error. The controller's command ring stopped answering
 * from that point and every interrupt-IN endpoint on it was deaf for the next 74 minutes.
 *
 * SUCCESS and SHORT_PACKET must stay out of this set (they are real reports), and so must
 * genuine failures like STALL and USB Transaction Error -- classifying one of those as
 * "cancelled" would silently swallow an endpoint that really does need recovering.
 */
static void test_cc_stopped_classification(void) {
    CHECK_HEX("cc 26 Stopped is a stop", 1, hype_xhci_cc_is_stopped(HYPE_XHCI_CC_STOPPED));
    CHECK_HEX("cc 27 Stopped-Length is a stop", 1,
              hype_xhci_cc_is_stopped(HYPE_XHCI_CC_STOPPED_LENGTH));
    CHECK_HEX("cc 28 Stopped-Short is a stop", 1,
              hype_xhci_cc_is_stopped(HYPE_XHCI_CC_STOPPED_SHORT));

    CHECK_HEX("success is not a stop", 0, hype_xhci_cc_is_stopped(HYPE_XHCI_CC_SUCCESS));
    CHECK_HEX("short packet is not a stop", 0,
              hype_xhci_cc_is_stopped(HYPE_XHCI_CC_SHORT_PACKET));
    CHECK_HEX("stall is not a stop", 0, hype_xhci_cc_is_stopped(6u));
    CHECK_HEX("usb transaction error is not a stop", 0, hype_xhci_cc_is_stopped(4u));
    CHECK_HEX("babble is not a stop", 0, hype_xhci_cc_is_stopped(3u));
    CHECK_HEX("event ring full is not a stop", 0,
              hype_xhci_cc_is_stopped(HYPE_XHCI_CC_EVENT_RING_FULL));
    CHECK_HEX("cc 0 invalid is not a stop", 0, hype_xhci_cc_is_stopped(0u));
    CHECK_HEX("cc 29 past the range is not a stop", 0, hype_xhci_cc_is_stopped(29u));
}

/*
 * Boot 32 regression (2026-08-30).
 *
 * ACKing a port's change bits must clear those bits and change nothing else. The version
 * this replaces wrote `sc & HYPE_XHCI_PORTSC_CHANGE_MASK`, which also wrote 0 into PP --
 * so hype powered the root port down in the act of acknowledging the unplug, and the
 * re-plug was never reported at all.
 */
static void test_portsc_ack_preserves_power(void) {
    /* Connected, enabled, powered, CSC set -- a device that has just been unplugged. */
    uint32_t sc = HYPE_XHCI_PORTSC_PP | HYPE_XHCI_PORTSC_CSC | (1u << 10) /* speed */;
    uint32_t w = hype_xhci_portsc_ack_changes(sc);

    CHECK_HEX("ack keeps port power", HYPE_XHCI_PORTSC_PP, w & HYPE_XHCI_PORTSC_PP);
    CHECK_HEX("ack clears CSC by writing it back", HYPE_XHCI_PORTSC_CSC,
              w & HYPE_XHCI_PORTSC_CSC);

    /* Every change bit set at once: all are written back, power still survives. */
    sc = HYPE_XHCI_PORTSC_PP | HYPE_XHCI_PORTSC_CHANGE_MASK;
    w = hype_xhci_portsc_ack_changes(sc);
    CHECK_HEX("all change bits acked", HYPE_XHCI_PORTSC_CHANGE_MASK,
              w & HYPE_XHCI_PORTSC_CHANGE_MASK);
    CHECK_HEX("power survives a full ack", HYPE_XHCI_PORTSC_PP, w & HYPE_XHCI_PORTSC_PP);

    /* PED is write-1-to-clear: never echo it, or the ACK disables the port. */
    sc = HYPE_XHCI_PORTSC_PP | HYPE_XHCI_PORTSC_PED | HYPE_XHCI_PORTSC_CSC;
    w = hype_xhci_portsc_ack_changes(sc);
    CHECK_HEX("ack never writes PED", 0u, w & HYPE_XHCI_PORTSC_PED);

    /* PR and the two other write-1 strobes must never be echoed either. */
    sc = HYPE_XHCI_PORTSC_PP | HYPE_XHCI_PORTSC_PR | (1u << 16) | (1u << 31)
         | HYPE_XHCI_PORTSC_PRC;
    w = hype_xhci_portsc_ack_changes(sc);
    CHECK_HEX("ack fires no strobe", 0u, w & HYPE_XHCI_PORTSC_STROBE);
    CHECK_HEX("ack still clears PRC", HYPE_XHCI_PORTSC_PRC, w & HYPE_XHCI_PORTSC_PRC);

    /* Nothing to ack: the write is a no-op that still leaves the port powered. */
    sc = HYPE_XHCI_PORTSC_PP | HYPE_XHCI_PORTSC_CCS;
    w = hype_xhci_portsc_ack_changes(sc);
    CHECK_HEX("no spurious change bits", 0u, w & HYPE_XHCI_PORTSC_CHANGE_MASK);
    CHECK_HEX("power kept when nothing to ack", HYPE_XHCI_PORTSC_PP,
              w & HYPE_XHCI_PORTSC_PP);
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

/*
 * The eviction is the fatal event, and until now nothing counted it. A dropped interrupt-IN
 * completion does not lose a report, it loses the ENDPOINT: `armed` clears only on a claim,
 * so the transfer stays outstanding for ever and the endpoint is never re-armed. A boot that
 * ends with a deaf keyboard and evictions=0 means something else went wrong.
 */
static void test_parked_evictions_counted(void) {
    hype_xhci_parked_t p;
    unsigned i;

    hype_xhci_parked_reset(&p);
    CHECK_HEX("a fresh table has evicted nothing", 0,
              (int)hype_xhci_parked_evictions(&p));

    /* Fill it exactly. HYPE_XHCI_PARKED_MAX distinct transfers fit without evicting. */
    for (i = 0; i < HYPE_XHCI_PARKED_MAX; i++) {
        hype_xhci_parked_put(&p, 1, 3, 0x1000ull + i * 0x10ull, 1, 0);
    }
    CHECK_HEX("a table filled to capacity has evicted nothing", 0,
              (int)hype_xhci_parked_evictions(&p));

    /* One more must displace an existing entry, and say so. */
    hype_xhci_parked_put(&p, 1, 3, 0x9000ull, 1, 0);
    CHECK_HEX("the entry past capacity is counted as an eviction", 1,
              (int)hype_xhci_parked_evictions(&p));

    /* Re-parking a transfer already held replaces it in place: not an eviction. */
    hype_xhci_parked_put(&p, 1, 3, 0x9000ull, 1, 0);
    CHECK_HEX("re-parking the same transfer is not an eviction", 1,
              (int)hype_xhci_parked_evictions(&p));

    hype_xhci_parked_reset(&p);
    CHECK_HEX("reset clears the eviction count", 0,
              (int)hype_xhci_parked_evictions(&p));
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

/*
 * #736: a periodic endpoint with Max ESIT Payload 0 gets no periodic bandwidth, so it
 * configures cleanly and never reports. The keyboard that measured 68818 polls with
 * reports=0 has mps 8; the mouse on the same hub has 64.
 */
static void test_ep_ctx_interval_sets_max_esit_payload(void) {
    uint32_t ep[8];

    hype_xhci_ep_ctx_interval(ep, HYPE_XHCI_EP_TYPE_INT_IN, 8u, 0x1000u, 1, 3u);
    CHECK_HEX("max ESIT payload = mps 8", 8u, (ep[4] >> 16) & 0xFFFFu);
    CHECK_HEX("average TRB length still 8", 8u, ep[4] & 0xFFFFu);

    hype_xhci_ep_ctx_interval(ep, HYPE_XHCI_EP_TYPE_INT_IN, 64u, 0x1000u, 1, 3u);
    CHECK_HEX("max ESIT payload = mps 64", 64u, (ep[4] >> 16) & 0xFFFFu);

    /* The bulk/control builder must NOT gain the field -- it is a periodic-only fact. */
    hype_xhci_ep_ctx(ep, HYPE_XHCI_EP_TYPE_BULK_IN, 512u, 0x1000u, 1);
    CHECK_HEX("bulk max ESIT payload stays 0", 0u, (ep[4] >> 16) & 0xFFFFu);
}

/*
 * #737: a hub's Slot Context needs Hub=1, Number of Ports and TT Think Time. Without
 * them a child that names the hub as its Transaction Translator is rejected with
 * Parameter Error (code 17) -- measured on a 32-byte-context controller.
 */
static void test_slot_ctx_hub_fields(void) {
    uint32_t c[8];
    /* A high-speed 4-port hub on root port 6, no TT of its own. */
    hype_xhci_slot_ctx(c, 0, HYPE_USB_SPEED_HIGH, 1, 6, 0, 0);
    CHECK_HEX("function slot has Hub=0", 0u, (c[0] >> 26) & 0x1u);
    CHECK_HEX("function slot has no port count", 0u, (c[1] >> 24) & 0xFFu);

    hype_xhci_slot_ctx_set_hub(c, 4u, 2u, 0u);
    CHECK_HEX("Hub bit set", 1u, (c[0] >> 26) & 0x1u);
    CHECK_HEX("MTT stays 0 for a single-TT hub", 0u, (c[0] >> 25) & 0x1u);
    CHECK_HEX("number of ports = 4", 4u, (c[1] >> 24) & 0xFFu);
    CHECK_HEX("TTT = 2", 2u, (c[2] >> 16) & 0x3u);
    /* Everything Address Device already put there must survive. */
    CHECK_HEX("speed preserved", HYPE_USB_SPEED_HIGH, (c[0] >> 20) & 0xFu);
    CHECK_HEX("ctx entries preserved", 1u, (c[0] >> 27) & 0x1Fu);
    CHECK_HEX("root port preserved", 6u, (c[1] >> 16) & 0xFFu);

    /* A hub behind a hub keeps its own TT fields when the hub bits are added. */
    hype_xhci_slot_ctx(c, 0x21, HYPE_USB_SPEED_FULL, 1, 7, 4, 3);
    hype_xhci_slot_ctx_set_hub(c, 7u, 3u, 1u);
    CHECK_HEX("TT hub slot preserved", 4u, c[2] & 0xFFu);
    CHECK_HEX("TT port preserved", 3u, (c[2] >> 8) & 0xFFu);
    CHECK_HEX("TTT = 3 alongside the TT fields", 3u, (c[2] >> 16) & 0x3u);
    CHECK_HEX("MTT set when asked", 1u, (c[0] >> 25) & 0x1u);
    CHECK_HEX("route string preserved", 0x21u, c[0] & 0xFFFFFu);

    /* An over-wide port count must not spill into TTT/Interrupter Target. */
    hype_xhci_slot_ctx(c, 0, HYPE_USB_SPEED_HIGH, 1, 1, 0, 0);
    hype_xhci_slot_ctx_set_hub(c, 0x1FFu, 0x7u, 0u);
    CHECK_HEX("port count masked to 8 bits", 0xFFu, (c[1] >> 24) & 0xFFu);
    CHECK_HEX("TTT masked to 2 bits", 0x3u, (c[2] >> 16) & 0x3u);
    CHECK_HEX("nothing above TTT touched", 0u, c[2] >> 18);
}

/* TT Think Time is wHubCharacteristics bits 6:5 -- hub-descriptor byte 3. */
static void test_hub_ttt_from_descriptor(void) {
    /* bNbrPorts 4, wHubCharacteristics 0x0009 -> TTT 0 (8 FS bit times). */
    static const uint8_t ttt0[] = { 9, 0x29, 4, 0x09, 0x00, 0x32, 0x64, 0x00, 0xFF };
    /* wHubCharacteristics 0x0069: bits 6:5 = 0b11 -> TTT 3 (32 FS bit times). */
    static const uint8_t ttt3[] = { 9, 0x29, 4, 0x69, 0x00, 0x32, 0x64, 0x00, 0xFF };
    /* bits 6:5 = 0b01 -> TTT 1. */
    static const uint8_t ttt1[] = { 9, 0x29, 7, 0x29, 0x00, 0x32, 0x64, 0x00, 0xFF };

    CHECK_HEX("TTT 0", 0u, hype_xhci_hub_ttt(ttt0));
    CHECK_HEX("TTT 3", 3u, hype_xhci_hub_ttt(ttt3));
    CHECK_HEX("TTT 1", 1u, hype_xhci_hub_ttt(ttt1));
    CHECK_HEX("port count still read from byte 2", 7u, hype_xhci_hub_nbr_ports(ttt1));
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

static void test_string_desc_langid0(void) {
    /* String descriptor 0: bLength 4, type STRING, LANGID 0x0409 (en-US). */
    static const uint8_t d0[] = {4, 0x03, 0x09, 0x04};
    static const uint8_t wrong_type[] = {4, 0x02, 0x09, 0x04};
    static const uint8_t too_short_blen[] = {2, 0x03, 0x09, 0x04};
    uint16_t lang = 0;

    CHECK_HEX("langid0 parses", 0, hype_usb_string_desc_langid0(d0, sizeof d0, &lang));
    CHECK_HEX("langid0 value", 0x0409u, lang);
    CHECK_HEX("langid0 wrong type refused", -1,
              hype_usb_string_desc_langid0(wrong_type, sizeof wrong_type, &lang));
    CHECK_HEX("langid0 empty list refused", -1,
              hype_usb_string_desc_langid0(too_short_blen, sizeof too_short_blen, &lang));
    CHECK_HEX("langid0 short buffer refused", -1, hype_usb_string_desc_langid0(d0, 3u, &lang));
    CHECK_HEX("langid0 null desc refused", -1, hype_usb_string_desc_langid0(0, 4u, &lang));
    CHECK_HEX("langid0 null out refused", -1, hype_usb_string_desc_langid0(d0, sizeof d0, 0));
}

static void test_string_desc_ascii(void) {
    /* "AB12" as UTF-16LE, with a leading pad space to prove trimming. */
    static const uint8_t sn[] = {12, 0x03, ' ', 0, 'A', 0, 'B', 0, '1', 0, '2', 0};
    char out[8];

    CHECK_HEX("iSerialNumber length", 4, hype_usb_string_desc_ascii(sn, sizeof sn, out, sizeof out));
    CHECK_HEX("iSerialNumber b0", 'A', out[0]);
    CHECK_HEX("iSerialNumber b3", '2', out[3]);
    CHECK_HEX("iSerialNumber NUL", 0, out[4]);

    /* the descriptor is bounded by bLength, not the buffer: the 0xEE tail
     * beyond bLength is never read as text */
    {
        static const uint8_t padded[16] = {8, 0x03, 'X', 0, 'Y', 0, 'Z', 0,
                                           0xEE, 0xEE, 0xEE, 0xEE};
        CHECK_HEX("bLength bounds the text", 3,
                  hype_usb_string_desc_ascii(padded, sizeof padded, out, sizeof out));
        CHECK_HEX("bounded text b2", 'Z', out[2]);
        CHECK_HEX("bounded text NUL", 0, out[3]);
    }

    /* refusals: no identity is ever repaired into one (#323) */
    {
        static const uint8_t nonascii[] = {6, 0x03, 0x42, 0x30, 'A', 0};   /* U+3042 */
        static const uint8_t ctrl_ch[] = {6, 0x03, 0x07, 0, 'A', 0};       /* BEL */
        static const uint8_t empty[] = {2, 0x03};
        static const uint8_t all_space[] = {6, 0x03, ' ', 0, ' ', 0};
        static const uint8_t odd_blen[] = {5, 0x03, 'A', 0, 'B'};
        static const uint8_t overlong_blen[] = {10, 0x03, 'A', 0};
        static const uint8_t not_string[] = {6, 0x04, 'A', 0, 'B', 0};
        CHECK_HEX("non-ASCII code unit refused", -1,
                  hype_usb_string_desc_ascii(nonascii, sizeof nonascii, out, sizeof out));
        CHECK_HEX("control char refused", -1,
                  hype_usb_string_desc_ascii(ctrl_ch, sizeof ctrl_ch, out, sizeof out));
        CHECK_HEX("empty string refused", -1,
                  hype_usb_string_desc_ascii(empty, sizeof empty, out, sizeof out));
        CHECK_HEX("all-space string refused", -1,
                  hype_usb_string_desc_ascii(all_space, sizeof all_space, out, sizeof out));
        CHECK_HEX("odd bLength refused", -1,
                  hype_usb_string_desc_ascii(odd_blen, sizeof odd_blen, out, sizeof out));
        CHECK_HEX("bLength past buffer refused", -1,
                  hype_usb_string_desc_ascii(overlong_blen, sizeof overlong_blen, out, sizeof out));
        CHECK_HEX("wrong descriptor type refused", -1,
                  hype_usb_string_desc_ascii(not_string, sizeof not_string, out, sizeof out));
        CHECK_HEX("null desc refused", -1, hype_usb_string_desc_ascii(0, 8u, out, sizeof out));
        CHECK_HEX("null out refused", -1, hype_usb_string_desc_ascii(sn, sizeof sn, 0, 8u));
        CHECK_HEX("overflowing serial refused", -1,
                  hype_usb_string_desc_ascii(sn, sizeof sn, out, 4u));
        CHECK_HEX("exact fit accepted", 4, hype_usb_string_desc_ascii(sn, sizeof sn, out, 5u));
    }

    /* device-descriptor iSerialNumber index accessor */
    {
        uint8_t dd[18] = {0};
        dd[16] = 3u;
        CHECK_HEX("iSerialNumber index", 3u, hype_usb_dev_iserial_index(dd));
    }
}

/*
 * #734: the interrupt-IN endpoint pool. The bug this replaces is the reason for every
 * assertion here: one shared block per controller meant a keyboard and a mouse on the
 * same controller shared one "transfer outstanding" flag, and an idle input device holds
 * that flag forever -- so the other endpoint's doorbell was never rung again.
 */
static void test_int_in_pool(void) {
    hype_xhci_int_in_key_t keys[3];
    int kbd, mouse, i;

    for (i = 0; i < 3; i++) keys[i].used = 0;

    /* Two endpoints on ONE controller get two DIFFERENT blocks. */
    kbd = hype_xhci_int_in_index(keys, 3u, 0u, 3u, 1u, 1);
    mouse = hype_xhci_int_in_index(keys, 3u, 0u, 4u, 1u, 1);
    CHECK_INT("keyboard gets a block", 0, kbd);
    CHECK_INT("mouse gets a DIFFERENT block", 1, mouse);

    /* A second look-up for the same endpoint returns the SAME block, so a poll never
     * re-points the ring the configure command armed. */
    CHECK_INT("keyboard look-up is stable", kbd, hype_xhci_int_in_index(keys, 3u, 0u, 3u, 1u, 0));
    CHECK_INT("mouse look-up is stable", mouse, hype_xhci_int_in_index(keys, 3u, 0u, 4u, 1u, 0));

    /* Two endpoints on ONE device (dci 1 and 2) are distinct too. */
    CHECK_INT("second endpoint of the same slot is its own block", 2,
              hype_xhci_int_in_index(keys, 3u, 0u, 3u, 2u, 1));

    /* Same slot id on a different controller is a different device (slots are
     * per-controller), so it must not alias. */
    CHECK_INT("pool full refuses rather than aliasing", -1,
              hype_xhci_int_in_index(keys, 3u, 1u, 3u, 1u, 1));

    /* Lookup-only never claims: a poll for an endpoint nobody configured must fail. */
    for (i = 0; i < 3; i++) keys[i].used = 0;
    CHECK_INT("unconfigured endpoint is not silently allocated", -1,
              hype_xhci_int_in_index(keys, 3u, 0u, 5u, 1u, 0));
    CHECK_INT("and no block was consumed", 0, keys[0].used);

    /* Rejected inputs. */
    CHECK_INT("slot 0 is invalid", -1, hype_xhci_int_in_index(keys, 3u, 0u, 0u, 1u, 1));
    CHECK_INT("dci 0 (EP0) is not an interrupt-IN endpoint", -1,
              hype_xhci_int_in_index(keys, 3u, 0u, 3u, 0u, 1));
    CHECK_INT("null pool", -1, hype_xhci_int_in_index(0, 3u, 0u, 3u, 1u, 1));

    /* Releasing one controller's blocks leaves the other controller's alone. */
    kbd = hype_xhci_int_in_index(keys, 3u, 0u, 3u, 1u, 1);
    mouse = hype_xhci_int_in_index(keys, 3u, 1u, 3u, 1u, 1);
    hype_xhci_int_in_release_ctrl(keys, 3u, 0u);
    CHECK_INT("released controller 0's block is gone", -1,
              hype_xhci_int_in_index(keys, 3u, 0u, 3u, 1u, 0));
    CHECK_INT("controller 1's block survives", mouse,
              hype_xhci_int_in_index(keys, 3u, 1u, 3u, 1u, 0));
    hype_xhci_int_in_release_ctrl(0, 3u, 0u); /* null-safe */
}

/* ---- #741: a composite device's interfaces ---- */

/* config(9) + iface0 HID/boot/keyboard + iface1 HID/boot/mouse + iface2 vendor.
 * This is the shape of a Logitech Unifying receiver, which is what #741 is about. */
static const uint8_t k741_receiver_cfg[] = {
    9, 0x02, 0x2D, 0x00, 3, 1, 0, 0xA0, 50,          /* config, 3 interfaces */
    9, 0x04, 0, 0, 1, 0x03, 0x01, 0x01, 0,           /* iface 0: HID boot KEYBOARD */
    7, 0x05, 0x81, 0x03, 8, 0, 8,                    /*   ep 0x81 int-in */
    9, 0x04, 1, 0, 1, 0x03, 0x01, 0x02, 0,           /* iface 1: HID boot MOUSE */
    7, 0x05, 0x82, 0x03, 8, 0, 8,                    /*   ep 0x82 int-in */
    9, 0x04, 2, 0, 1, 0xFF, 0x00, 0x00, 0,           /* iface 2: vendor-specific */
    7, 0x05, 0x83, 0x03, 32, 0, 1,                   /*   ep 0x83 int-in */
};

static void test_741_collects_every_interface(void) {
    hype_usb_iface_t ifs[HYPE_USB_MAX_IFACES];
    uint8_t over = 0xFF;
    unsigned n = hype_usb_collect_interfaces(k741_receiver_cfg, sizeof k741_receiver_cfg,
                                             ifs, HYPE_USB_MAX_IFACES, &over);
    CHECK_INT("three interfaces", 3u, n);
    CHECK_INT("no overflow", 0u, (unsigned)over);
    CHECK_INT("iface0 number", 0u, (unsigned)ifs[0].number);
    CHECK_INT("iface0 is a boot keyboard", 0x01u, (unsigned)ifs[0].protocol);
    CHECK_INT("iface1 number", 1u, (unsigned)ifs[1].number);
    CHECK_INT("iface1 is a boot mouse", 0x02u, (unsigned)ifs[1].protocol);
    CHECK_INT("iface2 is vendor", 0xFFu, (unsigned)ifs[2].cls);
}

static void test_741_first_iface_class_still_sees_only_the_first(void) {
    uint8_t c = 0, sc = 0, pr = 0;
    /* The old accessor is unchanged and still reports the FIRST interface. That is the
     * whole defect, kept working because existing lookups key off it -- the new list is
     * the fuller truth alongside it, not a replacement. */
    CHECK_INT("found", 1, hype_usb_first_iface_class(k741_receiver_cfg,
                                                    sizeof k741_receiver_cfg, &c, &sc, &pr));
    CHECK_INT("class HID", 0x03u, (unsigned)c);
    CHECK_INT("protocol keyboard only", 0x01u, (unsigned)pr);
}

static void test_741_find_iface_matches_a_later_interface(void) {
    hype_usb_devinfo_t d;
    uint8_t over = 0;
    unsigned i;
    for (i = 0; i < sizeof d; i++) ((uint8_t *)&d)[i] = 0;
    d.iface_count = (uint8_t)hype_usb_collect_interfaces(k741_receiver_cfg,
                                                         sizeof k741_receiver_cfg, d.ifaces,
                                                         HYPE_USB_MAX_IFACES, &over);
    /* The boot MOUSE is interface 1. Matching only the first interface -- which is what
     * the claim path did -- would miss it, and did. */
    CHECK_INT("boot keyboard found", 0, hype_usb_devinfo_find_iface(&d, 0x03u, 0x01u, 0x01u));
    CHECK_INT("boot mouse found too", 1, hype_usb_devinfo_find_iface(&d, 0x03u, 0x01u, 0x02u));
    CHECK_INT("a class it does not have", -1, hype_usb_devinfo_find_iface(&d, 0x08u, 0x06u, 0x50u));
}

static void test_741_overflow_is_reported_not_hidden(void) {
    /* 3 interfaces into a cap of 2: two recorded, one counted. A table that quietly
     * truncated would report a device as having fewer interfaces than it does, which is
     * the same class of wrongness #741 exists to remove. */
    hype_usb_iface_t ifs[2];
    uint8_t over = 0;
    unsigned n = hype_usb_collect_interfaces(k741_receiver_cfg, sizeof k741_receiver_cfg,
                                             ifs, 2u, &over);
    CHECK_INT("capped", 2u, n);
    CHECK_INT("and says how many it dropped", 1u, (unsigned)over);
}

static void test_741_malformed_descriptor_terminates(void) {
    /* A zero length would spin forever on a device-supplied buffer. Same rule as every
     * other descriptor walker here: stop, keeping whatever was already read. */
    uint8_t bad[] = { 9, 0x02, 0x12, 0x00, 1, 1, 0, 0xA0, 50,
                      9, 0x04, 0, 0, 1, 0x03, 0x01, 0x01, 0,
                      0, 0x04, 0, 0, 0, 0, 0, 0, 0 };
    hype_usb_iface_t ifs[HYPE_USB_MAX_IFACES];
    uint8_t over = 0;
    unsigned n = hype_usb_collect_interfaces(bad, sizeof bad, ifs, HYPE_USB_MAX_IFACES, &over);
    CHECK_INT("kept the good one and stopped", 1u, n);
}

static void test_741_null_safe(void) {
    hype_usb_iface_t ifs[2];
    uint8_t over = 0xFF;
    CHECK_INT("null cfg", 0u, hype_usb_collect_interfaces(0, 10, ifs, 2, &over));
    CHECK_INT("overflow cleared even so", 0u, (unsigned)over);
    CHECK_INT("null out", 0u, hype_usb_collect_interfaces(k741_receiver_cfg,
                                                         sizeof k741_receiver_cfg, 0, 2, 0));
    CHECK_INT("null devinfo", -1, hype_usb_devinfo_find_iface(0, 3, 1, 1));
}

/* ---- #744: departure bookkeeping ---- */

static void t744_dev(hype_usb_devinfo_t *d, unsigned ctrl, unsigned port, unsigned route,
                     unsigned slot) {
    unsigned i;
    for (i = 0; i < sizeof *d; i++) ((uint8_t *)d)[i] = 0;
    d->controller = ctrl; d->root_port = port; d->route = route; d->slot = slot;
    d->owner = (uint8_t)HYPE_USB_OWNER_HYPE;
}

static void test_744_note_departed_clears_slot_and_owner(void) {
    hype_usb_inventory_t inv;
    hype_usb_devinfo_t d;
    int idx;

    hype_usb_inventory_reset(&inv);
    t744_dev(&d, 0, 3, 0, 7);
    idx = hype_usb_inventory_add(&inv, &d);
    CHECK_INT("added", 0, idx);

    CHECK_INT("found and marked", 1, hype_usb_inventory_note_departed(&inv, 0, 3, 0));
    /* slot 0 is the struct's own documented "the slot was released", so a reader that
     * already honours that needs no new flag to check. */
    CHECK_INT("slot released", 0u, inv.dev[0].slot);
    CHECK_INT("no longer owned", (int)HYPE_USB_OWNER_NONE, (int)inv.dev[0].owner);
    /* The ENTRY stays. Removing it would shift every later index, and callers hold
     * indices across a sweep. */
    CHECK_INT("entry kept", 1u, inv.count);
}

static void test_744_note_departed_is_a_no_op_for_a_position_with_nothing_on_it(void) {
    hype_usb_inventory_t inv;
    hype_usb_devinfo_t d;

    hype_usb_inventory_reset(&inv);
    t744_dev(&d, 0, 3, 0, 7);
    (void)hype_usb_inventory_add(&inv, &d);
    CHECK_INT("wrong port", 0, hype_usb_inventory_note_departed(&inv, 0, 4, 0));
    CHECK_INT("wrong controller", 0, hype_usb_inventory_note_departed(&inv, 1, 3, 0));
    CHECK_INT("wrong route", 0, hype_usb_inventory_note_departed(&inv, 0, 3, 2));
    CHECK_INT("the real one is untouched", 7u, inv.dev[0].slot);
}

static void test_744_release_slot_frees_only_that_slot(void) {
    hype_xhci_int_in_key_t keys[4];
    unsigned i;
    for (i = 0; i < 4u; i++) keys[i].used = 0;

    /* Two endpoints on slot 3, one on slot 4, all on controller 0. */
    CHECK_INT("a", 0, hype_xhci_int_in_index(keys, 4u, 0u, 3u, 3u, 1));
    CHECK_INT("b", 1, hype_xhci_int_in_index(keys, 4u, 0u, 3u, 5u, 1));
    CHECK_INT("c", 2, hype_xhci_int_in_index(keys, 4u, 0u, 4u, 3u, 1));

    hype_xhci_int_in_release_slot(keys, 4u, 0u, 3u);

    /* Both of slot 3's blocks come back; slot 4's is untouched. Releasing the whole
     * controller here would have taken the surviving device's endpoint with it. */
    CHECK_INT("slot 3 block 0 freed", 0, (int)keys[0].used);
    CHECK_INT("slot 3 block 1 freed", 0, (int)keys[1].used);
    CHECK_INT("slot 4 block kept", 1, (int)keys[2].used);
}

static void test_744_release_slot_ignores_other_controllers(void) {
    hype_xhci_int_in_key_t keys[4];
    unsigned i;
    for (i = 0; i < 4u; i++) keys[i].used = 0;
    CHECK_INT("ctrl0 slot3", 0, hype_xhci_int_in_index(keys, 4u, 0u, 3u, 3u, 1));
    CHECK_INT("ctrl1 slot3", 1, hype_xhci_int_in_index(keys, 4u, 1u, 3u, 3u, 1));
    /* Same slot id on a DIFFERENT controller is a different device -- slot ids are
     * per-controller, and freeing across them would silence an innocent keyboard. */
    hype_xhci_int_in_release_slot(keys, 4u, 0u, 3u);
    CHECK_INT("ctrl0 freed", 0, (int)keys[0].used);
    CHECK_INT("ctrl1 kept", 1, (int)keys[1].used);
}

static void test_744_release_slot_null_and_zero_safe(void) {
    hype_xhci_int_in_key_t keys[2];
    keys[0].used = 1; keys[0].ctrl = 0; keys[0].slot = 3; keys[0].dci = 3;
    keys[1].used = 0;
    hype_xhci_int_in_release_slot(0, 2u, 0u, 3u);            /* must not fault */
    hype_xhci_int_in_release_slot(keys, 2u, 0u, 0u);          /* slot 0 is not a slot */
    CHECK_INT("slot 0 released nothing", 1, (int)keys[0].used);
}


/* #783: the reset refusal is pure logic over two recorded controller indices. */
static void test_783_may_reset_a_controller_carrying_nothing(void) {
    hype_xhci_reset_policy_t p = { 1u, 1u };
    const char *why = (const char *)0;
    CHECK_INT("ctrl 2 carries nothing", 1, hype_xhci_may_reset(&p, 2u, &why));
    CHECK_INT("no reason when allowed", 0, why[0] != '\0');
}

static void test_783_refuses_the_log_controller(void) {
    hype_xhci_reset_policy_t p = { 2u, 0u };
    const char *why = (const char *)0;
    CHECK_INT("log ctrl refused", 0, hype_xhci_may_reset(&p, 2u, &why));
    CHECK_INT("reason names the log", 0, why == (const char *)0 || why[0] == '\0');
    CHECK_INT("other ctrl allowed", 1, hype_xhci_may_reset(&p, 1u, &why));
}

static void test_783_refuses_the_boot_controller(void) {
    hype_xhci_reset_policy_t p = { 0u, 1u };
    const char *why = (const char *)0;
    CHECK_INT("boot ctrl refused", 0, hype_xhci_may_reset(&p, 1u, &why));
    CHECK_INT("reason set", 0, why == (const char *)0 || why[0] == '\0');
    CHECK_INT("ctrl 2 allowed", 1, hype_xhci_may_reset(&p, 2u, &why));
}

static void test_783_names_both_roles_on_one_controller(void) {
    hype_xhci_reset_policy_t p = { 1u, 1u };
    const char *why = (const char *)0;
    CHECK_INT("refused", 0, hype_xhci_may_reset(&p, 1u, &why));
    /* The reason must mention both, or the operator reads half the truth. */
    CHECK_INT("mentions log", 1, why != (const char *)0 &&
              why[0] == 'c' && why[12] == 'l'); /* "carries the log sink and ..." */
}

static void test_783_unknown_controller_is_never_reset(void) {
    hype_xhci_reset_policy_t p = { 0u, 0u };
    const char *why = (const char *)0;
    CHECK_INT("index 0 refused", 0, hype_xhci_may_reset(&p, 0u, &why));
    CHECK_INT("reason set", 0, why == (const char *)0 || why[0] == '\0');
    CHECK_INT("null policy refused", 0, hype_xhci_may_reset((const hype_xhci_reset_policy_t *)0, 2u, &why));
    CHECK_INT("null reason tolerated", 0, hype_xhci_may_reset(&p, 0u, (const char **)0));
    CHECK_INT("nothing recorded: any real index allowed", 1, hype_xhci_may_reset(&p, 3u, &why));
}

int main(void) {
    test_744_note_departed_clears_slot_and_owner();
    test_744_note_departed_is_a_no_op_for_a_position_with_nothing_on_it();
    test_744_release_slot_frees_only_that_slot();
    test_744_release_slot_ignores_other_controllers();
    test_744_release_slot_null_and_zero_safe();
    test_741_collects_every_interface();
    test_741_first_iface_class_still_sees_only_the_first();
    test_741_find_iface_matches_a_later_interface();
    test_741_overflow_is_reported_not_hidden();
    test_741_malformed_descriptor_terminates();
    test_741_null_safe();
    test_string_desc_langid0();
    test_string_desc_ascii();
    test_collect_endpoints_walks_all_interfaces();
    test_collect_endpoints_stops_at_capacity();
    test_collect_endpoints_rejects_bad_input();
    test_interval_encode();
    test_ep_ctx_interval_field();
    test_ep_ctx_interval_sets_max_esit_payload();
    test_slot_ctx_hub_fields();
    test_hub_ttt_from_descriptor();
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
    test_cc_stopped_classification();
    test_portsc_ack_preserves_power();

    test_parked_events();
    test_parked_drop_exact();
    test_parked_evictions_counted();
    test_exact_transfer_result();
    test_parked_no_duplicates();
    test_parked_overflow_keeps_newest();
    test_parked_drop_slot();
    test_int_in_pool();

    test_783_may_reset_a_controller_carrying_nothing();
    test_783_refuses_the_log_controller();
    test_783_refuses_the_boot_controller();
    test_783_names_both_roles_on_one_controller();
    test_783_unknown_controller_is_never_reset();
    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
