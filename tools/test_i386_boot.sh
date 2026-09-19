#!/bin/bash
#
# End-to-end boot tests for the i386 kernel: the whole stack (paging, interrupts,
# scheduler, VFS, virtio-blk, ELF32 loader, ring 3, syscalls) under QEMU, booting
# real userland from the ext4 image `make i386-disk` builds. Behind
# `make test-i386-boot`.
#
# Each case boots a private copy of the disk (bootd and the services write to
# it), so nothing is left behind. A clean run ends with qemu-exit, status 1
# (isa-debug-exit, code 0); a fatal phase failure or panic is status 3.
# Plain bash 3.2 (what macOS ships).

set -u

KERNEL=${1:?usage: test_i386_boot.sh <kernel.elf> <disk.img>}
DISK=${2:?usage: test_i386_boot.sh <kernel.elf> <disk.img>}
TIMEOUT=${I386_TEST_TIMEOUT:-60}
HERE=$(dirname "$0")

STATUS_CLEAN=1

WORK=$(mktemp -d "${TMPDIR:-/tmp}/i386boot.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

failures=0
count=0

# run_case <name> <with disk: yes|no> <boot args> <expected status> <expected output>...
run_case() {
	local name=$1 with_disk=$2 append=$3 want=$4 output status pattern ok=1
	shift 4

	local disk_args=()

	if [ "$with_disk" = yes ]; then
		cp "$DISK" "$WORK/disk.img"
		disk_args=(-drive "if=none,format=raw,file=$WORK/disk.img,id=d0" -device virtio-blk-pci,drive=d0,disable-modern=on)
	fi

	output=$(perl "$HERE/qemu_watchdog.pl" "$TIMEOUT" qemu-system-i386 -M pc \
		-kernel "$KERNEL" -m 512M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		${disk_args[@]+"${disk_args[@]}"} \
		-append "$append" < /dev/null 2>&1)
	status=$?

	count=$((count + 1))

	if [ "$status" -ne "$want" ]; then
		ok=0
		printf 'FAIL  %-16s qemu exited %s, expected %s\n' "$name" "$status" "$want"
	fi

	for pattern in "$@"; do
		if ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
			ok=0
			printf 'FAIL  %-16s missing output: %s\n' "$name" "$pattern"
		fi
	done

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-16s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$output" | tail -40 | sed 's/^/      | /'
	fi
}

run_case kernel no "test=kernel qemu-exit=1" $STATUS_CLEAN \
	"i386_init_kernel: ramfs mounted at /, /disk mountpoint created" \
	"i386_init_kernel_selftest: NXPC, name tables, ramfs and boot-args ok"

run_case kernel-only no "qemu-exit=1" $STATUS_CLEAN \
	"no block device, staying in kernel-only mode" \
	"i386_init: boot phases complete"

run_case bootd yes "run-seconds=10 qemu-exit=1" $STATUS_CLEAN \
	"i386_init_userland: bootd PID 1 ready for scheduler dispatch" \
	"bootd: userspace service manager started as PID 1" \
	"bootd: com.nxu.logd: started" \
	"bootd: com.nxu.patchd: started" \
	"patchd: integrity repair service started" \
	"run time elapsed, leaving the scheduler"

run_case process-thread yes "process-test=thread qemu-exit=1" $STATUS_CLEAN \
	"thread_process_test: real process spawned its own threads and all ran correctly"

run_case process-ipc yes "process-test=ipc qemu-exit=1" $STATUS_CLEAN \
	"ipctest_b: received \"pong\", exiting" \
	"ipc_process_test: two-process bootstrap register/lookup round trip passed"

run_case process-socket yes "process-test=socket qemu-exit=1" $STATUS_CLEAN \
	"socket_process_test: two-process streaming socket round trip passed"

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
