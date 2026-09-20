# i386 interrupts

Hardware interrupts and the periodic tick on the legacy PC: two cascaded
8259 PICs and the 8254 PIT. Shared code keeps using `irq_register()` and the
`timer_*` API unchanged.

## Path of an interrupt

```
device -> PIC line n -> vector 32+n -> i386_trap_handler -> i386_trap_irq
       -> irq_dispatch(n) -> handlers chained with irq_register(n, ...)
       -> pic_eoi(n) -> i386_trap_exit
```

Interrupt gates clear IF, so handlers run with interrupts off and never
nest. The `intid` of `irq_register`/`irq_dispatch` is the PIC line, 0..15.

## Files

| File | Role |
|------|------|
| `kern/i386/pic.{c,h}` | 8259 driver: remap to vectors 32-47, mask/unmask, specific EOI with the cascade, spurious IRQ7/IRQ15, ISR/IRR/IMR reads, ELCR level/edge |
| `kern/i386/timer.{c,h}` | TSC counter (calibrated against PIT channel 2) plus the arm64-shaped API: `timer_start_periodic`, `timer_handle_interrupt`, `timer_get_interrupt_count/rate`, `timer_delay_ms`, `timer_get_ticks/microseconds/frequency` |
| `kern/i386/irq.{c,h}` | strong `i386_trap_irq`, strong `i386_init_interrupts`, IRQ0 tick handler |
| `kern/i386/interrupts_test.c` | strong `i386_init_interrupts_selftest` (`test=interrupts`) |
| `kern/irq/irq.c` | dispatch table; on x86 each line holds a chain of up to 8 handlers |
| `makedefs/i386/interrupts.mk`, `tools/test_i386_interrupts.sh` | sources and `make test-i386-interrupts` |

## Behaviour worth knowing

* **PIC.** All lines are masked at init. The cascade (line 2) is not a
  device line: it opens while any slave line is unmasked and closes with the
  last one. EOI is a *specific* EOI, which is a no-op for a line not in
  service, so a software `int $32+n` is harmless. IRQ7 and IRQ15 are checked
  against the ISR first; a spurious one is counted and not acknowledged (a
  spurious IRQ15 gets only the master's cascade EOI). `int $39`/`int $47`
  therefore never dispatch, so use other lines for software-interrupt tests.
* **Shared lines.** On x86 `irq_register` chains handlers on an intid;
  `irq_dispatch` runs all of them and returns true if any was present, so
  each handler must check its own device and return quietly otherwise. The
  same handler and context twice, or a full chain (8), is refused. arm64
  keeps one owner per INTID and is unchanged. PCI INTx is level triggered:
  a driver claiming a line should call `pic_set_level_triggered(irq, true)`.
* **Tick.** `i386_init_interrupts` starts IRQ0 at 100 Hz (`I386_TIMER_HZ`)
  and leaves IF clear. `timer_start_periodic` may be called again to change
  the rate; it resets the count and opens IRQ0. The IRQ0 handler counts the
  tick and then calls the weak `sched_tick()`. Ticks are counted, not time:
  use `timer_get_microseconds()` for wall time. The TSC is calibrated
  against PIT channel 2 by keeping the shortest of eight ~5 ms countdowns,
  since a stalled host can only make a sample longer.
* **Unclaimed interrupts** (no handler chained) make `i386_trap_irq` return
  false, which the trap layer turns into a fatal report.

## Expectations on other areas

* threads: provide `void sched_tick(void)` (called from IRQ context, IF
  clear; account and *request* preemption), enable IF once the scheduler can
  take a tick (this phase leaves it clear), and do the actual switch from
  `i386_trap_exit` (arm64's `exception_handle` does the same after
  `irq_handle`).
* devices: `pic_unmask(line)` (and `pic_set_level_triggered`) after
  `irq_register` for each device line; use the PCI interrupt line.
* vm: nothing beyond the kernel being mapped where the trap stubs run.

## Testing

`make test-i386-interrupts BUILD_ROOT=<scratch>` boots three cases: the
normal boot, `test=interrupts` (PIC bookkeeping; tick rate; `timer_delay_ms`
against PIT ticks; register/dispatch through `int $37`; three and full
chains; spurious vectors; masking IRQ0; the RTC on IRQ8 through the
cascade), and an unclaimed IRQ still being fatal. QEMU's PIT can drop ticks
on a busy host, so the rate checks are one-sided per window and two-sided
over the best of several, and the runner retries a case up to three times.
