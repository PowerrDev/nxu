#!/bin/bash
#
# Boots the i386 kernel under QEMU with -smp 4 and -smp 1 and checks the smp
# phase self-test: the MP table lists the right number of CPUs, all of them
# come online, and an IPI round trip to each secondary actually lands. Behind
# `make test-i386-smp`.
#
# Exit status is the isa-debug-exit device's: a clean shutdown (qemu-exit=1)
# exits 1, a failed phase exits 3. A watchdog kills QEMU that never exits
# (exit 124). Plain bash 3.2 (what macOS ships).

set -u

KERNEL=${1:?usage: test_i386_smp.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-30}

STATUS_CLEAN=1

failures=0
count=0

watchdog() {
	local seconds=$1
	shift
	perl -e '
		my $t = shift @ARGV;
		my $pid = fork();
		die "fork: $!" unless defined $pid;
		if ($pid == 0) { exec @ARGV or die "exec: $!"; }
		$SIG{ALRM} = sub { kill 9, $pid; waitpid($pid, 0); exit 124; };
		alarm $t;
		waitpid($pid, 0);
		exit($? >> 8);
	' "$seconds" "$@"
}

verify() {
	local name=$1 status=$2 want=$3 output=$4 ok=1 pattern
	shift 4

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

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-14s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$output" | sed 's/^/      | /'
	fi
}

boot() {
	local smp=$1 append=$2

	OUTPUT=$(watchdog "$TIMEOUT" qemu-system-i386 -M pc -smp "$smp" \
		-kernel "$KERNEL" -m 512M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-append "$append" 2>&1 < /dev/null)
	STATUS=$?
}

# ---- -smp 4: four CPUs, all online, IPI round trip to each -------------------

boot 4 "test=smp qemu-exit=1 expect-cpus=4"
verify smp-4 "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"mp_table_scan: ACPI MADT lists 4 CPU(s)" \
	"SMP: 4 CPUs online" \
	"cpu1: online" \
	"cpu2: online" \
	"cpu3: online" \
	"IPI round trip to cpu1 ok" \
	"IPI round trip to cpu2 ok" \
	"IPI round trip to cpu3 ok" \
	"cpu count and IPI round trip ok" \
	"smp self-test passed"

# ---- -smp 1: the MP table lists one CPU, smp stays a clean no-op -------------

boot 1 "test=smp qemu-exit=1"
verify smp-1 "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"SMP: one CPU" \
	"smp self-test passed"

# ---- a plain boot (no test=, no disk) still completes with SMP linked in ----

boot 4 "qemu-exit=1"
verify smp-boot "$STATUS" $STATUS_CLEAN "$OUTPUT" \
	"SMP: 4 CPUs online" \
	"i386_init_userland: no block device, staying in kernel-only mode" \
	"i386_init: boot phases complete"

echo
printf '%s case(s), %s failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
