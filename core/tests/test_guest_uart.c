#include <stdio.h>
#include "../../devices/guest_uart.h"

static int failures = 0;

#define CHECK_HEX(desc, expected, actual) \
    do { \
        if ((unsigned long long)(expected) != (unsigned long long)(actual)) { \
            printf("FAIL: %s: expected 0x%llx, got 0x%llx\n", (desc), \
                   (unsigned long long)(expected), (unsigned long long)(actual)); \
            failures++; \
        } \
    } while (0)

static void test_scratch_register_roundtrips(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    /* This is the PciSioSerialDxe SerialPresent probe (0xAA then 0x55). */
    hype_guest_uart_write(&u, HYPE_UART_REG_SCR, 0xAA);
    CHECK_HEX("SCR returns 0xAA", 0xAA, hype_guest_uart_read(&u, HYPE_UART_REG_SCR));
    hype_guest_uart_write(&u, HYPE_UART_REG_SCR, 0x55);
    CHECK_HEX("SCR returns 0x55", 0x55, hype_guest_uart_read(&u, HYPE_UART_REG_SCR));
}

static void test_lsr_transmit_ready_while_ring_has_room(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    /* THRE|TEMT set, DR clear when no input. */
    CHECK_HEX("LSR THRE|TEMT, no DR", 0x60, hype_guest_uart_read(&u, HYPE_UART_REG_LSR));
    hype_guest_uart_rx_enqueue(&u, 'k');
    CHECK_HEX("LSR DR set with input pending", 0x61, hype_guest_uart_read(&u, HYPE_UART_REG_LSR));
}

static void test_thr_write_transmits(void) {
    hype_guest_uart_t u;
    uint8_t b = 0;
    hype_guest_uart_reset(&u);
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'H');
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'i');
    CHECK_HEX("first TX byte", 'H', (hype_guest_uart_tx_dequeue(&u, &b), b));
    CHECK_HEX("second TX byte", 'i', (hype_guest_uart_tx_dequeue(&u, &b), b));
    CHECK_HEX("TX ring now empty", 0, hype_guest_uart_tx_dequeue(&u, &b));
}

static void test_rx_read_and_dr(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    CHECK_HEX("rx enqueue accepted", 1, hype_guest_uart_rx_enqueue(&u, 'A'));
    CHECK_HEX("RBR returns the byte", 'A', hype_guest_uart_read(&u, HYPE_UART_REG_DATA));
    /* Consumed -> DR clears. */
    CHECK_HEX("LSR DR clear after read", 0x60, hype_guest_uart_read(&u, HYPE_UART_REG_LSR));
    CHECK_HEX("RBR reads 0 when empty", 0, hype_guest_uart_read(&u, HYPE_UART_REG_DATA));
}

static void test_dlab_aliases_divisor(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    /* Set DLAB in LCR; offsets 0/1 become DLL/DLM. */
    hype_guest_uart_write(&u, HYPE_UART_REG_LCR, HYPE_UART_LCR_DLAB);
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 0x01); /* DLL */
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, 0x00);  /* DLM */
    CHECK_HEX("DLL reads back under DLAB", 0x01, hype_guest_uart_read(&u, HYPE_UART_REG_DATA));
    CHECK_HEX("DLM reads back under DLAB", 0x00, hype_guest_uart_read(&u, HYPE_UART_REG_IER));
    /* Clearing DLAB: offset 0 is THR/RBR again, offset 1 is IER. */
    hype_guest_uart_write(&u, HYPE_UART_REG_LCR, 0x03); /* 8N1, DLAB=0 */
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, 0x05);
    CHECK_HEX("IER reads back with DLAB clear", 0x05, hype_guest_uart_read(&u, HYPE_UART_REG_IER));
    /* A data write with DLAB clear must transmit, not set the divisor. */
    {
        uint8_t b = 0;
        hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'Z');
        CHECK_HEX("data write transmits when DLAB clear", 'Z', (hype_guest_uart_tx_dequeue(&u, &b), b));
    }
}

