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

    /* route 0, speed 4 (SS), ctx entries 1, root port 3 */
    hype_xhci_slot_ctx(c, 0, 4, 1, 3);
    CHECK_HEX("slot speed field", 4u, (c[0] >> 20) & 0xFu);
    CHECK_HEX("slot ctx entries", 1u, (c[0] >> 27) & 0x1Fu);
    CHECK_HEX("slot root port", 3u, (c[1] >> 16) & 0xFFu);

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

int main(void) {
    test_reg_offsets();
    test_context_encoders();
    test_msc_config_parse();
    test_cap_fields();
    test_cmd_trbs();
    test_control_transfer_trbs();
    test_event_decode();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
