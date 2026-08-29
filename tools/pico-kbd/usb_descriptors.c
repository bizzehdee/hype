/*
 * Descriptors for the #778 synthetic keyboard.
 *
 * The shape matters more than the content: hype must claim this board through the SAME code
 * path it claims the operator's Keychron, or the test proves nothing about that path. So it
 * is a single-interface HID device with the BOOT subclass and the KEYBOARD protocol, one
 * interrupt IN endpoint, 8-byte packets, 1 ms interval -- byte-for-byte the interface shape
 * of a boot keyboard, and what hype's fw_1_claim_boot_hid looks for.
 */
#include "tusb.h"

/*
 * 0xCAFE is TinyUSB's example vendor id. Deliberately NOT a real vendor's: this board is a
 * test fixture that must never be mistaken for a product, and a distinctive pair makes it
 * obvious in hype's inventory dump which device is the robot.
 */
#define HYPE_KBD_VID 0xCAFE
#define HYPE_KBD_PID 0x4B44 /* "KD" */

tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00, /* per-interface, as a real keyboard does */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = HYPE_KBD_VID,
    .idProduct          = HYPE_KBD_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&desc_device; }

/* No report ID: a boot-protocol keyboard report is eight raw bytes, and adding an ID would
 * shift every field by one and stop hype decoding it. */
uint8_t const desc_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

enum { ITF_NUM_HID = 0, ITF_NUM_TOTAL };

#define EPNUM_HID 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    /* boot protocol keyboard, ep 0x81, 8 bytes, 1 ms */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report),
                       EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 1)
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, /* 0: English (US) */
    "hype",                     /* 1: manufacturer */
    "hype test keyboard #778",  /* 2: product */
    "HYPEKBD0001"               /* 3: serial */
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    uint8_t chr_count;
    (void)langid;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        const char *str;
        uint8_t i;
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* The host never GETs a report from us and never SETs one we care about, but TinyUSB
 * requires both callbacks to exist. SET_REPORT is where a host would drive the LEDs; hype
 * does not, and ignoring it is correct rather than merely convenient. */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)bufsize;
}
