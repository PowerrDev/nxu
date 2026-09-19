#!/bin/bash
#
# Boots the i386 kernel under QEMU and checks the interrupts area: the PIC
# and PIT bring-up in a normal boot, the "test=interrupts" self-test (tick
# rate, timer_delay_ms, irq_register/irq_dispatch, chained handlers, masking,
# spurious interrupts, the slave PIC), and that an interrupt nobody claimed
# is still fatal. Behind `make test-i386-interrupts`.
#
# Exit status is the isa-debug-exit device's: a clean shutdown (qemu-exit=1)
# exits 1, and the fatal-trap report path exits 3. Anything else -- in
# particular the watchdog firing -- is a failure. A case is retried a couple
# of times because QEMU's PIT can lose ticks when the host is busy (see
# interrupts_test.c); a real defect fails every attempt. Plain bash 3.2.

set -u

KERNEL=${1:?usage: test_i386_interrupts.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-30}
ATTEMPTS=${I386_TEST_ATTEMPTS:-3}

STATUS_CLEAN=1
STATUS_PANIC=3

failures=0
count=0

# run_once <boot args> <expected status> <patterns...>; sets $output and returns
# 0 if every pattern is present (a leading "!" means it must be absent).
run_once() {
	local append=$1 want=$2 status pattern ok=0
	shift 2

	output=$(perl "$(dirname "$0")/qemu_watchdog.pl" "$TIMEOUT" qemu-system-i386 \
		-kernel "$KERNEL" -m 128M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-append "$append" 2>&1)
	status=$?

	problems=""

	if [ "$status" -ne "$want" ]; then
		ok=1
		problems="${problems}qemu exited $status, expected $want
"
	fi

	for pattern in "$@"; do
		if [ "${pattern#!}" != "$pattern" ]; then
			if printf '%s\n' "$output" | grep -qF -- "${pattern#!}"; then
				ok=1
				problems="${problems}unexpected output: ${pattern#!}
"
			fi
		elif ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
			ok=1
			problems="${problems}missing output: $pattern
"
		fi
	done

	return $ok
}

# run_case <name> <boot args> <expected qemu status> <pattern>...
run_case() {
	local name=$1 append=$2 want=$3 attempt=1
	shift 3

	count=$((count + 1))

	while :; do
		if run_once "$append" "$want" "$@"; then
			if [ "$attempt" -gt 1 ]; then
				printf 'ok    %-12s (attempt %d)\n' "$name" "$attempt"
			else
				printf 'ok    %-12s\n' "$name"
			fi
			return
		fi

		[ "$attempt" -ge "$ATTEMPTS" ] && break
		attempt=$((attempt + 1))
	done

	failures=$((failures + 1))
	printf 'FAIL  %-12s\n' "$name"
	printf '%s' "$problems" | sed 's/^/      ! /'
	printf '%s\n' "$output" | sed 's/^/      | /'
}

run_case boot "qemu-exit=1" $STATUS_CLEAN \
	"i386_init_interrupts: pic remapped to vectors 32-47, all lines masked" \
	"i386_init_interrupts: pit channel 0 armed at 100 Hz on IRQ 0, interrupts disabled" \
	"i386_init: boot phases complete" \
	"!interrupts self-test"

run_case selftest "test=interrupts qemu-exit=1" $STATUS_CLEAN \
	"i386_init: running interrupts self-test" \
	"ok: hardware mask registers agree with the driver" \
	"ok: unmasking a slave line opens the cascade" \
	"ok: IRQ0 ticks at the requested rate" \
	"ok: timer_start_periodic resets the interrupt count" \
	"ok: timer_delay_ms(100) spans 100 ms of PIT ticks" \
	"ok: int \$37 reaches the handler with intid 5" \
	"ok: every chained handler runs" \
	"ok: a full chain unregisters cleanly" \
	"ok: spurious IRQ7 and IRQ15 are recognised" \
	"ok: a real PIT interrupt reaches a handler registered with irq_register" \
	"ok: a masked line delivers nothing" \
	"ok: a slave line (IRQ8) is delivered repeatedly, so the cascade EOI works" \
	"ok: a masked slave line delivers nothing" \
	"ok: every interrupt that was taken has been acknowledged" \
	"ok: interrupts are disabled again on return" \
	"0 failure(s)" \
	"i386_init: interrupts self-test passed" \
	"!FAIL"

run_case unclaimed "trap-test=irq qemu-exit=1" $STATUS_PANIC \
	"i386_trap_irq: no handler for IRQ 1" \
	"panic: unexpected interrupt vector 33 (hardware interrupt)"

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
