# Trusted Enclave Mailbox

## Overview

tepOS, the security OS of the Trusted Enclave Processor, runs on its own
machine: a separate QEMU built from the TrustedEnclaveProcessor repository,
based on the seL4 microkernel. NXU never maps or reads tepOS memory. The only
connection between them is a dedicated serial link, and everything crosses it
as framed requests and responses (`drivers/tep/tep_mailbox.c`).

```
NXU QEMU                                        tepOS QEMU
  serial1 (arm64: PL011 @ 0x9040000,              serial1 (PL011 @ 0x9040000)
           i386: 16550 COM2 @ 0x2F8)
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

With `TEP_MB_FEATURE_CRYPTO` in HELLO's flags, tepOS also serves:

| Command | Request | Response |
|---------|---------|----------|
| `SHA256` | 1..240 bytes | 32-byte digest |
| `RANDOM` | u16 n (1..64) | n bytes from tepOS's HMAC-DRBG (seeded from virtio-rng) |
| `KEY_GENERATE` | u8 algorithm (1 = Ed25519) | u32 handle |
| `KEY_PUBLIC` | u32 handle | 32-byte public key |
| `KEY_SIGN` | u32 handle, 1..224 bytes | 64-byte Ed25519 signature |
| `KEY_DELETE` | u32 handle | empty |

Keys are generated inside tepOS's KeyStore and never leave it; NXU only ever
sees handles, public keys and signatures (`drivers/tep/tep_crypto.h`:
`tep_sha256`, `tep_random`, `tep_key_*`, all failing closed). tepOS seals its
key table to disk so keys survive its reboots, but under a sealing key that is
a host file: that store is not a protection boundary against the host yet.

No command reads or writes memory on either side.

## Driver

The port is behind `drivers/tep/tep_uart.h`, one backend per architecture:

- arm64 (`tep_uart_pl011.c`): the Device Tree's first `arm,pl011` is the
  console; a second one becomes `platform->mailbox_uart`, mapped as device
  memory in the higher-half direct map, and must pass the PrimeCell ID check.
- i386 (`tep_uart_16550.c`): COM1 is the console; the mailbox is the 16550 on
  COM2 (I/O ports 0x2F8-0x2FF), which QEMU's pc machine has with a second
  `-serial`. Its presence is probed through the scratch register.

Without the port the link is `absent` and every request is refused.

`tep_mailbox_start()` (arm64: from `kern_init` after the core POST; i386: at
the end of the drivers phase) starts a monitor thread:

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
that, so boot-time tests work with the cooperative scheduler. On i386 without
bootd (`make run-i386` has no disk) `i386_init_run` keeps scheduling instead of
halting while a mailbox port exists, so the monitor keeps running.

## Running it

`make run` and `make run-i386` boot tepOS alongside NXU
(`tools/with_tepos.sh`): it builds tepOS from the TrustedEnclaveProcessor
checkout next to this repository (`TEP_DIR` overrides it), boots it headless,
starts the relay and gives NXU's QEMU the mailbox serial port. Quitting NXU
stops tepOS and the relay. `TEP=0` boots NXU alone; `TEP=1` adds tepOS to any
`make test TEST=<id>` or to `make run-console`. Without the tepOS checkout, or
if it does not build, NXU boots alone and requests to tepOS fail closed.

From a terminal the consoles share it (`tools/console_switch.py`):

| Key | |
|-----|---|
| `s` | show NXU's console (the default) |
| `t` | show tepOS's console |
| Ctrl-C | quit: stops both machines |

Switching clears the screen and replays the recent part of that console; other
keys go to the machine being shown. Both consoles are also logged in full to
`BUILD/tepos-run/nxu-console.log` and `BUILD/tepos-run/console.log`. Both QEMUs
wait for the switcher before booting, so nothing printed early is lost. Run
non-interactively (a script, a pipe) or with `TEP_CONSOLE=stdio`, NXU keeps
stdio as before and tepOS's console only goes to the log.

By hand: in the TrustedEnclaveProcessor repository `make run` boots tepOS with
its serial1 on `/tmp/tepos-mailbox.sock`; `make test TEST=tep-mailbox` gives
NXU's serial1 `/tmp/nxu-mailbox.sock` (`TEP_NXU_SOCK`); join them with that
repository's `tools/mailbox_link.py`. Either side may start first or restart,
and the relay relinks.

Neither QEMU connects out on purpose: QEMU 11.1's socket chardev in client
mode with `reconnect-ms` aborts the whole emulator whenever a connection
attempt fails (`qemu_chr_socket_connected` -> `yank_unregister_function`).
Both QEMUs listen, and only the relay connects.

## Known issue: layout-sensitive `everything` failure

While this driver was added, linking `kern/tests/tep_mailbox_test.o` into
every kernel -- its code never runs outside the `tep-mailbox` test build --
made the `everything` row fail every time with `unified_boot_summary:
UIService Voyager.app FAILED: its event loop is not running`: all five tests
and the boot chime passed, but Voyager's poll counter stopped advancing. The
failure followed that object exactly (bisected across the commits; `main` and
the driver alone passed repeatedly), while 8 KiB of dead `.text` added to
`main` did not reproduce it, so it is not simply the image size. The root cause
was not found; the test file is now compiled only under
`NXU_TEP_MAILBOX_TEST`. Something in the UI session appears sensitive to the
kernel's layout and is worth investigating on its own.

## Testing

`make test-tep-mailbox` (`tools/test_tep_mailbox.sh`) runs tepOS on a scratch
key store disk and sealing key, so it never touches tepOS's real keys. It builds tepOS and the
`tep-mailbox` test kernel, boots both with the relay and runs
`kern/tests/tep_mailbox_test.c`: the protocol and crypto checks (SHA-256
against the FIPS vector, random bytes, key generate/public/sign, unknown
handles) against the live tepOS,
then the harness stops tepOS and the test requires the driver to declare it
unavailable and refuse requests (crypto calls included), then the harness
restarts tepOS and the test requires the driver to reconnect and the key it
made earlier to still be there. See [Testing](../testing.md).
