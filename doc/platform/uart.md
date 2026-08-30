# PL011 UART

`platform/arm64/uart.c` is NXU's raw early serial transport for the ARM PL011
on QEMU `virt`.

It is no longer the kernel-wide formatting API. Kernel diagnostics go through
[`kern/console.c`](../kern/console.md), which always sends each character to the
PL011 and may fan the same character out to later display sinks.

## MMIO base

The bootstrap physical base is:

```c
#define UART_EARLY_BASE 0x09000000ULL
```

Before the MMU transition the driver accesses that physical address directly.
After the higher-half direct map exists, `uart_enter_higher_half()` converts the
same physical base to its permanent higher-half alias. This avoids retaining a
low virtual dependency after TTBR0 is disabled.

The Device Tree still describes the UART and the platform layer validates the
resource during discovery. The compile-time early base exists because the first
diagnostic output must work before Device Tree discovery itself can report an
error.

## Registers

Only two PL011 registers are required:

| Offset | Register | Purpose |
| --- | --- | --- |
| `0x00` | `UARTDR` | transmit one character |
| `0x18` | `UARTFR` | test transmit-FIFO-full bit |

`UART_FR_TXFF` is bit 5. MMIO accesses use `volatile` pointers and the VMM maps
the device region with Device attributes.

## Raw transmission

The public data operation is intentionally only:

```c
void uart_putc(char character);
```

A newline emits `\r` first, producing CRLF for serial terminals. The function
then polls `TXFF` until the FIFO accepts another byte and writes that byte to
`UARTDR`.

There is no allocation, queue or interrupt dependency, so the raw transport is
available from early boot and panic context.

## Why formatting moved out

The UART driver previously owned string, hexadecimal and decimal helpers. That
made every kernel log inherently a serial log and would have forced graphical
code to duplicate messages.

Those helpers now live in the kernel console as `kputs()`, `kputhex64()`,
`kputu64()` and `kprintf()`. The console performs fan-out once. A future
framebuffer console registers one callback and automatically receives the same
live output as UART, plus retained early boot history.

## Limitations

- transmit only;
- polling only;
- no timeout if the hardware stops accepting bytes;
- no baud-rate or line-control initialization;
- the early physical base is QEMU-`virt` specific.

## Source files

- [`platform/arm64/uart.c`](../../platform/arm64/uart.c)
- [`platform/uart.h`](../../platform/uart.h)
- [`kern/console.c`](../../kern/console.c)
- [`kern/console.h`](../../kern/console.h)
