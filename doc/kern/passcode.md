# Login passcode

The passcode that unlocks the desktop is kept by the kernel itself, on the
system volume (`kern/security/passcode.c`). Nothing outside the machine NXU
runs on is involved: no enclave, no second QEMU. tepOS
([Trusted Enclave Mailbox](../drivers/tep-mailbox.md)) is an optional
experiment and plays no part in it.

## Who uses it

| Caller | What it does |
|--------|--------------|
| Setup assistant (`drivers/video/setup_assistant.c`) | First boot: `passcode_set` with the password chosen for the new account. That is what unlocks; the account's `/etc/shadow` line is written next to it with the same password. |
| Lock screen (`drivers/video/keystore_manager.c`) | Every later boot of the desktop (`ui.userland=1`): `passcode_status` until the system volume is there, then `passcode_verify`. |
| UIService login (`drivers/video/ui_service_login.c`) | The in-kernel UIService desktop (`UISERVICE=1`): the same three calls behind `UIServiceRunLogin`. |
| SAP (`kern/security/sap.c`) | The privacy prompt before a protected delete: `passcode_verify`. |

## The record

`/var/db/passcode` on the system volume (`/Volumes/mainOS/var/db/passcode` to
the kernel), owned by root, mode 0600. It is text, padded with newlines to
256 bytes so that every update after the first overwrites it in place,
within one block, without truncating it:

```
nxu-passcode 1
hash pbkdf2_sha256$20000$<salt, 16 bytes hex>$<hash, 32 bytes hex>
failures 0
last 0
```

The hash is PBKDF2-HMAC-SHA256 (`libk/pbkdf2.c`) in the same format as
`/etc/shadow` (`frameworks/lib/auth.c`), with a salt from the kernel's random
number generator ([Random numbers](random.md)). The passcode itself is never
stored, and the buffers that held it are wiped.

No file, or an empty one, means no passcode is set: the next desktop boot runs
the setup assistant (or the lock screen's own "create a password"). A file
that is there but cannot be read as a record means **locked**: a damaged
record never turns into "no passcode".

## Policy

The same as tepOS applied when it held the passcode:

- The failure count is written to disk, and synced, **before** a passcode is
  compared: turning the machine off mid-check does not undo a failure. If the
  count cannot be written, the passcode is not compared.
- From the fifth failure in a row each attempt waits: 1 min, 5 min, 15 min,
  1 h, 1 h (measured with the RTC; setting the clock back restarts the wait).
- The tenth failure locks the passcode.
- A right passcode clears the count.
- Changing a set passcode needs the old one, under the same count and waits.

Until the system volume is mounted the store answers `UNAVAILABLE`: the lock
screen says "Starting up..." and keeps asking. It never unlocks because the
store could not answer.

## Forgotten or locked

Remove the record and the next boot asks for a new passcode, as on first
boot:

- From Recovery's shell: `rm /Volumes/mainOS/var/db/passcode`.
- Erase All Content and Settings removes it with everything else on the
  system volume.
- From the host, on a disk image: `debugfs -w -R "rm /var/db/passcode"`.

## Limits

Like any passcode checked by the machine it protects, this stops someone at
the keyboard, not someone holding the disk: the volume is not encrypted, the
record can be removed from Recovery or the host, and a modified kernel can
skip the screen. Root can also remove the record from a running system, and
until directory-level permission checks land (create/unlink,
[Unix security](../architecture/unix-security.md)) so can any process that can
reach `/var/db`.

## Tests

Test kernels (`make test`, `make check`) are built with `NXU_LOGIN_UNLOCKED`,
which skips the UIService login screen: nobody is at the keyboard to type a
passcode. Only the `desktop` row, what `make run` boots, keeps the login
(`TEST_desktop_LOCKED`). The flag is a compile-time switch, never a boot
argument, so a shipped kernel cannot be told to skip the lock screen.
