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
    u->thre_int = 0;
    u->tx_head = 0;
    u->tx_tail = 0;
    u->tx_dropped = 0;
    u->tx_stalled = 0;
    u->tx_written = 0;
    u->rx_head = 0;
    u->rx_tail = 0;
    for (i = 0; i < HYPE_GUEST_UART_TX_RING; i++) {
        u->tx[i] = 0;
    }
    for (i = 0; i < HYPE_GUEST_UART_RX_RING; i++) {
        u->rx[i] = 0;
    }
    u->irq_events = 0; /* #512 */
}

/* #512: bump the edge counter when a mutation raised the interrupt condition. Callers sample
 * the level BEFORE mutating and pass it in. */
static void note_irq_edge(hype_guest_uart_t *u, int was_pending) {
    if (!was_pending && hype_guest_uart_irq_pending(u)) {
        u->irq_events++;
    }
}

static int rx_available(const hype_guest_uart_t *u) {
    return u->rx_head != u->rx_tail;
}

/* #639: the transmitter is "busy" exactly while the ring has no room, and LSR.THRE has to
 * say so. Reporting a permanently-ready transmitter over a ring that can fill is what turned
 * a slow drain into silent data loss. */
static int tx_full(const hype_guest_uart_t *u) {
    return ((u->tx_tail + 1u) % HYPE_GUEST_UART_TX_RING) == u->tx_head;
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
            /* IIR: report the highest-priority enabled, asserted interrupt so the guest's
             * serial ISR knows why it fired and services it. RX-data-available (when ERBFI is
             * set) outranks THRE. bit0=0 means "interrupt pending".
             *
             * #318: THRE is a LATCH, not a level. This used to return THRE for as long as ETBEI
             * was set, on the theory that "the driver clears the source ... so this stops
             * asserting on its own". That holds only for a driver that disables ETBEI between
             * bytes. OpenBSD's com keeps ETBEI enabled while its output queue is non-empty, so
             * the line stayed asserted permanently and the guest took about six thousand
             * interrupts a second -- it never got far enough to run the handler that would have
             * serviced it, and userland output stopped after a single character.
             *
             * On real hardware, reading IIR when it reports THRE clears that interrupt. */
            if ((u->ier & HYPE_UART_IER_ERBFI) && rx_available(u)) {
                return HYPE_UART_IIR_RDA;
            }
            if ((u->ier & HYPE_UART_IER_ETBEI) && u->thre_int) {
                u->thre_int = 0;
                return HYPE_UART_IIR_THRE;
            }
            return HYPE_UART_IIR_NONE;
        case HYPE_UART_REG_LCR:
            return u->lcr;
        case HYPE_UART_REG_MCR:
            return u->mcr;
        case HYPE_UART_REG_LSR:
            /* #639: THRE/TEMT while the ring has room, clear while it is full (a polled
             * writer then spins on LSR, as it would on real hardware at 115200 baud, instead
             * of handing hype bytes it has nowhere to put). DR set iff an RX byte waits. */
            return (uint8_t)((tx_full(u) ? 0u : HYPE_UART_LSR_THRE_TEMT) |
                             (rx_available(u) ? HYPE_UART_LSR_DR : 0u));
        case HYPE_UART_REG_MSR:
            /* Benign "carrier/CTS/DSR present" -- flow control is off, so
             * OVMF's default config never gates transmit on these. */
            return 0xB0u;
        case HYPE_UART_REG_SCR:
        default:
            return u->scr;
    }
}

