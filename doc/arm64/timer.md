# Generic Timer

NXU uses the ARM generic **physical** timer at EL1 as its only interrupt
source and its only time base.

## Architectural components

The generic timer has two halves:

- A free-running **system counter**, readable through `CNTPCT_EL0`, incrementing
  at the fixed rate reported by `CNTFRQ_EL0`. It never stops and never wraps in
  any practical timeframe.
- A per-core **timer**, programmed through `CNTP_TVAL_EL0` and `CNTP_CTL_EL0`,
  which asserts an interrupt when its comparison point is reached.

NXU uses the physical timer (`CNTP_*`) rather than the virtual timer
(`CNTV_*`). The virtual timer applies an offset controlled from EL2 and is
intended for guests under a hypervisor; a kernel that owns the machine uses the
physical one.

## Counter frequency

`timer_get_frequency()` returns `CNTFRQ_EL0`. This register is not something the
timer computes — it is a value firmware programs to describe the system counter
increment rate, and the kernel treats it as authoritative.

Every conversion function checks it for zero and returns a safe value rather
than dividing:

```c
uint64_t frequency = timer_get_frequency();

if (frequency == 0) {
	return 0;
}
```

## Periodic timer programming

`timer_start_periodic(frequency_hz)` arms the timer to fire at a fixed rate:

```text
interval = CNTFRQ_EL0 / frequency_hz

if interval == 0            -> interval = 1
if interval > 0x7FFFFFFF    -> interval = 0x7FFFFFFF

g_timer_interrupt_count = 0
g_timer_interrupt_rate  = frequency_hz
g_timer_interval_ticks  = interval

CNTP_TVAL_EL0 = interval
CNTP_CTL_EL0  = 1              ENABLE=1, IMASK=0
```

`CNTP_TVAL_EL0` is a *countdown* view of the timer. Writing a value sets the
comparison point to "the current counter plus this many ticks", and the timer
asserts when it is reached. Architecturally it behaves as a signed 32-bit
value, which is why the interval is clamped to `0x7FFFFFFF`: a larger value
would appear negative and read as already expired.

`CNTP_CTL_EL0` has three relevant bits:

| Bit | Name | NXU value |
| --- | --- | --- |
| 0 | `ENABLE` | 1 |
| 1 | `IMASK` | 0 — interrupt not masked |
| 2 | `ISTATUS` | read-only condition status; not consulted |

Writing `1` therefore enables the timer with its interrupt unmasked, which is
why the source comment describes `1` as "enables and unmasks".

`timer_start_periodic()` returns `void` and silently does nothing if either the
requested rate or `CNTFRQ_EL0` is zero.

## Timer PPI

The physical timer at EL1 is delivered as **PPI INTID 30**. `kern_init()`
defines this as `PHYSICAL_TIMER_INTID` and calls
`gic_enable_ppi(30, 0x80)`; `irq_handle()` compares the acknowledged INTID
against a literal `30`.

The PPI is configured **level-sensitive**. See
[Interrupt controller](interrupt-controller.md) for why, and for how
`GICR_ICFGR1` encodes it.

## Handler flow

```c
void timer_handle_interrupt(void)
{
	arm64_write_physical_timer_value(g_timer_interval_ticks);
	g_timer_interrupt_count++;
}
```

The rearm comes first, and the ordering is required rather than stylistic.
Because the interrupt is level-sensitive, the timer holds its signal asserted
until the comparison point moves into the future. Writing `CNTP_TVAL_EL0`
re-establishes a future deadline and deasserts the line. If the EOI in
`irq_handle()` ran while the line was still asserted, the interrupt would
immediately become pending again and the kernel would livelock in the handler.

The full path is:

```text
counter reaches the comparison point
     |
     v
CNTP_CTL_EL0.ISTATUS asserts, interrupt line raised
     |
     v
GICv3 redistributor forwards PPI 30 (Group 1, level)
     |
     v
IRQ taken to EL1 -> vector entry 5 -> exception_handle()
     |
     v
irq_handle() -> ICC_IAR1_EL1 returns 30
     |
     v
timer_handle_interrupt()
     |   CNTP_TVAL_EL0 = interval     <- deasserts the line
     |   ++g_timer_interrupt_count
     v
ICC_EOIR1_EL1 = 30
     |
     v
eret back to the idle loop's wfi
```

## Interrupt count and uptime

`g_timer_interrupt_count` is a `volatile uint64_t` incremented once per tick and
exposed through `timer_get_interrupt_count()`. `volatile` matters here: the
counter is written in interrupt context and read from the idle loop, and without
it the compiler could hoist the load out of the loop.

`kern_init()` derives uptime by integer division:

```c
uint64_t seconds = timer_get_interrupt_count() / KERNEL_TIMER_HZ;
```

