#include "guest_uart.h"

void hype_guest_uart_reset(hype_guest_uart_t *u) {
    unsigned int i;
    u->ier = 0;
    u->lcr = 0;
    u->mcr = 0;
    u->scr = 0;
    u->dll = 1; /* nonzero default divisor (115200) -- value is irrelevant to us */
    u->dlm = 0;
    u->fcr = 0;
    u->tx_head = 0;
    u->tx_tail = 0;
    u->rx_head = 0;
    u->rx_tail = 0;
    for (i = 0; i < HYPE_GUEST_UART_TX_RING; i++) {
        u->tx[i] = 0;
    }
    for (i = 0; i < HYPE_GUEST_UART_RX_RING; i++) {
        u->rx[i] = 0;
    }
}

static int rx_available(const hype_guest_uart_t *u) {
    return u->rx_head != u->rx_tail;
}

uint8_t hype_guest_uart_read(hype_guest_uart_t *u, uint32_t offset) {
    int dlab = (u->lcr & HYPE_UART_LCR_DLAB) != 0;

    switch (offset & 0x7u) {
        case HYPE_UART_REG_DATA:
            if (dlab) {
                return u->dll;
            }
            /* RBR: pop one RX byte if available, else 0. */
            if (rx_available(u)) {
                uint8_t b = u->rx[u->rx_head];
                u->rx_head = (u->rx_head + 1u) % HYPE_GUEST_UART_RX_RING;
                return b;
            }
            return 0;
        case HYPE_UART_REG_IER:
            return dlab ? u->dlm : u->ier;
        case HYPE_UART_REG_IIR_FCR:
            /* IIR: bit 0 set = "no interrupt pending" (we drive none). */
            return 0x01u;
        case HYPE_UART_REG_LCR:
            return u->lcr;
        case HYPE_UART_REG_MCR:
            return u->mcr;
        case HYPE_UART_REG_LSR:
            /* Transmitter always ready; DR set iff an RX byte waits. */
            return (uint8_t)(HYPE_UART_LSR_THRE_TEMT | (rx_available(u) ? HYPE_UART_LSR_DR : 0u));
        case HYPE_UART_REG_MSR:
            /* Benign "carrier/CTS/DSR present" -- flow control is off, so
             * OVMF's default config never gates transmit on these. */
            return 0xB0u;
        case HYPE_UART_REG_SCR:
        default:
            return u->scr;
    }
}

void hype_guest_uart_write(hype_guest_uart_t *u, uint32_t offset, uint8_t value) {
    int dlab = (u->lcr & HYPE_UART_LCR_DLAB) != 0;

    switch (offset & 0x7u) {
        case HYPE_UART_REG_DATA:
            if (dlab) {
                u->dll = value;
            } else {
                /* THR: enqueue for transmit (drop if the ring is full --
                 * we never fill it in practice since the caller drains
                 * every exit). */
                uint32_t next = (u->tx_tail + 1u) % HYPE_GUEST_UART_TX_RING;
                if (next != u->tx_head) {
                    u->tx[u->tx_tail] = value;
                    u->tx_tail = next;
                }
            }
            return;
        case HYPE_UART_REG_IER:
            if (dlab) {
                u->dlm = value;
            } else {
                u->ier = value;
            }
            return;
        case HYPE_UART_REG_IIR_FCR:
            u->fcr = value;
            return;
        case HYPE_UART_REG_LCR:
            u->lcr = value;
            return;
        case HYPE_UART_REG_MCR:
            u->mcr = value;
            return;
        case HYPE_UART_REG_SCR:
            u->scr = value;
            return;
        case HYPE_UART_REG_LSR:
        case HYPE_UART_REG_MSR:
        default:
            /* LSR/MSR are read-only; ignore writes. */
            return;
    }
}

int hype_guest_uart_tx_dequeue(hype_guest_uart_t *u, uint8_t *out) {
    if (u->tx_head == u->tx_tail) {
        return 0;
    }
    *out = u->tx[u->tx_head];
    u->tx_head = (u->tx_head + 1u) % HYPE_GUEST_UART_TX_RING;
    return 1;
}

int hype_guest_uart_rx_enqueue(hype_guest_uart_t *u, uint8_t byte) {
    uint32_t next = (u->rx_tail + 1u) % HYPE_GUEST_UART_RX_RING;
    if (next == u->rx_head) {
        return 0; /* ring full */
    }
    u->rx[u->rx_tail] = byte;
    u->rx_tail = next;
    return 1;
}
