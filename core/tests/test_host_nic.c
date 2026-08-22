#include <stdio.h>
#include "../host_nic.h"

static int failures;

#define CHECK(desc, cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL: %s\n", (desc));                                                          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define CHECK_HEX(desc, expected, actual)                                                          \
    do {                                                                                           \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) {                      \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), (unsigned long long)(expected), \
                   (unsigned long long)(actual));                                                  \
            failures++;                                                                            \
        }                                                                                           \
    } while (0)

/* Same topology as core/tests/test_host_pci.c's cfg_two_nics: an Intel 8086:100E at 00:03.0
 * (a chip this test's fake driver claims), a Realtek 10ec:8168 at 02:00.0 (nothing claims it). */
static uint32_t cfg_two_nics(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    if (bus == 0 && dev == 3 && func == 0) {
        switch (off) {
            case 0x00: return 0x100E8086u;
            case 0x08: return 0x02000003u;
            case 0x10: return 0xFEB80000u;
            default:   return 0x00000000u;
        }
    }
    if (bus == 2 && dev == 0 && func == 0) {
        switch (off) {
            case 0x00: return 0x816810ecu;
            case 0x08: return 0x02000000u;
            case 0x10: return 0xFE900000u;
            default:   return 0x00000000u;
        }
    }
    return 0xFFFFFFFFu;
}

static int g_attach_calls;
static int g_attach_should_fail;

static int fake_attach(const hype_host_nic_t *loc, hype_host_nic_bound_t *out) {
    g_attach_calls++;
    if (g_attach_should_fail) {
        return 0;
    }
    out->mac[0] = 0xDE;
    out->mac[1] = 0xAD;
    out->mac[2] = 0xBE;
    out->mac[3] = 0xEF;
    out->mac[4] = (uint8_t)loc->bus;
    out->mac[5] = (uint8_t)loc->dev;
    out->mac_valid = 1;
    out->link_up = 1;
    return 1;
}

static hype_host_nic_driver_t fake_driver(void) {
    hype_host_nic_driver_t d;
    d.name = "fake";
    d.match_count = 1;
    d.match_ids[0].vendor_id = 0x8086u;
    d.match_ids[0].device_id = 0x100Eu;
    d.probe = 0;
    d.attach = fake_attach;
    return d;
}

static void test_register_and_match(void) {
    hype_host_nic_driver_t d = fake_driver();
    hype_host_nic_driver_t bad;
    const hype_host_nic_driver_t *m;

    hype_host_nic_registry_reset();

    CHECK("registering NULL is refused", !hype_host_nic_register(0));

    bad = d;
    bad.attach = 0;
    CHECK("registering with no attach fn is refused", !hype_host_nic_register(&bad));

    bad = d;
    bad.match_count = 0;
    CHECK("registering with zero match entries is refused", !hype_host_nic_register(&bad));

    bad = d;
    bad.match_count = HYPE_HOST_NIC_MAX_MATCH_IDS + 1u;
    CHECK("registering with too many match entries is refused", !hype_host_nic_register(&bad));

    CHECK("a well-formed driver registers", hype_host_nic_register(&d));

    m = hype_host_nic_match(0x8086u, 0x100Eu);
    CHECK("the registered vendor/device matches", m != 0 && m->attach == fake_attach);
    CHECK("an unregistered vendor/device does not match", hype_host_nic_match(0x10ecu, 0x8168u) == 0);

    hype_host_nic_registry_reset();
    CHECK("reset clears the registry", hype_host_nic_match(0x8086u, 0x100Eu) == 0);
}

static void test_registration_cap(void) {
    int i, ok;
    hype_host_nic_driver_t d = fake_driver();

    hype_host_nic_registry_reset();
    ok = 1;
    for (i = 0; i < HYPE_HOST_NIC_MAX_DRIVERS; i++) {
        ok = ok && hype_host_nic_register(&d);
    }
    CHECK("filling the registry to its cap succeeds", ok);
    CHECK("one more registration past the cap is refused", !hype_host_nic_register(&d));
    hype_host_nic_registry_reset();
}

