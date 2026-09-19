#!/bin/bash
#
# Boots the i386 kernel with test=threads under QEMU and checks that every
# stage of the threads self-test ran and passed, and that ring 3 faults are
# contained. Behind `make test-i386-threads`.
#
# Exit status is the isa-debug-exit device's: a clean shutdown (qemu-exit=1)
# exits 1. Anything else, in particular the watchdog firing, is a failure.
# Plain bash 3.2 (what macOS ships). QEMU handles SIGALRM itself, so the
# watchdog is a forked perl that SIGKILLs it.

set -u

KERNEL=${1:?usage: test_i386_threads.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-30}

STATUS_CLEAN=1

failures=0
count=0

# run_case <name> <boot args> <expected qemu status> <expected output>...
run_case() {
	local name=$1 append=$2 want=$3 output status pattern ok=1
	shift 3

	output=$(perl -e '
		my $timeout = shift;
		my $pid = fork();
		die "fork failed" unless defined $pid;
		if ($pid == 0) { exec @ARGV or die "exec failed"; }
		$SIG{ALRM} = sub { kill 9, $pid; };
		alarm $timeout;
		waitpid($pid, 0);
		exit($? >> 8);
	' "$TIMEOUT" qemu-system-i386 \
		-kernel "$KERNEL" -m 128M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-append "$append" < /dev/null 2>&1)
	status=$?

	count=$((count + 1))

	if [ "$status" -ne "$want" ]; then
		ok=0
		printf 'FAIL  %-12s qemu exited %s, expected %s\n' "$name" "$status" "$want"
	fi

	for pattern in "$@"; do
		if ! printf '%s\n' "$output" | grep -qF -- "$pattern"; then
			ok=0
			printf 'FAIL  %-12s missing output: %s\n' "$name" "$pattern"
		fi
	done

	if printf '%s\n' "$output" | grep -qF -- "FAIL"; then
		ok=0
		printf 'FAIL  %-12s a self-test reported FAIL\n' "$name"
	fi

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-12s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$output" | sed 's/^/      | /'
	fi
}

# The phase alone: scheduler up, nothing else.
run_case boot "qemu-exit=1" $STATUS_CLEAN \
	"i386_init_threads: scheduler up" \
	"i386_init: boot phases complete"

# The whole self-test, stage by stage.
run_case selftest "test=threads qemu-exit=1" $STATUS_CLEAN \
	"i386_init_threads_selftest: run queue and MLFQ self-tests passed" \
	"i386_init_threads_selftest: machine_thread contracts passed" \
	"i386_init_threads_selftest: kernel threads passed (log ABCABCABCABC)" \
	"i386_init_threads_selftest: MLFQ ticks and preempt-on-return passed" \
	"i386_init_threads_selftest: system-call request path passed" \
	"i386_init_threads_selftest: hello from ring 3" \
	"syscall: exit(42)" \
	"i386_init_threads_selftest: ring 3 entry and system calls passed" \
	"terminated by #GP General Protection Fault" \
	"terminated by #UD Invalid Opcode" \
	"terminated by #DE Divide Error" \
	"i386_init_threads_selftest: ring 3 faults terminate only the offender" \
	"syscall: exit(7)" \
	"i386_init_threads_selftest: ring 3 preemption passed (log uuuVuuuuu)" \
	"i386_init: threads self-test passed" \
	"i386_init: boot phases complete"

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
