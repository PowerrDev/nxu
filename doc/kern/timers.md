# Timers: the clock, timer calls, the event timer and the scheduler's deadlines

NXU keeps time and schedules interrupts the way XNU does: separate layers for
keeping time, holding the kernel's pending deadlines, deciding when each CPU's
next interrupt must happen, and programming the hardware. A CPU takes a timer
interrupt only when something is due on it. A CPU that runs a thread takes the
scheduler's tick; an idle CPU takes none.

```
 timed waits, sleeps, any kernel callback          the scheduler
                 |                                      |
                 v                                      v
        timer_call (per-CPU queues)        sched_timer (a tick only while busy)
                 \                                     /
                  \__________ etimer (per CPU) _______/
                         earliest deadline -> arm()
                                   |
                      etimer_hw_t: arm / disarm
                                   |
             Local APIC timer (TSC-deadline or one-shot) | none: periodic tick
                                   |
                     clock: microseconds on the TSC
```

The files:

| Layer | Files | Role |
|---|---|---|
| clock | `kern/clock/clock.{h,c}` | The monotonic clock every deadline is on |
| timer calls | `kern/clock/timer_call.{h,c}` | Per-CPU queues of callbacks due at a time |
| event timer | `kern/clock/etimer.{h,c}` | Each CPU's next interrupt: the earliest timer call or scheduler deadline |
| scheduler | `kern/sched_prism/sched_timer.c` | The scheduler's tick as a deadline, only while a CPU is busy |
| hardware (i386) | `kern/i386/lapic_timer.{h,c}` | Programs each CPU's Local APIC timer |
| bring-up (i386) | `kern/i386/rtclock.c` | Chooses what drives the CPUs and connects the rest |