static void test_probe_all(void) {
    hype_host_nic_driver_t d = fake_driver();
    hype_host_nic_bound_t out[4];
    unsigned int n;

    hype_host_nic_registry_reset();
    hype_host_nic_register(&d);
    g_attach_calls = 0;
    g_attach_should_fail = 0;

    n = hype_host_nic_probe_all(cfg_two_nics, 3, out, 4u);
    CHECK_HEX("only the driven Intel NIC is reported, not the driverless Realtek", 1, n);
    CHECK_HEX("attach was called exactly once", 1, g_attach_calls);
    CHECK_HEX("bound entry is marked attached", 1, out[0].attached);
    CHECK_HEX("mac_valid set by the driver survives", 1, out[0].mac_valid);
    CHECK_HEX("mac byte 0 from the driver survives", 0xDEu, out[0].mac[0]);
    CHECK_HEX("link_up from the driver survives", 1, out[0].link_up);
    CHECK_HEX("bound entry keeps the discovered vendor id", 0x8086u, out[0].loc.vendor_id);

    hype_host_nic_registry_reset();
}

static void test_probe_all_no_driver(void) {
    hype_host_nic_bound_t out[4];
    unsigned int n;

    /* No driver registered at all -- both NICs are class-0x02 hits with nobody to claim them. */
    hype_host_nic_registry_reset();
    n = hype_host_nic_probe_all(cfg_two_nics, 3, out, 4u);
    CHECK_HEX("no registered driver means nothing is reported", 0, n);
}

static void test_probe_all_attach_failure(void) {
    hype_host_nic_driver_t d = fake_driver();
    hype_host_nic_bound_t out[4];
    unsigned int n;

    hype_host_nic_registry_reset();
    hype_host_nic_register(&d);
    g_attach_should_fail = 1;

    n = hype_host_nic_probe_all(cfg_two_nics, 3, out, 4u);
    CHECK_HEX("a matched but failed attach is still reported", 1, n);
    CHECK_HEX("the bound entry reflects the attach failure", 0, out[0].attached);

    g_attach_should_fail = 0;
    hype_host_nic_registry_reset();
}

static int probe_decline(const hype_host_nic_t *loc) {
    (void)loc;
    return 0;
}

static void test_probe_all_null_guards(void) {
    hype_host_nic_bound_t out[4];
    CHECK_HEX("NULL read32 refuses rather than crashing", 0, hype_host_nic_probe_all(0, 3, out, 4u));
    CHECK_HEX("NULL out refuses rather than crashing", 0, hype_host_nic_probe_all(cfg_two_nics, 3, 0, 4u));
}

static void test_probe_declined_by_probe_fn(void) {
    hype_host_nic_driver_t d = fake_driver();
    hype_host_nic_bound_t out[4];
    unsigned int n;

    /* The vendor/device table matches, but the driver's own probe() gate says no (e.g. a
     * revision/prog-if check beyond IDs) -- the function must be treated as undriven, same as if
     * no driver had matched the IDs at all. */
    d.probe = probe_decline;
    hype_host_nic_registry_reset();
    hype_host_nic_register(&d);
    g_attach_calls = 0;

    n = hype_host_nic_probe_all(cfg_two_nics, 3, out, 4u);
    CHECK_HEX("a declined probe reports nothing", 0, n);
    CHECK_HEX("attach is never called after probe declines", 0, g_attach_calls);

    hype_host_nic_registry_reset();
}

static void test_probe_all_output_cap(void) {
    hype_host_nic_driver_t d = fake_driver();
    hype_host_nic_bound_t out[1];
    unsigned int n;

    hype_host_nic_registry_reset();
    hype_host_nic_register(&d);
    n = hype_host_nic_probe_all(cfg_two_nics, 3, out, 1u);
    CHECK_HEX("output cap of 1 is respected even with room to find more", 1, n);
    hype_host_nic_registry_reset();
}

int main(void) {
    test_register_and_match();
    test_registration_cap();
    test_probe_all();
    test_probe_all_no_driver();
    test_probe_all_attach_failure();
    test_probe_all_null_guards();
    test_probe_declined_by_probe_fn();
    test_probe_all_output_cap();

    if (failures) {
        printf("%d test(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
