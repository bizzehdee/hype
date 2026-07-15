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

typedef struct {
    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
    uint8_t fcr;

    uint8_t tx[HYPE_GUEST_UART_TX_RING];
    uint32_t tx_head;
    uint32_t tx_tail;

    uint8_t rx[HYPE_GUEST_UART_RX_RING];
    uint32_t rx_head;
    uint32_t rx_tail;
} hype_guest_uart_t;

void hype_guest_uart_reset(hype_guest_uart_t *u);

/* Byte-wide register access (offset is relative to the COM1 base). */
uint8_t hype_guest_uart_read(hype_guest_uart_t *u, uint32_t offset);
void hype_guest_uart_write(hype_guest_uart_t *u, uint32_t offset, uint8_t value);

/* Drain one transmitted byte (guest wrote it to THR). Returns 1 and
 * sets *out if one was available, else 0. */
int hype_guest_uart_tx_dequeue(hype_guest_uart_t *u, uint8_t *out);

/* Feed one input byte to the guest (RBR + LSR.DR). Returns 1 if
 * accepted, 0 if the RX ring is full. Used by an input source (FW-1f);
 * unused by the output-only FW-1e path but modeled + tested now. */
int hype_guest_uart_rx_enqueue(hype_guest_uart_t *u, uint8_t byte);

#endif /* HYPE_DEVICES_GUEST_UART_H */
