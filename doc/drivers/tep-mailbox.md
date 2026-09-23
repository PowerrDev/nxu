# Trusted Enclave Mailbox

## Overview

tepOS, the security OS of the Trusted Enclave Processor, runs on its own
machine: a separate QEMU built from the TrustedEnclaveProcessor repository,
based on the seL4 microkernel. NXU never maps or reads tepOS memory. The only
connection between them is a dedicated serial link, and everything crosses it
as framed requests and responses (`drivers/tep/tep_mailbox.c`).

```
NXU QEMU                                        tepOS QEMU
  serial1 (PL011 @ 0x9040000)                     serial1 (PL011 @ 0x9040000)
     |                                                 |
  Unix socket (listening)  <-- mailbox_link.py -->  Unix socket (listening)
```

## Protocol

Version 1 is defined by tepOS's `boot/include/tep/mailbox.h`; NXU keeps a copy
in `drivers/tep/tep_mailbox_proto.h`. A frame is a 16-byte little-endian header
(magic `TP`, version, request/response type, command, status, 32-bit request
id, payload length), at most 240 bytes of payload, and an IEEE CRC-32. Every
command has an exact payload length. The CRC detects link corruption only; it
is not authentication.

| Command | Request | Response |
|---------|---------|----------|
| `HELLO` | empty | protocol version, tepOS version, boot id |
| `GET_HEALTH` | empty | tepOS health, then state and restart count per service |

No command reads or writes memory on either side. Cryptographic services come
later and will be added as further commands.

## Driver

The Device Tree's first `arm,pl011` is the console; a second one becomes
`platform->mailbox_uart`, mapped as device memory in the higher-half direct
map. Without it the link is `absent` and every request is refused.

`tep_mailbox_start()` (called from `kern_init` after the core POST) checks the
PrimeCell IDs and starts a monitor thread:

- until tepOS answers `HELLO` with protocol version 1, the link is
  `unavailable`;
- once it has, it is `available` and the monitor asks for health every second,
  logging changes;
- three failed health checks in a row (500 ms response timeout each) make it
  `unavailable` again, and it goes back to `HELLO`;
- a changed boot id in `HELLO` is logged as a tepOS restart.

`tep_mailbox_request()` fails closed: unless the link is `available` it
returns `TEP_REQ_UNAVAILABLE` at once without touching the UART, and nothing
in NXU stands in for a tepOS answer. Requests are serialized; each flushes
stale input, sends, and waits for the response with its request id.

The UART is polled. Waits sleep once the periodic timer runs and yield before
that, so boot-time tests work with the cooperative scheduler.

## Running it

In the TrustedEnclaveProcessor repository, `make run` boots tepOS with its
serial1 on `/tmp/tepos-mailbox.sock`. `make run-console TEP=1` boots NXU with
its serial1 on `/tmp/nxu-mailbox.sock` (`TEP_NXU_SOCK`). Then join them with
that repository's `tools/mailbox_link.py`; either side may start first or
restart, and the relay relinks.

Neither QEMU connects out on purpose: QEMU 11.1's socket chardev in client
mode with `reconnect-ms` aborts the whole emulator whenever a connection
attempt fails (`qemu_chr_socket_connected` -> `yank_unregister_function`).
Both QEMUs listen, and only the relay connects.

## Testing

`make test-tep-mailbox` (`tools/test_tep_mailbox.sh`) builds tepOS and the
`tep-mailbox` test kernel, boots both with the relay and runs
`kern/tests/tep_mailbox_test.c`: the protocol checks against the live tepOS,
then the harness stops tepOS and the test requires the driver to declare it
unavailable and refuse requests, then the harness restarts tepOS and the test
requires the driver to reconnect. See [Testing](../testing.md).
