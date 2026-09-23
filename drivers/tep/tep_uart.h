/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_uart.h
 *
 * The serial port the Trusted Enclave mailbox runs over. One backend is
 * linked per architecture: tep_uart_pl011.c on arm64 (the Device Tree's
 * second PL011, platform->mailbox_uart), tep_uart_16550.c on i386 (COM2).
 * Polled; the mailbox keeps one request in flight at a time.
 */

#ifndef NXU_DRIVERS_TEP_TEP_UART_H
#define NXU_DRIVERS_TEP_TEP_UART_H

#include <stdbool.h>
#include <stdint.h>

/*
 * tep_uart_init:
 *
 * Find and set up the mailbox port. Returns false when this machine has
 * none (or it does not identify as the expected UART). On success *where
 * names it for the log and *address holds its base (MMIO physical address
 * or I/O port).
 */
bool tep_uart_init(const char **where, uint64_t *address);

/* The next received byte, or -1 when nothing is waiting. */
int tep_uart_getc(void);

/* Send one byte, waiting while the transmitter is full. */
void tep_uart_putc(uint8_t byte);

#endif
