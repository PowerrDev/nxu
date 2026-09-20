# Testing

NXU has three layers of tests, and one command that runs the headless ones.

| Layer | Where | How to run |
| --- | --- | --- |
| Boot self-tests | `kern/tests/post*.c` (`kernel_do_post`), run on every boot | any boot; see [Initialization](kern/initialization.md) |
| Kernel test configs | `kern/tests/*_test.c`, each a kernel `CONFIG` selected by a `-DNXU_*_TEST` switch | `make test TEST=<id>`, or `make tests` for the menu |
| i386 suites | `tools/test_i386*.sh`, one `make test-i386-<area>` each | see [i386 port](i386/port.md) |

## `make check`

`make check` builds and boots every headless test, runs every i386 suite, and
prints one table. It exits 0 only if everything passed, so it is what to run
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

## Checking that `make check` can fail

A runner that cannot fail is worthless. To see it fail, give one test a pass line
that never appears:

```sh
make check CHECK_ONLY=ipc-process "TEST_ipc-process_PASS=never appears" CHECK_TIMEOUT=20
```

This reports `FAIL ipc-process`, keeps the logs and exits non-zero.
