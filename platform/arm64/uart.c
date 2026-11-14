/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/arm64/uart.c
 *
 * The console PL011 (ARM DDI 0183) of QEMU virt, the <platform/uart.h>
 * interface on arm64. Transmitting works from the first instruction of
 * the boot, at the fixed physical base; uart_enter_higher_half moves to the
 * direct-map alias of the same frame once there is one.
 *
 * Receiving. The receive, receive-timeout and error interrupts are enabled
 * and disabled together. The interrupt is the one the Device Tree gives the
 * console UART (platform->uart_intid), a GIC SPI routed to the boot CPU.
 * The handler clears every interrupt the UART has raised before the client
 * drains the FIFO, so a byte that arrives while it drains raises the
 * interrupt again rather than being missed. The data register returns each
 * byte with its own error bits.
 *
 * Line settings. The character format (LCR_H) can be changed; the speed
 * cannot, because the divisor depends on the UART's reference clock, which
 * the Device Tree describes and this driver does not read. The line stays at
 * the speed the firmware left (115200 on QEMU, where it has no effect) and
 * uart_set_line refuses any other.
 */

#include <platform/uart.h>

#include <kern/arm64/gic.h>
#include <kern/irq/irq.h>
#include <platform/platform.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>

#define UART_EARLY_BASE 0x09000000ULL

#define UART_DR_OFFSET 0x00ULL
#define UART_FR_OFFSET 0x18ULL
#define UART_LCR_H_OFFSET 0x2CULL
#define UART_CR_OFFSET 0x30ULL
#define UART_IMSC_OFFSET 0x38ULL
#define UART_ICR_OFFSET 0x44ULL

#define UART_DR_FRAMING (1U << 8U)
#define UART_DR_PARITY (1U << 9U)
#define UART_DR_BREAK (1U << 10U)
#define UART_DR_OVERRUN (1U << 11U)

#define UART_FR_BUSY (1U << 3U)
#define UART_FR_RXFE (1U << 4U)
#define UART_FR_TXFF (1U << 5U)

#define UART_LCR_H_PEN (1U << 1U)
#define UART_LCR_H_EPS (1U << 2U)
#define UART_LCR_H_STP2 (1U << 3U)
#define UART_LCR_H_FEN (1U << 4U)
#define UART_LCR_H_WLEN_SHIFT 5U

#define UART_CR_UARTEN (1U << 0U)

/* Receive, receive timeout, and the framing, parity, break and overrun errors. */
#define UART_IMSC_RECEIVE ((1U << 4U) | (1U << 6U) | (1U << 7U) | (1U << 8U) | (1U << 9U) | (1U << 10U))
#define UART_ICR_ALL 0x7FFU

/* The only speed the line runs at; see the file header. */
#define UART_FIXED_SPEED 115200U

/* As the virtio-mmio devices. */
#define UART_GIC_PRIORITY 0x90U

static uint64_t g_uart_base = UART_EARLY_BASE;
static bool g_uart_higher_half;

static uart_receive_handler_t g_uart_handler;
static void *g_uart_handler_context;

static volatile uint32_t *
uart_register(uint64_t offset)
{
	return (volatile uint32_t *)(g_uart_base + offset);
}

bool
uart_enter_higher_half(void)
{
	if (g_uart_higher_half || !vmm_higher_half_direct_map_enabled()) {
		return false;
	}

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(UART_EARLY_BASE, &virtual_address)) {
		return false;
	}

	g_uart_base = virtual_address;
	g_uart_higher_half = true;

	return true;
}

bool
uart_higher_half_enabled(void)
{
	return g_uart_higher_half;
}

void
uart_put_byte(uint8_t byte)
{
	while ((*uart_register(UART_FR_OFFSET) & UART_FR_TXFF) != 0U) {
		__asm__ volatile("yield");
	}

	*uart_register(UART_DR_OFFSET) = (uint32_t)byte;
}

void
uart_putc(char character)
{
	if (character == '\n') uart_put_byte('\r');

	uart_put_byte((uint8_t)character);
}

bool
uart_get_byte(uint8_t *byte, uint32_t *status)
{
	if ((*uart_register(UART_FR_OFFSET) & UART_FR_RXFE) != 0U) return false;

	uint32_t data = *uart_register(UART_DR_OFFSET);
	uint32_t flags = 0U;

	if ((data & UART_DR_BREAK) != 0U) flags |= UART_RX_BREAK;
	if ((data & UART_DR_PARITY) != 0U) flags |= UART_RX_PARITY;
	if ((data & UART_DR_FRAMING) != 0U) flags |= UART_RX_FRAMING;
	if ((data & UART_DR_OVERRUN) != 0U) flags |= UART_RX_OVERRUN;

	*byte = (uint8_t)(data & 0xFFU);
	*status = flags;
	return true;
}

bool
uart_set_line(const uart_line_t *line)
{
	if (line->speed != UART_FIXED_SPEED) return false;
	if (line->data_bits < 5U || line->data_bits > 8U) return false;
	if (line->stop_bits != 1U && line->stop_bits != 2U) return false;

	uint32_t format = UART_LCR_H_FEN | ((line->data_bits - 5U) << UART_LCR_H_WLEN_SHIFT);

	if (line->stop_bits == 2U) format |= UART_LCR_H_STP2;
	if (line->parity != UART_PARITY_NONE) format |= UART_LCR_H_PEN;
	if (line->parity == UART_PARITY_EVEN) format |= UART_LCR_H_EPS;

	/* The format may only change with the UART disabled and idle (DDI 0183, UARTCR). */
	uint32_t control = *uart_register(UART_CR_OFFSET);

	while ((*uart_register(UART_FR_OFFSET) & UART_FR_BUSY) != 0U) {
		__asm__ volatile("yield");
	}

	*uart_register(UART_CR_OFFSET) = control & ~UART_CR_UARTEN;
	*uart_register(UART_LCR_H_OFFSET) = format;
	*uart_register(UART_CR_OFFSET) = control;
	return true;
}

/* The console UART's SPI: clear what it raised, then let the client drain. */
static void
uart_irq(uint32_t intid, void *context)
{
	(void)intid;
	(void)context;

	*uart_register(UART_ICR_OFFSET) = UART_ICR_ALL;
	g_uart_handler(g_uart_handler_context);
}

bool
uart_receive_interrupt_attach(uart_receive_handler_t handler, void *context)
{
	const platform_t *platform = platform_get();

	if (handler == 0 || g_uart_handler != 0 || platform == 0 || platform->uart_intid == 0U) return false;

	g_uart_handler = handler;
	g_uart_handler_context = context;

	*uart_register(UART_IMSC_OFFSET) = 0U;
	*uart_register(UART_ICR_OFFSET) = UART_ICR_ALL;

	if (!irq_register(platform->uart_intid, uart_irq, 0)) {
		g_uart_handler = 0;
		return false;
	}

	/* Device Tree trigger flags: 1 and 2 are the edges, 4 and 8 the levels. */
	bool edge_triggered = (platform->uart_irq_flags & 3U) != 0U;

	if (!gic_enable_spi(platform->uart_intid, UART_GIC_PRIORITY, edge_triggered)) {
		(void)irq_unregister(platform->uart_intid, uart_irq, 0);
		g_uart_handler = 0;
		return false;
	}

	return true;
}

void
uart_receive_interrupt_enable(bool enable)
{
	*uart_register(UART_IMSC_OFFSET) = enable ? UART_IMSC_RECEIVE : 0U;
}
