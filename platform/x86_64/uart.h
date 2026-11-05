/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        platform/x86_64/uart.h
 *
 * Declares the early COM1 serial interface used before the console exists.
 */

#ifndef NXU_PLATFORM_X86_64_UART_H
#define NXU_PLATFORM_X86_64_UART_H

/*
 * Routine:     x86_uart_initialize
 * Purpose:
 *              Configure COM1 as an 8N1 polling UART for early boot output.
 */
void x86_uart_initialize(void);

/*
 * Routine:     x86_uart_write
 * Purpose:
 *              Send a NUL-terminated message through the early COM1 console.
 */
void x86_uart_write(const char *message);

#endif
