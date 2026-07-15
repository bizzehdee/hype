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

static void test_lsr_always_transmit_ready(void) {
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

int main(void) {
    test_scratch_register_roundtrips();
    test_lsr_always_transmit_ready();
    test_thr_write_transmits();
    test_rx_read_and_dr();
    test_dlab_aliases_divisor();
    test_misc_registers();
    test_rx_ring_full_rejects();

    if (failures == 0) {
        printf("all tests passed\n");
        return 0;
    }
    printf("%d test(s) failed\n", failures);
    return 1;
}
