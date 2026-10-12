/* Copyright (c) 2026 NXU Project. All rights reserved. */
#ifndef NXU_TERMINAL_NETUTIL_H
#define NXU_TERMINAL_NETUTIL_H

#include <stdbool.h>
#include <stdint.h>

bool netutil_address(const char *text, uint32_t family, uint8_t address[16]);
void netutil_write(const char *text);
void netutil_number(uint64_t number);
void netutil_print_address(const uint8_t *address, uint32_t family);
int netutil_ping(uint64_t argc, const char *const *argv);
int netutil_interfaces(void);

#endif
