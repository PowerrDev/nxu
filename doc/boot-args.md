# Boot arguments and NVRAM

NXU parses boot arguments before the higher-half transition from the Device
Tree `/chosen/bootargs` property. QEMU populates that property through `-append`.
The Makefile exposes it as `BOOT_ARGS`:

```sh
make run-console BOOT_ARGS="-v -no-gpu"
```

## Current firmware limitation

The documented NXU QEMU path boots with `-kernel`, not a UEFI firmware stack.
There are therefore no UEFI Runtime Services from which the kernel can read or
persist firmware variables. The current NVRAM layer deliberately exposes a
read-only Device Tree backend: `nvram_get("boot-args", ...)` returns the boot
arguments and `nvram_set()` returns `NVRAM_STATUS_READ_ONLY`.

A future firmware-backed implementation can replace this backend without
changing boot-argument consumers.

`nzh` includes `nvram` to inspect the current `boot-args` value. An assignment
such as `nvram boot-args="-v"` explains how to pass that value on the next QEMU
launch; it cannot persist it from inside the guest while the firmware backend
is read-only.

## Supported arguments

- Verbose output is disabled by default. `-v` explicitly enables it and is
  passed through to userland services. `log.verbose=1` also enables detail;
  `log.verbose=0` keeps milestone output only.
- `proc.max=N` requests a process table limit, bounded by available memory and
  the kernel's 256-process storage ceiling. The default scales with configured
  CPUs and available memory.
- `log.stats_ms=N` sets recurring real kernel snapshot frequency in milliseconds
  (default 1000, clamped to 250–60000; 0 disables snapshots).
- `log.trace=0` disables individual syscall traces; interval call/error counts
  remain available in snapshots. Hot polling, I/O and UI calls use interval
  counts rather than individual traces.
- `-x` enables safe mode and suppresses optional root daemons.
- `-no-crashd` disables the crash-report daemon.
- `-no-crash-ui` disables crash dialogs while reports continue to be saved.
- `-no-gpu` prevents VirtIO GPU attachment.
- `-no-input` prevents VirtIO keyboard/mouse attachment.
- `-no-block` prevents VirtIO block attachment and therefore disk userspace.
- `-no-sound` prevents VirtIO Sound attachment.
- `-no-chime` keeps the boot chime from playing (see
  [VirtIO Sound](drivers/virtio-sound.md)).
- `-no-logd` prevents `bootd` from starting `logd`.
- `-no-patchd` prevents `bootd` from starting `patchd`.
- `recovery.volume=DEVICE_INDEX` mounts the selected ext4 device or partition
  at `/Volumes/Recovery` for NSW preparation. It cannot remount a device
  already in use.
- `dyrld.aslr=0` disables runtime framework sliding for developer tests.
  The default is 1 and uses randomness from tepOS when an enclave mailbox is
  available. Machines without a mailbox use fixed framework slides; if a
  mailbox is present but tepOS is unavailable, dynamic launches fail closed.

The parser also supports `name=value` tokens for later kernel tunables.

## Userland desktop integration

`ui.userland=1` selects the independent trusted kernel login path and skips the
legacy kernel desktop. With an enclave UART present, ordinary display/input
operations remain excluded until tepOS accepts authentication. An unreachable
enclave leaves the desktop locked; no enclave UART retains the existing
no-login policy. Recovery boot bypasses this desktop login path.

`make run` boots this mode by default. bootd starts the installed WindowServer
and DesktopService jobs; DesktopService requires `ui.userland=1`. The kernel
image excludes the legacy WindowServer and UIService archives. `-no-desktop`
disables DesktopService, while safe mode keeps both essential GUI services.
Legacy GUI test targets retain their kernel framework configuration.

Normal desktop boot also sets `log.trace=0 log.stats_ms=0` to avoid continuous
syscall traces and diagnostic snapshots feeding logd. Ordinary logs and crash
reports remain enabled. For debugging, use
`make run BOOT_ARGS="log.trace=1 log.stats_ms=1000"`.

The dynamic desktop reads `ui.scale=1000` as 1× and `ui.scale=2000` as 2×.
Values from 500 through 4000 are accepted; invalid values use 1000. Scaling
applies to desktop layout and pointer movement. The integration mode supports
display dimensions up to 3840 by 2160 pixels.

`update.test.fail_after=N` injects a boot-installer failure after N changed
files for rollback tests. It affects only an explicitly queued installation.

Named GPT `NXU System` and `NXU Recovery` partitions are discovered automatically.
`recovery.volume` remains a legacy override for a registered block-device index.
The i386 desktop uses one persistent GPT SSD by default; `I386_PARTITIONED_DISK=0`
selects the old raw-disk layout.
