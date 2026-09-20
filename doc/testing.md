# Testing

NXU has three layers of tests, and one command that runs the headless ones.

| Layer | Where | How to run |
| --- | --- | --- |
| Boot self-tests | `kern/tests/post*.c` (`kernel_do_post`), run on every boot | any boot; see [Initialization](kern/initialization.md) |
| Kernel test configs | `kern/tests/*_test.c`, each a kernel `CONFIG` selected by a `-DNXU_*_TEST` switch | `make test TEST=<id>`, or `make tests` for the menu |
| i386 suites | `tools/test_i386*.sh`, one `make test-i386-<area>` each | see [i386 port](i386/port.md) |
| Host tests | pure code compiled natively under ASan+UBSan (`tools/audio/`, `tools/btrfs/`) | `make test-audio-host`, `make test-btrfs-host` |

## `make check`

`make check` runs the host tests (`test-audio-host`), builds and boots every
headless test, runs every i386 suite, and prints one table. It exits 0 only if everything passed, so it is what to run
before a commit and what a CI job would call.

```
== make check
ok    default  (13s)
ok    ipc-process  (13s)
...
ok    test-i386-btrfs  (...)

15 check(s), 0 failure(s)
```

```sh
make check                                         # everything
make check CHECK_ONLY="ipc-process default"        # just those ids
make check CHECK_ONLY=i386                         # every i386 suite
make check CHECK_ONLY=test-i386-vm                 # one i386 suite
make check CHECK_I386=0                            # skip the i386 suites
make check CHECK_ONLY=host                         # just the host tests
make check-list                                    # the kernel tests it runs
make check-i386-list                               # the i386 suites it runs
```

A failure prints the tail of the serial log and keeps the scratch directory
(the path is printed); a clean run deletes it. Set `NXU_CHECK_KEEP=1` to keep it
anyway.

### What it does not touch

The tracked `disk.img` and `tools/DiskRoot` are left alone. `tools/check.sh`
copies `tools/DiskRoot` into a scratch directory and builds a fresh disk image
from it for every test, so a test that writes to the disk cannot affect the next.
It also overrides `USER_STAGE_STAMP`: that stamp normally lives in the shared
build directory, and without the override an up-to-date stamp would skip staging
into the scratch root, so a test would boot stale userland binaries.

The i386 suites keep their disks under `BUILD/i386` already.

### How a test is decided

A kernel test passes when its **pass line** appears on the serial console, and
fails on `FAILED`, `panic(` or `kern_fail`, or when its timeout runs out. The
pass line is `TEST_<id>_PASS` in `makedefs/tests.mk`; every test there that sets
it is part of `make check`. The plain boot (`default`, no test switch) passes on
`kern_init: root userspace services active`.

### Tests that play sound

Every boot `make check` runs has a VirtIO Sound device on QEMU's `none` audio
backend, so nothing reaches the host's speakers (`make run` and `make test` use
`coreaudio`; override with `QEMU_AUDIODEV`). A test that plays audio and needs it
checked sets `TEST_<id>_CAPTURE := 1` and `TEST_<id>_VERIFY` in
`makedefs/tests.mk`: `make check` then gives it QEMU's `wav` backend, and once the
pass line has shown, runs the verify command on the recording (`@CAPTURE@` is its
path). The `sound` test uses `tools/audio/verify_capture.py`, which finds the test
tone in the recording and compares it frame by frame with the formula, then finds
the boot chime twice and compares each with `Boot_Audio.wav` sample by sample
(exit 0 only if every check held; a recording that is cut short, silent or altered
fails). QEMU does not finalize the WAV header when it is stopped, so the checker
reads the data to the end of the file. `make test-i386-sound` does the same on
i386, in both interrupt and polling mode, and boots with no sound device. See
[VirtIO Sound](drivers/virtio-sound.md).

The graphical tests (`ui-*`, `windowserver-*`, `about-sevos-process`) and
`journal-crash` need a display or a person, and `xamethyst-process` has no pass
line yet, so none of them are in `make check`.

### Adding a test

1. Add the id to `TEST_IDS` in `makedefs/tests.mk` and define
   `TEST_<id>_GROUP`, `_DESC` and `_CFLAGS`.
2. Have the test print a single line when it passes, and set `TEST_<id>_PASS` to
   it. That is the whole registration for `make check`.
3. For an i386 area, add its `test-i386-<area>` target to `CHECK_I386_TARGETS`.
   Nothing discovers those on its own, so a suite that is not listed is not run.
   A host test target goes in `CHECK_HOST_TARGETS` the same way.

## Checking that `make check` can fail

A runner that cannot fail is worthless. To see it fail, give one test a pass line
that never appears:

```sh
make check CHECK_ONLY=ipc-process "TEST_ipc-process_PASS=never appears" CHECK_TIMEOUT=20
```

This reports `FAIL ipc-process`, keeps the logs and exits non-zero.
