# Boot arguments and NVRAM

NXU parses boot arguments before the higher-half transition from the Device
Tree `/chosen/bootargs` property. QEMU populates that property through `-append`.
The Makefile exposes it as `BOOT_ARGS`:

```sh
make run BOOT_ARGS="-v -no-gpu"
```

## Current firmware limitation

The documented NXU QEMU path boots with `-kernel`, not a UEFI firmware stack.
There are therefore no UEFI Runtime Services from which the kernel can read or
persist firmware variables. The current NVRAM layer deliberately exposes a
read-only Device Tree backend: `nvram_get("boot-args", ...)` returns the boot
arguments and `nvram_set()` returns `NVRAM_STATUS_READ_ONLY`.

A future firmware-backed implementation can replace this backend without
changing boot-argument consumers.

## Supported arguments

- `-v` enables verbose-boot policy for the future splash/console handoff.
- `-x` enables safe mode and suppresses optional root daemons.
- `-no-gpu` prevents VirtIO GPU attachment.
- `-no-input` prevents VirtIO keyboard/mouse attachment.
- `-no-block` prevents VirtIO block attachment and therefore disk userspace.
- `-no-logd` prevents `bootd` from starting `logd`.
- `-no-patchd` prevents `bootd` from starting `patchd`.

The parser also supports `name=value` tokens for later kernel tunables.
