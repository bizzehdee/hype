#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../log_split.h"

static int failures = 0;

#define CHECK_INT(desc, expected, actual)                                                    \
    do {                                                                                     \
        long long e_ = (long long)(expected), a_ = (long long)(actual);                      \
        if (e_ != a_) {                                                                      \
            printf("FAIL: %s (expected %lld, got %lld)\n", (desc), e_, a_);                  \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static int vm_of(const char *rec) { return hype_log_record_vm(rec, (unsigned int)strlen(rec)); }
static unsigned int body_of(const char *rec) {
    return hype_log_record_body_off(rec, (unsigned int)strlen(rec));
}

static void test_guest_records_route_to_their_vm(void) {
    CHECK_INT("vm0 serial", 0, vm_of("fw-1 vm0 ttyS0| Booting..."));
    CHECK_INT("vm1 serial", 1, vm_of("fw-1 vm1 ttyS0| Booting..."));
    CHECK_INT("second port still routes by VM", 1, vm_of("fw-1 vm1 ttyS1| on com2"));
    CHECK_INT("multi-digit VM index", 12, vm_of("fw-1 vm12 ttyS0| hi"));
}

/*
 * The whole point of matching the FULL emitter prefix. hype's own reports name
 * a VM too; they are hype speaking about a guest, so they belong in \hype.log.
 * A "starts with fw-1 vm" test would misfile every one of these.
 */
static void test_hype_own_records_are_not_guest_output(void) {
    CHECK_INT("VMSTAT is hype's", HYPE_LOG_VM_HYPE,
              vm_of("fw-1 VMSTAT vm0: state=2 uptime=9s"));
    CHECK_INT("IOHIST is hype's", HYPE_LOG_VM_HYPE, vm_of("fw-1 IOHIST vm0 total=99: 0x402=99"));
    CHECK_INT("ISOLATION is hype's", HYPE_LOG_VM_HYPE, vm_of("fw-1 ISOLATION: PASS -- vm0 ram@0x0"));
    CHECK_INT("EXHIST is hype's", HYPE_LOG_VM_HYPE, vm_of("fw-1 EXHIST: total=5 hlt=1"));
    CHECK_INT("usb-log is hype's", HYPE_LOG_VM_HYPE, vm_of("usb-log: streaming full log"));
    CHECK_INT("empty record is hype's", HYPE_LOG_VM_HYPE, vm_of(""));
}

/*
 * Regression, found on real hardware: boot/main.c emits guest output from TWO
 * sources -- "ttyS<M>|" for a serial port and "screen|" for the on-screen
 * terminal capture. Matching only the first sent every screen record into
 * \HYPE.LOG, i.e. guest output in hype's own log, and it took a hardware run to
 * expose it because that is where the on-screen path is exercised.
 */
static void test_screen_capture_is_guest_output_too(void) {
    CHECK_INT("vm0 screen", 0, vm_of("fw-1 vm0 screen| Booting..."));
    CHECK_INT("vm1 screen", 1, vm_of("fw-1 vm1 screen| lo0: link state changed to UP"));
    {
        const char *rec = "fw-1 vm1 screen| lo0: UP";
        CHECK_INT("screen tag kept, VM tag stripped", 0,
                  strcmp(rec + body_of(rec), "screen| lo0: UP"));
    }
    /* Still strict: an unknown source is not guest output. */
    CHECK_INT("unknown source rejected", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm0 mystery| x"));
    CHECK_INT("screen tag not terminated", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm0 screen x"));
}

static void test_malformed_prefixes_are_not_guest_output(void) {
    CHECK_INT("no port tag", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm0 hello"));
    CHECK_INT("port tag not terminated", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm0 ttyS0 hello"));
    CHECK_INT("no VM index", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm ttyS0| hi"));
    CHECK_INT("no port index", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm0 ttyS| hi"));
    CHECK_INT("truncated mid-prefix", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm0 tty"));
    CHECK_INT("absurd VM index rejected", HYPE_LOG_VM_HYPE, vm_of("fw-1 vm99999 ttyS0| hi"));
}

/* len must bound the scan: a record is a slice of the capture buffer, so the
 * bytes after it are the NEXT record, not padding. */
static void test_length_bounds_the_match(void) {
    const char *buf = "fw-1 vm0 ttyS0| hi";
    CHECK_INT("prefix cut short by len does not match", HYPE_LOG_VM_HYPE,
              hype_log_record_vm(buf, 10));
    CHECK_INT("exactly the prefix matches", 0, hype_log_record_vm(buf, 16));
}

static void test_body_offset_strips_only_the_vm_tag(void) {
    const char *rec = "fw-1 vm1 ttyS0| Booting...";
    unsigned int off = body_of(rec);
    CHECK_INT("offset lands on the port tag", 0, strcmp(rec + off, "ttyS0| Booting..."));
    CHECK_INT("hype records are not stripped", 0u, body_of("fw-1 EXHIST: total=5"));
    {
        const char *wide = "fw-1 vm12 ttyS3| x";
        CHECK_INT("multi-digit tag fully stripped", 0, strcmp(wide + body_of(wide), "ttyS3| x"));
    }
}

int main(void) {
    test_guest_records_route_to_their_vm();
    test_hype_own_records_are_not_guest_output();
    test_screen_capture_is_guest_output_too();
    test_malformed_prefixes_are_not_guest_output();
    test_length_bounds_the_match();
    test_body_offset_strips_only_the_vm_tag();
    if (failures != 0) {
        printf("test_log_split: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_log_split: all checks passed\n");
    return 0;
}
