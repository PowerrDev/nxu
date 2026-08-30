#ifndef NXU_UART_H
#define NXU_UART_H

#include <stdbool.h>
#include <stdint.h>

bool uart_enter_higher_half(void);
bool uart_higher_half_enabled(void);

void uart_putc(char character);

#endif