static void uart_write_reg(hype_guest_uart_t *u, uint32_t offset, uint8_t value) {
    int dlab = (u->lcr & HYPE_UART_LCR_DLAB) != 0;

    switch (offset & 0x7u) {
        case HYPE_UART_REG_DATA:
            if (dlab) {
                u->dll = value;
            } else {
                /* THR: enqueue for transmit. A full ring drops the byte, which the caller
                 * cannot otherwise detect -- #356: the guest wrote 17198 bytes to this
                 * register and roughly 4200 reached the log, and "the caller drains every
                 * exit, so this never fills" was an assertion in a comment with nothing
                 * measuring it. Count the drops so the next run can say whether hype is
                 * losing guest output or the guest genuinely stopped talking. */
                uint32_t next = (u->tx_tail + 1u) % HYPE_GUEST_UART_TX_RING;
                if (next != u->tx_head) {
                    u->tx[u->tx_tail] = value;
                    u->tx_tail = next;
                    u->tx_written++;
                    /* The holding register empties into the ring immediately, so THRE rises
                     * again: that 0->1 edge is what re-arms the interrupt for the next byte.
                     * Writing THR also clears any THRE interrupt already pending, as on real
                     * hardware. */
                    u->thre_int = 1;
                } else {
                    /*
                     * #639: full ring. LSR already reported the transmitter busy, so a driver
                     * that respects THRE is not here; one that wrote anyway overruns, exactly
                     * as it would on hardware. Do NOT raise THRE -- claiming the byte was sent
                     * is what let a slow drain eat 14% of a guest's console. The dequeue side
                     * raises the edge when room appears.
                     */
                    u->tx_stalled++;
                    u->tx_dropped++;
                    u->thre_int = 0;
                }
            }
            return;
        case HYPE_UART_REG_IER:
            if (dlab) {
                u->dlm = value;
            } else {
                /* Enabling ETBEI while the transmitter is idle raises THRE straight away, which
                 * is how a driver gets its first transmit interrupt without priming the port. */
                if ((value & HYPE_UART_IER_ETBEI) != 0 && (u->ier & HYPE_UART_IER_ETBEI) == 0) {
                    u->thre_int = 1;
                }
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

void hype_guest_uart_write(hype_guest_uart_t *u, uint32_t offset, uint8_t value) {
    int was = hype_guest_uart_irq_pending(u);
    uart_write_reg(u, offset, value);
    note_irq_edge(u, was); /* #512: a THR or IER write may raise the condition */
}

unsigned long long hype_guest_uart_irq_events(const hype_guest_uart_t *u) {
    return u->irq_events;
}

int hype_guest_uart_tx_dequeue(hype_guest_uart_t *u, uint8_t *out) {
    int was_full;
    int was_pending;
    if (u->tx_head == u->tx_tail) {
        return 0;
    }
    was_full = tx_full(u);
    was_pending = hype_guest_uart_irq_pending(u);
    *out = u->tx[u->tx_head];
    u->tx_head = (u->tx_head + 1u) % HYPE_GUEST_UART_TX_RING;
    /* #639: a full->has-room transition is the transmitter becoming ready again. An
     * ETBEI-driven writer is asleep waiting for precisely that edge, and nothing else in the
     * model produces it once THR writes have stopped raising THRE on a full ring. */
    if (was_full) {
        u->thre_int = 1;
        note_irq_edge(u, was_pending);
    }
    return 1;
}

int hype_guest_uart_irq_pending(const hype_guest_uart_t *u) {
    if ((u->ier & HYPE_UART_IER_ERBFI) && rx_available(u)) {
        return 1;
    }
    if ((u->ier & HYPE_UART_IER_ETBEI) && u->thre_int) {
        return 1;
    }
    return 0;
}

int hype_guest_uart_rx_enqueue(hype_guest_uart_t *u, uint8_t byte) {
    uint32_t next = (u->rx_tail + 1u) % HYPE_GUEST_UART_RX_RING;
    int was = hype_guest_uart_irq_pending(u); /* #512 */
    if (next == u->rx_head) {
        return 0; /* ring full */
    }
    u->rx[u->rx_tail] = byte;
    /*
     * #363: the producer is now the BSP (host input polling) while the consumer is the VM's own
     * core, so this is a genuine cross-core single-producer/single-consumer ring. x86 TSO does not
     * reorder stores, so the byte is visible before the tail advance that publishes it -- but the
     * COMPILER may reorder them, which would publish a slot before its contents. Barrier only; no
     * lock is needed for SPSC.
     */
    __atomic_signal_fence(__ATOMIC_RELEASE);
    u->rx_tail = next;
    note_irq_edge(u, was); /* #512: a first RX byte with ERBFI set raises the condition */
    return 1;
}
