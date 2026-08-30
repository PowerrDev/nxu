#include <platform/uart.h>
#include <vm/vmm.h>

#include <stdbool.h>
#include <stdint.h>

#define UART_EARLY_BASE 0x09000000ULL
#define UART_DR_OFFSET 0x00ULL
#define UART_FR_OFFSET 0x18ULL

#define UART_FR_TXFF (1U << 5U)

static uint64_t g_uart_base = UART_EARLY_BASE;
static bool g_uart_higher_half;

static volatile uint32_t *uart_register(uint64_t offset)
{
	return (volatile uint32_t *)(g_uart_base + offset);
}

bool uart_enter_higher_half(void)
{
	if (g_uart_higher_half || !vmm_higher_half_direct_map_enabled()) {
		return false;
	}

	uint64_t virtual_address;

	if (!vmm_physical_to_higher_half(
		UART_EARLY_BASE,
		&virtual_address
	)) {
		return false;
	}

	g_uart_base = virtual_address;
	g_uart_higher_half = true;

	return true;
}

bool uart_higher_half_enabled(void)
{
	return g_uart_higher_half;
}

void uart_putc(char character)
{
	if (character == '\n') {
		uart_putc('\r');
	}

	while ((*uart_register(UART_FR_OFFSET) & UART_FR_TXFF) != 0U) {
		__asm__ volatile("yield");
	}

	*uart_register(UART_DR_OFFSET) = (uint32_t)character;
}
