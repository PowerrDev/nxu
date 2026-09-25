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

With `TEP_MB_FEATURE_AUTH`, the passcode, and with `TEP_MB_FEATURE_BOOT`,
signed boot manifests:

| Command | Request | Response |
|---------|---------|----------|
| `AUTH_SET` | u8 old length (0 if none set), old passcode, new passcode (4..64 bytes each) | empty |
| `AUTH_VERIFY` | passcode (4..64 bytes) | empty |
| `AUTH_STATUS` | empty | u8 set, u8 failures, u8 locked, u8 reserved, u32 seconds to wait |
| `BOOT_VERIFY` | 136-byte manifest, 32-byte measured SHA-256 | u32 image version |

Their refusals are statuses: `DENIED` (wrong passcode, with a u8 failure
count; or a refused manifest, with a u8 reason: 1 not a manifest, 2 bad
signature, 3 signed for another image), `RETRY_LATER` (u32 seconds),
`LOCKED`, `ROLLBACK` (u32 minimum version) and `NOT_FOUND` (no passcode set).
`drivers/tep/tep_crypto.h` has `tep_auth_set`, `tep_auth_verify`,
`tep_auth_status` and `tep_boot_verify`, each failing closed and checking
the exact payload length of every answer.

No command reads or writes memory on either side.

## Passcode

tepOS's AuthenticationService keeps only a salted Argon2id hash of the
passcode, sealed in its key store, and does all the checking: the failure
count is written to disk before a passcode is compared, from the fifth
failure in a row each attempt waits (1 min, 5 min, 15 min, 1 h, 1 h), and the
tenth locks the passcode until a recovery reset that only whoever runs tepOS's
machine can make. A right passcode clears the count. NXU keeps no copy of the
passcode or its hash and wipes its request buffer after sending.

Limits worth knowing: the passcode crosses the serial link in the clear (the
link is checked for corruption, not encrypted), the delays use tepOS's RTC,
which is the host's clock, and the key store is sealed with a host file. So
this protects against guessing through NXU, not against the host.

## Login screen

When the mailbox port exists, the desktop is locked behind the passcode
(`drivers/video/ui_service_login.c`). Before the desktop app starts, the UIService
host runs `UIServiceRunLogin` (UIService.framework's `ui-login`), a fullscreen
screen over the blurred wallpaper:

- **First boot** (tepOS has no passcode): "welcome to sevOS" in Borel, the
  script face armOS's setup greets with, then "Create a passcode" (typed
  twice). It is set with `tep_auth_set` and the desktop starts.
- **Every later boot:** "Enter your passcode", checked with `tep_auth_verify`.
  A wrong one shakes the card; tepOS's delays show as a countdown that blocks
  typing, and a locked passcode says it needs a recovery reset on tepOS's
  machine.
- **tepOS unreachable:** "Waiting for the Trusted Enclave", asking again every
  second. It never unlocks without tepOS.

Only tepOS's OK unlocks. Any other end of the login screen keeps the desktop
locked, and without the mailbox port (`TEP=0`, the `make check` rows) there is
no login screen. Keys reach UIService as key events from
`drivers/input/keyboard.c`'s press queue, and the kernel log records only what
tepOS answered.

Like the passcode itself, this stops someone at the keyboard, not someone with
the disk image or the host: the disk is not encrypted, and a modified kernel
can skip the screen.

## Measured bootd (reported, not enforced)

The disk build signs bootd for tepOS: `tools/sign_boot_image.sh` runs tepOS's
`build/boot_sign` with the host boot-signing key in the TrustedEnclaveProcessor
checkout (`build/boot-signing.key`, made on first use with the public key
tepOS is built with) and writes `System/Library/CoreServices/bootd.manifest`
next to bootd. The manifest's version is the commit count, so it only moves
forward along the history. The key never enters this repository or the disk.
The manifest is ignored by git: it is only valid with this machine's key.
Without the tepOS checkout bootd stays unsigned.

When the mailbox port exists, `tep_boot_check_start()` (`drivers/tep/tep_boot.c`)
starts a thread that waits for bootd on disk, checks libk's SHA-256
(`libk/sha256.c`, FIPS 180-4) against its known answer, hashes bootd in 8 KiB
chunks and, once tepOS offers `TEP_MB_FEATURE_BOOT`, sends the manifest and
digest. It logs one of:

```
tep_boot_check_thread: bootd verified by tepOS (not enforced): version 342
tep_boot_check_thread: bootd NOT verified by tepOS (not enforced): signed for another image
tep_boot_check_thread: bootd NOT verified by tepOS (not enforced): rollback, minimum version 342
tep_boot_check_thread: bootd not checked: the disk has no signed manifest for it
```

tepOS keeps the highest version it accepted, so after running a newer build
an older checkout's bootd is reported as a rollback.

Nothing acts on the verdict. bootd starts whatever it is, and the check does
not delay it. The measurement is taken by the same kernel that loads bootd,
from the same disk, so a modified kernel could lie about it. Enforcement needs
a trusted boot chain that measures NXU itself, which does not exist yet.

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
handles) against the live tepOS; the passcode (set, change, refusals, five
failures starting the first delay, a right passcode refused during it); and
boot policy: the kernel's own bootd check must say verified, then a bootd with
one byte changed, a manifest changed after signing, a bad magic and a
manifest signed as version 1 must be refused for the right reason (the
harness stages `bootd.tampered` and `bootd.v1.manifest` on the scratch disk).
Then the harness stops tepOS and the test requires the driver to declare it
unavailable and refuse requests (crypto, passcode and boot calls included),
then the harness restarts tepOS and the test requires the driver to reconnect,
the key it made earlier to still be there, and the passcode failures and delay
to have survived the restart; it waits the delay out and requires the right
passcode to clear them. See [Testing](../testing.md).
