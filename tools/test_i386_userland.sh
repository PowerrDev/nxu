#!/bin/bash
#
# Boots the i386 kernel with "test=userland" and checks that the ELF32 loader
# self-test (kern/loader/elf_selftest.c) ran and passed. Behind
# `make test-i386-userland`; needs no disk and none of the VFS/VM/thread
# machinery. The kernel ends the run through isa-debug-exit: qemu-exit=1 after
# clean phases gives status 1, a failed phase self-test also stops with status
# 1 (i386_fail), so the output is what tells the two apart. Plain bash 3.2.

set -u

KERNEL=${1:?usage: test_i386_userland.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-20}

failures=0

output=$(perl "$(dirname "$0")/qemu_watchdog.pl" "$TIMEOUT" qemu-system-i386 \
	-kernel "$KERNEL" -m 128M \
	-display none -serial stdio -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	-append "test=userland qemu-exit=1" 2>&1)
status=$?

if [ "$status" -ne 1 ]; then
	printf 'FAIL  userland    qemu exited %s, expected 1\n' "$status"
	failures=$((failures + 1))
fi

for pattern in \
	"i386_init: running userland self-test" \
	"ELF32 image cases, 0 failed" \
	"i386_init: userland self-test passed" \
	"i386_init: boot phases complete"; do
	if ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
		printf 'FAIL  userland    missing output: %s\n' "$pattern"
		failures=$((failures + 1))
	fi
done

if printf '%s\n' "$output" | grep -qF -- "FAIL "; then
	printf 'FAIL  userland    a self-test case failed\n'
	failures=$((failures + 1))
fi

if [ "$failures" -eq 0 ]; then
	printf 'ok    userland    %s\n' "$(printf '%s\n' "$output" | grep -F 'ELF32 image cases')"
else
	printf '%s\n' "$output" | sed 's/^/      | /'
fi

[ "$failures" -eq 0 ]