static void test_misc_registers(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    CHECK_HEX("IIR reads 'no interrupt pending'", 0x01, hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
    hype_guest_uart_write(&u, HYPE_UART_REG_MCR, 0x0B);
    CHECK_HEX("MCR reads back", 0x0B, hype_guest_uart_read(&u, HYPE_UART_REG_MCR));
    CHECK_HEX("MSR benign value", 0xB0, hype_guest_uart_read(&u, HYPE_UART_REG_MSR));
    /* Write to LSR (read-only) is ignored -- still the ready value. */
    hype_guest_uart_write(&u, HYPE_UART_REG_LSR, 0x00);
    CHECK_HEX("LSR write ignored", 0x60, hype_guest_uart_read(&u, HYPE_UART_REG_LSR));
    /* FCR write stored (offset 2 write path). */
    hype_guest_uart_write(&u, HYPE_UART_REG_IIR_FCR, 0x07);
}

static void test_tx_interrupt_generation(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    /* No interrupt enabled -> none pending, IIR = NONE. */
    CHECK_HEX("irq_pending 0 when IER=0", 0, hype_guest_uart_irq_pending(&u));
    CHECK_HEX("IIR NONE when IER=0", HYPE_UART_IIR_NONE,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
    /* Enable the transmit-empty interrupt: TX is infinite-speed, so it's
     * asserted immediately -- this is what unblocks interrupt-driven tty
     * writes. */
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, HYPE_UART_IER_ETBEI);
    CHECK_HEX("irq_pending 1 when ETBEI set", 1, hype_guest_uart_irq_pending(&u));
    CHECK_HEX("IIR reports THRE when ETBEI set", HYPE_UART_IIR_THRE,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
}

/*
 * #318 regression. THRE is a latch, not a level. It used to stay asserted for as long as ETBEI
 * was set, which is only harmless for a driver that disables ETBEI between bytes. OpenBSD's com
 * keeps it enabled while its output queue is non-empty, so the line never dropped and the guest
 * took about six thousand interrupts a second -- it never reached the handler that would have
 * serviced them, and userland output stopped after one character.
 */
static void test_thre_is_a_latch_not_a_level(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);

    hype_guest_uart_write(&u, HYPE_UART_REG_IER, HYPE_UART_IER_ETBEI);
    CHECK_HEX("THRE asserted when ETBEI is first enabled", 1, hype_guest_uart_irq_pending(&u));

    /* Reading IIR acknowledges it, exactly as on real hardware. */
    CHECK_HEX("IIR reports THRE once", HYPE_UART_IIR_THRE,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
    CHECK_HEX("THRE clears after IIR is read", 0, hype_guest_uart_irq_pending(&u));
    CHECK_HEX("IIR reports NONE on the second read", HYPE_UART_IIR_NONE,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));

    /* Sending a byte empties the holding register again, which re-arms it. */
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'A');
    CHECK_HEX("THRE re-arms after a byte is written", 1, hype_guest_uart_irq_pending(&u));
    CHECK_HEX("IIR reports THRE for the re-armed interrupt", HYPE_UART_IIR_THRE,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
    CHECK_HEX("and clears again", 0, hype_guest_uart_irq_pending(&u));

    /* ETBEI already set: re-writing the same IER value must not re-arm anything. */
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, HYPE_UART_IER_ETBEI);
    CHECK_HEX("re-writing IER with ETBEI already set does not re-arm", 0,
              hype_guest_uart_irq_pending(&u));
}

/*
 * #512 regression. The interrupt-condition edges must be COUNTED BY THE MODEL: a poller that
 * edge-detects the sampled level misses every edge another vCPU consumes-and-rearms between
 * two polls. Each 0->1 of irq_pending is one irq_events increment; nothing else moves it.
 */
static void test_irq_events_counts_model_edges(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    CHECK_HEX("no events after reset", 0, (unsigned)hype_guest_uart_irq_events(&u));

    /* Enabling ETBEI over an empty transmitter is the first edge. */
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, HYPE_UART_IER_ETBEI);
    CHECK_HEX("IER-enable edge counted", 1, (unsigned)hype_guest_uart_irq_events(&u));

    /* Condition already high: more THR writes are not new edges. */
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'A');
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'B');
    CHECK_HEX("no edge while condition stays high", 1,
              (unsigned)hype_guest_uart_irq_events(&u));

    /* Service (IIR read drops the latch), then the next byte is a NEW edge. */
    (void)hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR);
    CHECK_HEX("service is not an edge", 1, (unsigned)hype_guest_uart_irq_events(&u));
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'C');
    CHECK_HEX("re-arm after service is an edge", 2,
              (unsigned)hype_guest_uart_irq_events(&u));

    /* RX: first byte with ERBFI enabled is an edge; a second while pending is not. */
    hype_guest_uart_reset(&u);
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, HYPE_UART_IER_ERBFI);
    CHECK_HEX("ERBFI with empty RX is no edge", 0, (unsigned)hype_guest_uart_irq_events(&u));
    (void)hype_guest_uart_rx_enqueue(&u, 'x');
    CHECK_HEX("first RX byte is an edge", 1, (unsigned)hype_guest_uart_irq_events(&u));
    (void)hype_guest_uart_rx_enqueue(&u, 'y');
    CHECK_HEX("second RX byte while pending is not", 1,
              (unsigned)hype_guest_uart_irq_events(&u));
}

