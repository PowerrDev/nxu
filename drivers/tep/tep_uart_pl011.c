/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_uart_pl011.c
 *
 * arm64 mailbox port: the Device Tree's second "arm,pl011"
 * (platform->mailbox_uart), reached through the higher-half direct map.
 */

#include <drivers/tep/tep_uart.h>

#include <kern/machine/cpu.h>
#include <platform/platform.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>

/* PL011 (ARM DDI 0183). */
#define UART_DR 0x000U
#define UART_FR 0x018U
#define UART_LCR_H 0x02cU
#define UART_CR 0x030U
#define UART_IMSC 0x038U
#define UART_ICR 0x044U
#define UART_PERIPHID0 0xfe0U
#define UART_CELLID0 0xff0U
#define UART_FR_RXFE (1U << 4U)
#define UART_FR_TXFF (1U << 5U)
#define UART_LCR_H_FEN (1U << 4U)
#define UART_LCR_H_WLEN8 (3U << 5U)
#define UART_CR_UARTEN (1U << 0U)
#define UART_CR_TXE (1U << 8U)
#define UART_CR_RXE (1U << 9U)

static volatile uint32_t *g_uart;

static uint32_t uart_read(uint32_t offset)
{
	return g_uart[offset / 4U];
}

static void uart_write(uint32_t offset, uint32_t value)
{
	g_uart[offset / 4U] = value;
}

bool tep_uart_init(const char **where, uint64_t *address)
{
	static const uint8_t cell_id[4] = { 0x0dU, 0xf0U, 0x05U, 0xb1U };
	const platform_t *platform = platform_get();
	uint64_t virtual_address;

	if (platform == 0 || platform->mailbox_uart.size == 0ULL) return false;
	if (!vmm_physical_to_higher_half(platform->mailbox_uart.base, &virtual_address)) return false;
	g_uart = (volatile uint32_t *)virtual_address;

	for (uint32_t index = 0U; index < 4U; index++) {
		if ((uart_read(UART_CELLID0 + 4U * index) & 0xffU) != cell_id[index]) return false;
	}
	if ((uart_read(UART_PERIPHID0) & 0xffU) != 0x11U) return false;

	uart_write(UART_CR, 0U);
	uart_write(UART_LCR_H, UART_LCR_H_WLEN8 | UART_LCR_H_FEN);
	uart_write(UART_IMSC, 0U);
	uart_write(UART_ICR, 0x7ffU);
	uart_write(UART_CR, UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);

	*where = "PL011";
	*address = platform->mailbox_uart.base;
	return true;
}

int tep_uart_getc(void)
{
	if ((uart_read(UART_FR) & UART_FR_RXFE) != 0U) return -1;
	return (int)(uart_read(UART_DR) & 0xffU);
}

void tep_uart_putc(uint8_t byte)
{
	while ((uart_read(UART_FR) & UART_FR_TXFF) != 0U) cpu_relax();
	uart_write(UART_DR, byte);
}