with `KERNEL_TIMER_HZ` defined as 100. Note that the divisor is the compile-time
constant, not `timer_get_interrupt_rate()`. The two agree today because
`kern_init()` passes the same constant to `timer_start_periodic()`.

Uptime derived this way counts *delivered interrupts*, not elapsed time. Any
tick lost — because interrupts were masked across a long region, for example —
makes the reported uptime lag real time permanently. A wall-clock reading should
use `CNTPCT_EL0` through `timer_get_microseconds()` instead.

## Time conversion

```c
uint64_t timer_get_ticks(void);                        /* isb; mrs CNTPCT_EL0 */
uint64_t timer_ticks_to_microseconds(uint64_t ticks);
uint64_t timer_get_microseconds(void);
void     timer_delay_ms(uint64_t milliseconds);
```

`timer_get_ticks()` issues an `isb` before reading `CNTPCT_EL0`. Without it the
counter read could be reordered relative to surrounding instructions, since the
counter is not a memory location and is not ordered by data dependencies.

`timer_ticks_to_microseconds()` avoids overflow by splitting the conversion:

```c
uint64_t seconds        = ticks / frequency;
uint64_t remaining      = ticks % frequency;

return seconds * 1000000UL + remaining * 1000000UL / frequency;
```

The naive `ticks * 1000000 / frequency` would overflow 64 bits after roughly
`2^64 / 10^6` ticks. Splitting keeps the multiplication bounded by
`frequency * 10^6`, which is safe for any realistic counter frequency, and the
final `seconds * 1000000` term only overflows after about 584542 years.

`timer_delay_ms()` is a busy-wait spin on `CNTPCT_EL0` with a `yield` in the
loop body. It is unused by the current kernel.

## Precision and overflow

| Concern | Current behavior |
| --- | --- |
| `CNTPCT_EL0` wrap | 64-bit; not a practical concern |
| `g_timer_interrupt_count` wrap | 64-bit; at 100 Hz, ~5.8 billion years |
| `CNTP_TVAL_EL0` range | Clamped to `0x7FFFFFFF`; caps the minimum rate |
| Interval rounding | `CNTFRQ_EL0 / frequency_hz` truncates; the effective rate is slightly faster than requested when the division is inexact |
| Tick drift | Rearming inside the handler means each period starts when the handler runs, so handler latency accumulates as drift |
| Missed ticks | Not detected. A tick that arrives while masked is coalesced, and the count — and therefore uptime — falls behind |

The drift point is the structural one. A periodic timer that rearms relative to
"now" rather than relative to the previous deadline cannot maintain a fixed
phase. Using `CNTP_CVAL_EL0` with an absolute comparison point advanced by a
fixed interval would avoid this; NXU does not do that.

## Concurrency and interrupt context

- `timer_handle_interrupt()` is an **interrupt-context** function. It is called
  only from `irq_handle()` and must not be called from ordinary kernel code.
- `timer_start_periodic()` must **not** be called from interrupt context: it
  resets `g_timer_interrupt_count` and rewrites the interval that the handler
  reads.
- `timer_get_interrupt_count()`, `timer_get_ticks()` and the conversion helpers
  are safe from either context. They read `volatile` or read-only state.
- No locking exists. Safety rests on single-core execution and on the fact that
  the handler is the only writer of the count.

## Current limitations

- One timer, one rate, no timer queue and no software timers.
- No `CNTP_CVAL_EL0` use, so no absolute deadlines and unavoidable drift.
- No one-shot mode; `timer_start_periodic()` is the only way to arm the timer.
- No way to stop the timer once started.
- `g_timer_interrupt_rate` is recorded and exposed through
  `timer_get_interrupt_rate()`, but nothing reads it — uptime uses the
  `KERNEL_TIMER_HZ` constant instead.
- `timer_delay_ms()`, `timer_get_microseconds()` and
  `timer_ticks_to_microseconds()` are implemented but currently unused.
- The timer INTID is hard-coded as 30 in two places rather than derived from the
  Device Tree's `interrupts` property, which discovery does not parse.
- The panic report prints `System uptime: unavailable` and does not consult the
  timer.

## Source files

- [`arch/arm64/timer.c`](../../arch/arm64/timer.c)
- [`arch/arm64/timer.h`](../../arch/arm64/timer.h)
- [`arch/arm64/exception.c`](../../arch/arm64/exception.c)
- [`kern/kern_init.c`](../../kern/kern_init.c)

## Related documentation

- [ARM64 overview](overview.md)
- [Interrupt controller](interrupt-controller.md)
- [Exceptions](exceptions.md)
- [System registers](system-registers.md)
- [Core kernel initialization](../kern/initialization.md)
