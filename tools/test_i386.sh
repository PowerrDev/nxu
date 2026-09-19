#!/bin/bash
#
# Boots the i386 kernel under QEMU once per case and checks the serial output
# and QEMU's exit status. Behind `make test-i386`.
#
# Exit status is the isa-debug-exit device's: a clean shutdown (qemu-exit=1)
# exits 1, and the fatal-trap report path (which stops the machine through
# the same device) exits 3. Anything else -- in particular the watchdog
# firing -- is a failure. Plain bash 3.2 (what macOS ships).

set -u

KERNEL=${1:?usage: test_i386.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-15}

STATUS_CLEAN=1
STATUS_PANIC=3

failures=0
count=0

# run_case <name> <boot args> <expected qemu status> <expected output>...
run_case() {
	local name=$1 append=$2 want=$3 output status pattern ok=1
	shift 3

	output=$(perl -e "alarm $TIMEOUT; exec @ARGV" qemu-system-i386 \
		-kernel "$KERNEL" -m 128M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-append "$append" 2>&1)
	status=$?

	count=$((count + 1))

	if [ "$status" -ne "$want" ]; then
		ok=0
		printf 'FAIL  %-10s qemu exited %s, expected %s\n' "$name" "$status" "$want"
	fi

	for pattern in "$@"; do
		if ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
			ok=0
			printf 'FAIL  %-10s missing output: %s\n' "$name" "$pattern"
		fi
	done

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-10s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$output" | sed 's/^/      | /'
	fi
}

run_case selftest "qemu-exit=1" $STATUS_CLEAN \
	"i386_gdt_init: 8 descriptors" \
	"i386_idt_init: 256 gates" \
	"trap self-test passed"

run_case div0 "trap-test=div0" $STATUS_PANIC \
	"panic: unhandled exception 0 (#DE Divide Error)" \
	"backtrace: #0" \
	"panic: halting"

run_case ud2 "trap-test=ud2" $STATUS_PANIC \
	"panic: unhandled exception 6 (#UD Invalid Opcode)"

run_case gp "trap-test=gp" $STATUS_PANIC \
	"panic: unhandled exception 13 (#GP General Protection Fault)" \
	"error code 0x1238" \
	"cause: selector 0x1238, GDT, index 583"

run_case np "trap-test=np" $STATUS_PANIC \
	"panic: unhandled exception 11 (#NP Segment Not Present)" \
	"cause: selector 0x38, GDT, index 7"

run_case df "trap-test=df" $STATUS_PANIC \
	"panic: unhandled exception 8 (#DF Double Fault)" \
	"ss=0x10"

run_case vector "trap-test=vector" $STATUS_PANIC \
	"(#GP General Protection Fault)" \
	"cause: selector 0x40a, IDT, index 129"

run_case irq "trap-test=irq" $STATUS_PANIC \
	"panic: unexpected interrupt vector 33 (hardware interrupt)"

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
