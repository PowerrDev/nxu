#!/bin/bash
#
# Boots the i386 kernel under QEMU and checks the VM area: the full self-test
# (test=vm), the same boot with different amounts of RAM, and the deliberate
# page faults (vm-fault=...) that must end in the trap layer's #PF report.
# Behind `make test-i386-vm`.
#
# Exit status is the isa-debug-exit device's: a clean shutdown (qemu-exit=1)
# exits 1, and the fatal-trap report path exits 3. Anything else, in
# particular the watchdog firing, is a failure. Plain bash 3.2 (what macOS
# ships), so no associative arrays and no `timeout`.

set -u

KERNEL=${1:?usage: test_i386_vm.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-60}

STATUS_CLEAN=1
STATUS_PANIC=3

failures=0
count=0

# run_case <name> <ram> <boot args> <expected qemu status> <expected output>...
run_case() {
	local name=$1 ram=$2 append=$3 want=$4 output status pattern ok=1
	shift 4

	local attempt
	for attempt in 1 2; do
		output=$(perl -e "alarm $TIMEOUT; exec @ARGV" qemu-system-i386 \
			-kernel "$KERNEL" -m "$ram" \
			-display none -serial stdio -monitor none -no-reboot \
			-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
			-append "$append" 2>&1)
		status=$?

		# QEMU killed by a signal (host hiccup, watchdog) gets one more try;
		# a guest that really hangs hangs twice and still fails.
		[ "$status" -lt 128 ] && break
		printf 'note  %-14s qemu died with status %s, retrying\n' "$name" "$status"
	done

	count=$((count + 1))

	if [ "$status" -ne "$want" ]; then
		ok=0
		printf 'FAIL  %-14s qemu exited %s, expected %s\n' "$name" "$status" "$want"
	fi

	for pattern in "$@"; do
		if ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
			ok=0
			printf 'FAIL  %-14s missing output: %s\n' "$name" "$pattern"
		fi
	done

	if printf '%s\n' "$output" | grep -q "check failed"; then
		ok=0
		printf 'FAIL  %-14s a self-test check failed\n' "$name"
	fi

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-14s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$output" | sed 's/^/      | /'
	fi
}

# The complete self-test: every layer, on the default 512 MiB machine.
run_case selftest 512M "test=vm qemu-exit=1" $STATUS_CLEAN \
	"pmap_init: kernel directory" \
	"i386_init_vm: paging on" \
	"vm_test_pmm: passed" \
	"vm_test_direct_map: passed" \
	"vm_test_vm_kern: passed" \
	"vm_test_kernel_mappings: passed" \
	"vm_test_heap: passed" \
	"vm_test_address_spaces: passed" \
	"vm_shm: self-test passed" \
	"vm_map: self-test passed" \
	"i386_init_vm_selftest: all checks passed" \
	"i386_init: vm self-test passed"

# A plain boot (no test=) still brings the VM up.
run_case plain 128M "qemu-exit=1" $STATUS_CLEAN \
	"i386_init_vm: paging on" \
	"i386_init: boot phases complete"

# The memory map is parsed and clipped for other machine sizes: a small one,
# and one bigger than the 768 MiB direct map (the rest is ignored).
run_case ram-32M 32M "test=vm qemu-exit=1" $STATUS_CLEAN \
	"i386_memory_map_dump: 1 region(s), 31616 KiB usable" \
	"i386_init: vm self-test passed"

run_case ram-1G 1024M "test=vm qemu-exit=1" $STATUS_CLEAN \
	"i386_memory_map_dump: region 0: 0x100000-0x30000000" \
	"i386_init: vm self-test passed"

# Deliberate faults: the report names the access, the region and the cause.
run_case fault-write-ro 512M "test=vm vm-fault=write-ro qemu-exit=1" $STATUS_PANIC \
	"i386_init_vm_selftest: all checks passed" \
	"i386_trap_page_fault: unresolved fault at 0xf" \
	"in vm_kern arena" \
	"(read-only, supervisor)" \
	"panic: unhandled exception 14 (#PF Page Fault)" \
	"cause: write of 0xf0000000 by kernel code: protection violation" \
	"panic: halting"

run_case fault-write-text 512M "test=vm vm-fault=write-text qemu-exit=1" $STATUS_PANIC \
	"in kernel text" \
	"(read-only, supervisor)" \
	"panic: unhandled exception 14 (#PF Page Fault)" \
	"cause: write of 0xc0101000 by kernel code: protection violation"

run_case fault-null 512M "test=vm vm-fault=null qemu-exit=1" $STATUS_PANIC \
	"in null guard page" \
	"panic: unhandled exception 14 (#PF Page Fault)" \
	"cause: read of 0x0 by kernel code: page not present"

run_case fault-unmapped 512M "test=vm vm-fault=unmapped qemu-exit=1" $STATUS_PANIC \
	"in vm_kern arena" \
	"cause: read of 0xf3ffc000 by kernel code: page not present"

run_case fault-user-null 512M "test=vm vm-fault=user-null qemu-exit=1" $STATUS_PANIC \
	"in null guard page" \
	"cause: read of 0x10 by kernel code: page not present"

run_case fault-user-ro 512M "test=vm vm-fault=user-ro qemu-exit=1" $STATUS_PANIC \
	"in user space" \
	"(read-only, user)" \
	"cause: write of 0x401000 by kernel code: protection violation"

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