The layers above the hardware are machine-independent. arm64 still drives
them from its periodic generic-timer tick (periodic mode, below); a deadline
driver for arm64 (the generic timer's compare register is one) plugs into the
same `etimer_hw_t`.

## The clock

`clock_uptime_us()` is microseconds since the free-running counter started:
the TSC on x86, scaled by the frequency `timer_calibrate()` measured against
the PIT; the counter register on arm64. It never goes backwards and does not
follow the wall-clock time. `clock_init()` checks the counter is calibrated and
reports it:

```
clock_init: initializing monotonic clock
clock_init: timebase 2304.520 MHz, 190312 us since the counter started
clock_init: monotonic clock initialized
```

Every deadline below is a value of this clock. On i386 a deadline converts to
a TSC value by rounding up: the first TSC value that reads back as that
microsecond or later. So a TSC-deadline interrupt can never arrive before its
deadline.

## Timer calls

A `struct timer_call` is a function to be called at a deadline. It is
entered on the queue of the CPU that enters it (`timer_call_enter`) and runs
there, from the timer interrupt, with interrupts masked and no lock held. A
callback must be short and must not sleep. The typical one ends a timed wait:
it takes the thread off its wait queue and makes it runnable.

- **Queues.** One per possible CPU (`NXU_MAX_CPUS`), each a doubly-linked list
  sorted by deadline (equal deadlines run in the order they were entered)
  under one spinlock. Insertion walks from the tail, because a new deadline
  is usually the latest. A queue holds a few calls: one per thread in a timed
  wait on that CPU, and any others.
- **Moving.** Entering a call that is pending elsewhere moves it. A call's
  `queue` field names the queue whose lock protects it. A move stores a
  "migrating" marker under the old queue's lock, then finishes under the new
  one's. Anyone who finds the marker waits rather than act, so a call is
  never on two queues and never runs twice for one entry. No code holds two
  queue locks at once, and a queue lock is a leaf.
- **Cancelling.** `timer_call_cancel` takes a pending call off its queue.
  `timer_call_cancel_sync` also waits for a callback already running on
  another CPU (counted in the call's `running` field). After it returns, the
  caller may free the call. A callback may enter its own call again (a
  periodic call). It must not free its call or cancel it synchronously.
- **Moving a whole queue.** `timer_call_queue_migrate(from, to)` moves every
  pending call from one CPU to another, keeping deadlines, for a CPU that
  stops taking timer interrupts. NXU does not take CPUs offline yet, so
  nothing calls it, but the self-test covers it.
- **Counters.** Calls entered, cancelled while pending, run, pending now, and
  the worst lateness seen (`timer_call_stats`).

A zero-filled `struct timer_call` is a valid call that has never been
entered, so one embedded in a zeroed object (a thread) needs only
`timer_call_setup()`.

### Timed waits

`waitq_block_deadline` (`kern/sched_prism/waitq.c`) puts the thread on the
wait queue and then enters the thread's own timer call (`thread->wait_timer`),
both under the wait queue's lock. Because of that order, the deadline can only
find the thread already waiting, or already woken (then it does nothing). The
callback ends the wait the way a signal does: only if it is the one that takes
the thread off the queue. Before returning, the thread cancels the call
synchronously, so no callback still running elsewhere can touch it afterwards.
`sched_thread_terminate` cancels it synchronously too.

`sched_sleep_us`, `sched_sleep_us_interruptible`, `sched_sleep_until_kicked`
and every other timed wait in the kernel go through this. Nothing wakes a
sleep queue as a whole any more.

## The event timer

`etimer` keeps, for each CPU, the deadline its hardware is armed for and the
scheduler's deadline. When something changes, `etimer_resync()` works out the
earliest of the CPU's timer call queue head and its scheduler deadline:

- if it is earlier than what the hardware is armed for, or the hardware is
  not armed, it programs the hardware for it;
- if nothing is due, it disarms the hardware, so an idle CPU sleeps until a
  device, another CPU or a deadline wakes it;
- if it is later (a call was cancelled), it leaves the hardware armed early.
  The interrupt comes, finds nothing due, and the hardware is programmed then.
  That costs a rare spurious interrupt and saves touching the device on every
  cancel. The scheduler's deadline is the exception: it changes on every
  idle/busy switch, so it is always programmed exactly.

`etimer_intr()` is the interrupt. It runs the CPU's due timer calls, gives the
scheduler its tick if that is due, and programs the next deadline. Counters
per CPU (`etimer_stats`, `etimer_dump`): interrupts, interrupts that found
nothing due, programmings, scheduler ticks, worst latency.

The hardware sits behind `etimer_hw_t`, a name and two functions: `arm`
(interrupt this CPU once the clock reaches a deadline) and `disarm`. The event
timer does not know what device it drives. The driver does not know what the
deadlines are for.

### Periodic mode

`etimer_init(0)` puts the event timer in periodic mode, for an architecture
with only a periodic tick (arm64 today), and for i386 when the Local APIC
timer cannot be used. Every timer call then goes on the boot CPU's queue. The
boot CPU's tick runs them: `sched_tick()` does so on arm64, and on i386
`sched_deadline_tick()` does so on each of the PIT's 1000 interrupts a second.
The scheduler keeps its periodic tick on every CPU, as before. Timed waits
then end within a tick of their deadline instead of within microseconds.

## The scheduler's deadlines

The scheduler's policy is written in ticks of `SCHED_TICK_US` (10 ms, 100 Hz).
Quanta and MLFQ allotments are charged one tick at a time to the running
thread. A CPU's queue is boosted every `SCHED_MLFQ_BOOST_INTERVAL_TICKS`.
Starvation and balancing are checked every few ticks. None of that needs a
tick while a CPU is idle: nothing runs to be charged, and its run queue is
empty. With a deadline event timer, `sched_timer.c` therefore asks for a tick
only while the CPU runs a thread:

- **busy to idle:** the scheduler deadline is withdrawn;
- **idle to busy:** the first tick is due `SCHED_TICK_US` later. The ticks the
  CPU skipped while idle are added to its boost counter and its tick count, as
  if it had taken them, so the boost comes when it would have.

`sched_switch` reports the transitions (`sched_timer_dispatch`). The tick
handler keeps the tick's phase. After a stall (the machine was descheduled by
its host) it carries on from now and does not try to catch up.

Two things the old per-CPU tick did implicitly are now explicit:

- **Wait times.** `run_queue_clock()`, which starvation lifts and QoS
  donations are measured on, is derived from the clock (`clock / SCHED_TICK_US`),
  not counted by the boot CPU's tick. It advances the same whether or not any
  CPU is ticking. Making it follow real time exposed a bug in the starvation
  guard: when its scan put a thread that was not yet starved back at the head
  of its line, the enqueue restamped the thread's wait, so the thread that had
  waited longest at each priority restarted its wait at every scan and could
  never be lifted. The scan now keeps the original stamp
  (`sched_starvation_scan`), and the QoS self-test checks it.
- **Idle balancing.** An idle CPU used to pull work from busier CPUs on its own
  ticks. Now a busy CPU with threads waiting that an idle CPU may run asks that
  CPU on each of its ticks: it sets the idle CPU's `balance_requested` and
  sends a reschedule IPI. The idle CPU balances on its way back from the IPI
  (`sched_preempt`).

The idle loop (`sched_idle_loop`) also looks before it sleeps, with interrupts
masked: work queued without an interrupt on its way (the case `sched_switch`
documents, where a thread queued while the run queue was unlocked leaves
`preemption_pending` set and its waker sends no IPI), or a balance request
that came while the CPU was busy. Then it sleeps with `cpu_idle_halt()`,
`sti; hlt` as one step, so anything arriving after the look wakes the sleep. A
periodic tick used to hide both cases within 10 ms. Without one they would
strand a thread until an unrelated interrupt.

## i386: the Local APIC timer

`kern/i386/rtclock.c` runs at the start of the `smp` boot phase, after the
`threads` phase has enabled interrupts with the PIT driving the boot CPU. It
calls `lapic_timer_init()`, which:

1. maps the Local APIC from `IA32_APIC_BASE` if the MP table has not
   (`apic_map_local`, so a uniprocessor boot has one too). Without a Local
   APIC it keeps the PIT;
2. picks a mode: TSC-deadline if CPUID says the CPU has it (leaf 1, ECX bit
   24), otherwise one-shot. For one-shot it measures the count rate against the
   TSC first (the median of three 10 ms countdowns at bus clock / 16);
3. **checks the mode works** before trusting it: it arms a 2 ms deadline and
   requires exactly one interrupt, not early (one-shot may be early by an
   eighth, which the event timer absorbs) and no more than 20 ms late. A
   hypervisor can advertise a mode and not deliver it;
4. hands over with interrupts masked: it masks the PIT's line, puts the event
   timer in deadline mode and starts the boot CPU's timer. Timer calls entered
   on the PIT's tick are on the boot CPU's queue already, which is where its
   timer looks.

Each secondary CPU starts its own timer early in its start-up, before it
enables interrupts (`lapic_timer_cpu_start`). Once all are up,
`i386_rtclock_init_complete` checks every online CPU started one. The vector
is `T_LAPIC_TIMER` (0xEF), one priority class below the IPIs. The handler
acknowledges at the APIC first, so a deadline programmed while the due work
runs can interrupt again.

| Mode | Arm | Disarm | Range |
|---|---|---|---|
| TSC-deadline | `wrmsr IA32_TSC_DEADLINE` (TSC value, rounded up) | write 0 | unlimited |
| one-shot | write the initial count (rounded up, at least 1) | write 0 | 32-bit count, at most 10 s per arming |

Switching the LVT to TSC-deadline mode is a memory-mapped write. The SDM
requires it to be ordered before the first `IA32_TSC_DEADLINE` write, which is
an MSR write, so an `mfence` separates them.

If no mode works, the PIT stays. It runs at `I386_TIMER_HZ` (1000 Hz) and
charges the scheduler every tenth interrupt (`I386_SCHED_HZ`, 100 Hz),
broadcast to the other CPUs as `T_IPI_TICK`. Timer calls run on every
interrupt.

The boot argument `timer.mode=auto|deadline|oneshot|pit` overrides the choice
(`auto`, the default, picks the best mode that works). A requested mode that
is missing or fails its check falls back to the PIT.

### Boot log

A 4-CPU boot under QEMU TCG (no TSC-deadline there), in order:

```
clock_init: initializing monotonic clock
clock_init: monotonic clock initialized
timer_call_init: initializing timer call subsystem
timer_call_init: 8 timer queues initialized, one per possible CPU
lapic_timer_init: initializing per-CPU timers
lapic_timer_report_tsc: TSC is not reported invariant: deadlines assume it keeps a constant rate
lapic_timer_init: this CPU has no TSC-deadline mode
lapic_timer_calibrate: one-shot timer counts at 62.469 MHz (bus clock / 16, median of 3 countdowns)
lapic_timer_probe: checking one-shot mode
lapic_timer_probe: one-shot mode works: interrupted 1305 us after a 2000 us deadline
lapic_timer_init: one-shot mode selected
etimer_init: initializing event timer subsystem
etimer_init: deadline synchronization enabled on the Local APIC timer (one-shot)
lapic_timer_cpu_start: CPU 0 timer ready
lapic_timer_init: PIT stopped: the Local APIC timers drive every CPU from here
sched_timer_init: connecting scheduler deadlines
sched_timer_init: per-CPU quantum timers enabled: a 100 Hz tick while a CPU runs a thread, none while it idles
lapic_timer_cpu_start: CPU 1 timer ready
lapic_timer_cpu_start: CPU 2 timer ready
lapic_timer_cpu_start: CPU 3 timer ready
i386_rtclock_init_complete: all 4 CPU timer(s) operational
i386_rtclock_init_complete: timer subsystem initialization complete: Local APIC timer (one-shot), 4 CPU(s)
```

## Tests

- `timer_call_self_test()` covers deadline order, equal deadlines in entry
  order, nothing early, lateness, rescheduling, cancelling, moving a call and a
  whole queue, and a callback that re-enters its call. It uses private queues
  and a synthetic clock.
- `etimer_self_test(strict)` runs on the live system. Ten timer calls 2 ms
  out must each fire exactly once and never early. A cancelled call must not
  fire. 1 ms sleeps must last at least 1 ms. It logs the spread it measured.
  Strict (a `test=smp` run) also fails on latency over 5 ms with a deadline
  timer, 25 ms periodic. The power-on self-test of every boot fails only on
  latency over 500 ms, a plainly broken timer, so a slow or loaded host cannot
  fail a boot.
- Both run in the `smp` phase self-test: on every boot (power-on, lenient)
  and with `test=smp` (strict).
- `make test-i386-timer` (`tools/test_i386_timer.sh`) boots each way the
  timers can come up and checks the log and the self-tests: one-shot on 4 CPUs
  and on 1, `timer.mode=deadline` without the feature (falls back to the PIT),
  `timer.mode=pit` on 4 CPUs, and a CPU without a Local APIC.
  `I386_TIMER_TEST_DEADLINE_QEMU` adds a TSC-deadline case on an accelerator
  that delivers it (KVM with `-cpu host`).

## Known limits

- **TSC-deadline is untested on real delivery.** QEMU's TCG cannot emulate it,
  and QEMU's Hypervisor.framework accelerator on macOS does not pass it
  through even when the host CPU has it. Every run so far used one-shot. The
  mode check at boot means a TSC-deadline mode that does not work falls back
  rather than hangs.
- **Host timer resolution.** Under QEMU on macOS, timer interrupts arrive
  0.2 to 2 ms after their deadline (the self-test's spread), because QEMU's main
  loop sleeps in millisecond steps there. That is the host's limit, not the
  kernel's. A 120 Hz animation (8.3 ms frames) still gets every frame.
- **The TSC is assumed constant-rate and in step across CPUs**, as the
  whole kernel's clock already assumes. Boot reports whether CPUID marks it
  invariant. Deadlines are armed on the CPU that computed them, so an offset
  between CPUs' TSCs would not misfire a timer, only skew a migrated thread's
  sense of time.
- **Device interrupts stay on the boot CPU** (no IOAPIC), as before.
