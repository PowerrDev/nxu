#!/bin/bash
#
# Boots the i386 kernel under QEMU in each way its timers can come up and
# checks the event timer chose what it should, every CPU's timer started,
# and the timer self-tests (timer call queues, live deadlines, timed sleeps)
# pass. Behind `make test-i386-timer`.
#
#   oneshot-smp    4 CPUs, Local APIC one-shot mode (QEMU's TCG has no
#                  TSC-deadline mode, so this is what "auto" picks there)
#   oneshot-up     1 CPU: the Local APIC is found from its MSR, not the MP table
#   deadline-off   timer.mode=deadline on a CPU without it: falls back to the PIT
#   pit-smp        timer.mode=pit, 4 CPUs: the periodic event timer and the
#                  tick broadcast to the secondaries
#   no-apic        a CPU without a Local APIC: the PIT
#
# TSC-deadline mode itself needs a CPU (or accelerator) that delivers it;
# I386_TIMER_TEST_DEADLINE_QEMU names a QEMU command line that does, e.g.
# "qemu-system-x86_64 -accel kvm -cpu host", to add a deadline-smp case.
#
# Exit status is the isa-debug-exit device's: a clean shutdown (qemu-exit=1)
# exits 1, a failed phase exits 3. A watchdog kills QEMU that never exits
# (exit 124). Plain bash 3.2 (what macOS ships).

set -u

KERNEL=${1:?usage: test_i386_timer.sh <kernel.elf>}
TIMEOUT=${I386_TEST_TIMEOUT:-60}
DEADLINE_QEMU=${I386_TIMER_TEST_DEADLINE_QEMU:-}

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

	if printf '%s\n' "$output" | grep -qE 'FAIL |panic'; then
		ok=0
		printf 'FAIL  %-14s a check failed:\n' "$name"
		printf '%s\n' "$output" | grep -E 'FAIL |panic' | sed 's/^/      ! /'
	fi

	if [ "$ok" -eq 1 ]; then
		printf 'ok    %-14s\n' "$name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$output" | grep -E 'clock_init|lapic_timer|etimer|sched_timer|rtclock|timer_call|timer self' | sed 's/^/      | /'
	fi
}

# boot <cpu model> <cpus> <boot args>: the timers phase is "smp", whose self-test runs the timer tests.
boot() {
	local cpu=$1 smp=$2 append=$3

	OUTPUT=$(watchdog "$TIMEOUT" qemu-system-i386 -cpu "$cpu" -M pc -smp "$smp" \
		-kernel "$KERNEL" -m 512M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-append "test=smp qemu-exit=1 expect-cpus=$smp $append" 2>&1 < /dev/null)
	STATUS=$?
}

common=(
	"clock_init: monotonic clock initialized"
	"timer_call_init: 8 timer queues initialized, one per possible CPU"
	"etimer_init: initializing event timer subsystem"
	"sched_timer_init: connecting scheduler deadlines"
	"i386_init_smp_selftest: timer self-tests passed"
	"i386_init: smp self-test passed"
)

lapic_common=(
	"${common[@]}"
	"lapic_timer_init: initializing per-CPU timers"
	"lapic_timer_init: PIT stopped: the Local APIC timers drive every CPU from here"
	"sched_timer_init: per-CPU quantum timers enabled"
)

pit_common=(
	"${common[@]}"
	"etimer_init: no deadline timer: timer calls run on the boot CPU's periodic tick"
	"i386_rtclock_init: the PIT drives every CPU at 1000 Hz (scheduler every 10 interrupts)"
	"timer subsystem initialization complete: periodic tick"
)

boot "qemu32,+nx" 4 ""
verify oneshot-smp "$STATUS" "$STATUS_CLEAN" "$OUTPUT" "${lapic_common[@]}" \
	"lapic_timer_probe: one-shot mode works" \
	"lapic_timer_init: one-shot mode selected" \
	"etimer_init: deadline synchronization enabled on the Local APIC timer (one-shot)" \
	"lapic_timer_cpu_start: CPU 0 timer ready" \
	"lapic_timer_cpu_start: CPU 1 timer ready" \
	"lapic_timer_cpu_start: CPU 2 timer ready" \
	"lapic_timer_cpu_start: CPU 3 timer ready" \
	"i386_rtclock_init_complete: all 4 CPU timer(s) operational" \
	"timer subsystem initialization complete: Local APIC timer (one-shot), 4 CPU(s)"

boot "qemu32,+nx" 1 ""
verify oneshot-up "$STATUS" "$STATUS_CLEAN" "$OUTPUT" "${lapic_common[@]}" \
	"lapic_timer_init: one-shot mode selected" \
	"lapic_timer_cpu_start: CPU 0 timer ready" \
	"i386_rtclock_init_complete: all 1 CPU timer(s) operational"

boot "qemu32,+nx" 1 "timer.mode=deadline"
verify deadline-off "$STATUS" "$STATUS_CLEAN" "$OUTPUT" "${pit_common[@]}" \
	"lapic_timer_init: this CPU has no TSC-deadline mode" \
	"lapic_timer_init: no usable Local APIC timer mode: keeping the PIT"

boot "qemu32,+nx" 4 "timer.mode=pit"
verify pit-smp "$STATUS" "$STATUS_CLEAN" "$OUTPUT" "${pit_common[@]}" \
	"lapic_timer_init: timer.mode=pit: keeping the PIT" \
	"sched_timer_init: periodic event timer: the scheduler keeps its 100 Hz tick on every CPU"

boot "qemu32,+nx,-apic" 1 ""
verify no-apic "$STATUS" "$STATUS_CLEAN" "$OUTPUT" "${pit_common[@]}" \
	"lapic_timer_init: no Local APIC: keeping the PIT"

if [ -n "$DEADLINE_QEMU" ]; then
	OUTPUT=$(watchdog "$TIMEOUT" $DEADLINE_QEMU -M pc -smp 4 -kernel "$KERNEL" -m 512M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-append "test=smp qemu-exit=1 expect-cpus=4 timer.mode=deadline" 2>&1 < /dev/null)
	STATUS=$?
	verify deadline-smp "$STATUS" "$STATUS_CLEAN" "$OUTPUT" "${lapic_common[@]}" \
		"lapic_timer_probe: TSC-deadline mode works" \
		"lapic_timer_init: TSC-deadline mode selected" \
		"i386_rtclock_init_complete: all 4 CPU timer(s) operational"
fi

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"
[ "$failures" -eq 0 ]