static void test_rx_interrupt_and_priority(void) {
    hype_guest_uart_t u;
    hype_guest_uart_reset(&u);
    /* ERBFI enabled but no RX byte waiting -> not pending. */
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, HYPE_UART_IER_ERBFI);
    CHECK_HEX("irq_pending 0 when ERBFI set but no RX", 0, hype_guest_uart_irq_pending(&u));
    /* A waiting RX byte asserts the received-data interrupt. */
    hype_guest_uart_rx_enqueue(&u, 'x');
    CHECK_HEX("irq_pending 1 when ERBFI + RX", 1, hype_guest_uart_irq_pending(&u));
    CHECK_HEX("IIR reports RDA on RX", HYPE_UART_IIR_RDA,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
    /* RX outranks THRE when both are enabled+asserted. */
    hype_guest_uart_write(&u, HYPE_UART_REG_IER,
                          (uint8_t)(HYPE_UART_IER_ERBFI | HYPE_UART_IER_ETBEI));
    CHECK_HEX("IIR RDA outranks THRE", HYPE_UART_IIR_RDA,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
    /* Draining the RX byte drops back to the THRE interrupt. */
    (void)hype_guest_uart_read(&u, HYPE_UART_REG_DATA);
    CHECK_HEX("IIR falls to THRE after RX drained", HYPE_UART_IIR_THRE,
              hype_guest_uart_read(&u, HYPE_UART_REG_IIR_FCR));
}

static void test_rx_ring_full_rejects(void) {
    hype_guest_uart_t u;
    unsigned int i;
    int last = 1;
    hype_guest_uart_reset(&u);
    /* Ring holds HYPE_GUEST_UART_RX_RING-1 usable slots; fill past it. */
    for (i = 0; i < HYPE_GUEST_UART_RX_RING + 4u; i++) {
        last = hype_guest_uart_rx_enqueue(&u, (uint8_t)i);
    }
    CHECK_HEX("rx enqueue eventually rejects when full", 0, last);
}

/*
 * #639: a full TX ring must apply back-pressure, not swallow bytes.
 *
 * Measured on the AMD boot-D run: only the BSP loop drains this ring, and a guest writing
 * from another vCPU queued 41199 characters between drains against a 256-byte ring. 5941 of
 * them were dropped, the shell prompt was among them, and the input script waiting on that
 * prompt hung for the rest of the run while the guest sat healthy at a live shell.
 *
 * The contract this pins: while the ring is full LSR reports the transmitter BUSY (so a
 * polled writer waits) and THR writes raise no THRE (so an interrupt-driven writer is not
 * told a byte it never queued was sent); the first dequeue that makes room raises THRE again
 * as a counted 0->1 edge, which is what wakes that writer.
 */
static void test_tx_ring_full_applies_backpressure(void) {
    hype_guest_uart_t u;
    unsigned int i;
    unsigned long long edges_before;
    uint8_t b = 0;
    hype_guest_uart_reset(&u);
    hype_guest_uart_write(&u, HYPE_UART_REG_IER, HYPE_UART_IER_ETBEI);

    for (i = 0; i < HYPE_GUEST_UART_TX_RING - 1u; i++) {
        hype_guest_uart_write(&u, HYPE_UART_REG_DATA, (uint8_t)('a' + (i % 26u)));
    }
    CHECK_HEX("ring holds capacity-1 bytes with none lost", 0, (int)u.tx_dropped);
    CHECK_HEX("LSR reports transmitter busy when full", 0x00,
              hype_guest_uart_read(&u, HYPE_UART_REG_LSR));

    /* A guest that writes anyway overruns -- hardware behaviour -- and must not be told THRE. */
    hype_guest_uart_write(&u, HYPE_UART_REG_DATA, 'X');
    CHECK_HEX("write to a full ring is counted as stalled", 1, (int)u.tx_stalled);
    CHECK_HEX("no THRE interrupt while the ring is full", 0, hype_guest_uart_irq_pending(&u));

    edges_before = hype_guest_uart_irq_events(&u);
    CHECK_HEX("dequeue from a full ring succeeds", 1, hype_guest_uart_tx_dequeue(&u, &b));
    CHECK_HEX("first queued byte survives the overrun", 'a', b);
    CHECK_HEX("room again raises THRE", 1, hype_guest_uart_irq_pending(&u));
    CHECK_HEX("room again counts one model edge", 1,
              (int)(hype_guest_uart_irq_events(&u) - edges_before));
    CHECK_HEX("LSR ready again once there is room", 0x60,
              hype_guest_uart_read(&u, HYPE_UART_REG_LSR));
}

int main(void) {
    test_scratch_register_roundtrips();
    test_lsr_transmit_ready_while_ring_has_room();
    test_thr_write_transmits();
    test_rx_read_and_dr();
    test_dlab_aliases_divisor();
    test_misc_registers();
    test_tx_interrupt_generation();
    test_thre_is_a_latch_not_a_level();
    test_irq_events_counts_model_edges();
    test_rx_interrupt_and_priority();
    test_rx_ring_full_rejects();
    test_tx_ring_full_applies_backpressure();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
