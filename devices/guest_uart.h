#ifndef HYPE_DEVICES_GUEST_UART_H
#define HYPE_DEVICES_GUEST_UART_H

#include <stdint.h>

/*
 * FW-1e: a minimal guest-facing 16550 UART (COM1, I/O ports
 * 0x3F8-0x3FF), emulated via IOIO-trap. This is what OVMF's serial
 * console rides on -- both the DEBUG log path (PcAtChipsetPkg
 * SerialIoLib, which spins on LSR.THRE) and, more importantly, the
 * interactive shell "Terminal" console (SioBusDxe -> PciSioSerialDxe ->
 * TerminalDxe). PciSioSerialDxe's SerialPresent probe writes 0xAA/0x55
 * to the Scratch register (offset 7) and reads them back; if that
 * doesn't round-trip, no serial console binds and no shell text ever
 * leaves the guest -- which is exactly why FW-1 saw nothing before.
 *
 * Requirements distilled from the vendored edk2 (SerialIoLib,
 * PciSioSerialDxe): the SCR (offset 7) must round-trip, and LSR
 * (offset 5) must always report THRE|TEMT so transmit never stalls.
 * The rest (IER/LCR/MCR/DLL/DLM/FCR) is store/echo. Hardware flow
 * control is off in this build, so MSR CTS/DSR/DCD are not on the
 * critical path.
 *
 * The model owns small TX/RX byte rings: guest THR writes (offset 0,
 * DLAB=0) enqueue TX (the caller drains + forwards them to hype's
 * console); RX bytes enqueued by an input source (FW-1f) are handed
 * back on RBR reads with LSR.DR set. Pure state -- no CPU/VMCB access.
 */

#define HYPE_GUEST_UART_NREGS 8u
#define HYPE_GUEST_UART_TX_RING 256u
#define HYPE_GUEST_UART_RX_RING 64u

/* Register offsets from the base port. */
#define HYPE_UART_REG_DATA 0u    /* RBR(read)/THR(write); DLL when DLAB=1 */
#define HYPE_UART_REG_IER 1u     /* interrupt enable; DLM when DLAB=1 */
#define HYPE_UART_REG_IIR_FCR 2u /* IIR(read)/FCR(write) */
#define HYPE_UART_REG_LCR 3u     /* line control; bit 7 = DLAB */
#define HYPE_UART_REG_MCR 4u     /* modem control */
#define HYPE_UART_REG_LSR 5u     /* line status (read-only here) */
#define HYPE_UART_REG_MSR 6u     /* modem status (read-only here) */
#define HYPE_UART_REG_SCR 7u     /* scratch (round-trips -- probe uses it) */

#define HYPE_UART_LCR_DLAB 0x80u
/* LSR: THRE (bit5) + TEMT (bit6) always set (infinite-speed TX); DR
 * (bit0) set when an RX byte is waiting. */
#define HYPE_UART_LSR_DR 0x01u
#define HYPE_UART_LSR_THRE_TEMT 0x60u

/* IER (interrupt enable) bits the 8250 driver uses. */
#define HYPE_UART_IER_ERBFI 0x01u /* received-data-available interrupt */
#define HYPE_UART_IER_ETBEI 0x02u /* transmitter-holding-register-empty interrupt */
/* IIR (interrupt identification) values, bit0=0 means "interrupt pending". */
#define HYPE_UART_IIR_NONE 0x01u /* no interrupt pending */
#define HYPE_UART_IIR_THRE 0x02u /* transmitter holding register empty */
#define HYPE_UART_IIR_RDA 0x04u  /* received data available */

typedef struct {
    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
    uint8_t fcr;
    /* #318: the pending transmit-empty interrupt, latched. A real 16550 raises THRE once when the
     * holding register empties and clears it when the guest reads IIR or writes THR; it does not
     * hold the line asserted for as long as ETBEI is set. Modelling it as always-asserted gave the
     * guest about six thousand interrupts a second, which it could not make progress through. */
    uint8_t thre_int;

    uint8_t tx[HYPE_GUEST_UART_TX_RING];
    uint32_t tx_head;
    uint32_t tx_tail;
    /* #356: bytes the guest wrote to THR, and how many the ring had no room for. A dropped
     * transmit byte is invisible to the guest (LSR always reports the transmitter ready), so
     * without these a lost console line looks exactly like a guest that stopped printing. */
    unsigned long long tx_written;
    unsigned long long tx_dropped;

    uint8_t rx[HYPE_GUEST_UART_RX_RING];
    uint32_t rx_head;
    uint32_t rx_tail;
    /*
     * #512: count of 0->1 transitions of the interrupt condition, kept by the MODEL. The run
     * loop's raise-once-per-assertion latch sampled the level once per loop pass -- with a
     * second vCPU servicing the ISR and re-arming the condition BETWEEN two passes, the poller
     * never saw the level fall and swallowed the new edge (ttyS0 died mid-OpenRC; apk blocked
     * on its progress write). An edge counted where it happens cannot be missed by a poller.
     */
    unsigned long long irq_events;
} hype_guest_uart_t;

void hype_guest_uart_reset(hype_guest_uart_t *u);

/* Byte-wide register access (offset is relative to the COM1 base). */
uint8_t hype_guest_uart_read(hype_guest_uart_t *u, uint32_t offset);
void hype_guest_uart_write(hype_guest_uart_t *u, uint32_t offset, uint8_t value);

/* Drain one transmitted byte (guest wrote it to THR). Returns 1 and
 * sets *out if one was available, else 0. */
int hype_guest_uart_tx_dequeue(hype_guest_uart_t *u, uint8_t *out);

/* Returns 1 if the UART currently has an enabled interrupt condition
 * asserted -- THRE (transmit always ready, when ETBEI is set) or RX data
 * available (when ERBFI is set). The 8250 driver's interrupt-driven TX
 * (used by userspace tty writes, unlike the kernel's polled printk path)
 * sleeps until this fires, so a guest that enables ETBEI blocks forever
 * on a write if the serial IRQ is never delivered. The vCPU loop turns
 * this into a raised PIC IRQ on the UART's line (COM1=IRQ4, COM2=IRQ3). */
int hype_guest_uart_irq_pending(const hype_guest_uart_t *u);

/* #512: the model's count of interrupt-condition 0->1 edges. The caller raises the guest's
 * line once per count it has not yet acted on, instead of edge-detecting the sampled level. */
unsigned long long hype_guest_uart_irq_events(const hype_guest_uart_t *u);

/* Feed one input byte to the guest (RBR + LSR.DR). Returns 1 if
 * accepted, 0 if the RX ring is full. Used by an input source (FW-1f);
 * unused by the output-only FW-1e path but modeled + tested now. */
int hype_guest_uart_rx_enqueue(hype_guest_uart_t *u, uint8_t byte);

#endif /* HYPE_DEVICES_GUEST_UART_H */
