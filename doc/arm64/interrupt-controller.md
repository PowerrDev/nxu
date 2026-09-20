# Interrupt Controller (GICv3)

NXU uses the GICv3 interrupt controller provided by QEMU's `virt` machine
with `gic-version=3`. The implementation covers exactly what is needed to route
one private peripheral interrupt to one core.

## The three GICv3 components

A GICv3 is not a single block. NXU touches all three parts, and the
distinction determines how each register is reached:

| Component | Scope | Access method |
| --- | --- | --- |
| Distributor | System-wide | MMIO at `GICD_BASE` |
| Redistributor | Per-core | MMIO at `GICR_BASE` (two frames) |
| CPU interface | Per-core | `ICC_*_EL1` system registers |

The move from the memory-mapped CPU interface of GICv2 to system registers is
the defining change in GICv3, and it is why `ICC_SRE_EL1.SRE` must be set before
any other `ICC_*` access is architecturally valid.

The redistributor is split into two 64 KiB frames:

```text
GICR_BASE + 0x00000    RD_base    redistributor control (GICR_WAKER, ...)
GICR_BASE + 0x10000    SGI_base   SGI and PPI configuration
```

`GICR_SGI_BASE` is defined as `GICR_BASE + 0x10000` for exactly this reason.

## Interrupt ID ranges

| Range | Kind | Used by NXU |
| --- | --- | --- |
| 0-15 | SGI — software generated | No |
| 16-31 | PPI — private peripheral | Yes: INTID 30 |
| 32-1019 | SPI — shared peripheral | No |
| 1020-1023 | Special / spurious | Recognized and discarded |

`gic_enable_ppi()` enforces its range explicitly, returning without effect for
any INTID outside 16-31. Because it returns `void`, an out-of-range request is
silently ignored.

## Initialization sequence

`gic_init()` performs three steps in a fixed order.

### 1. Distributor

```text
GICD_CTLR = 0                          disable while reconfiguring
wait for GICD_CTLR.RWP == 0            register write pending
GICD_CTLR = ARE_NS | EnableGrp1        affinity routing + Group 1
wait for GICD_CTLR.RWP == 0
```

`ARE_NS` (bit 5) enables affinity routing, which is the GICv3 model in which
interrupts are targeted by affinity value rather than by a CPU bitmask.
`EnableGrp1` (bit 1) enables forwarding of Non-secure Group 1 interrupts.

`RWP` (bit 31) is the register-write-pending flag. Several distributor writes
take effect asynchronously, so both writes are followed by a poll on `RWP`
before proceeding. The poll body executes `yield`, a hint instruction that
costs nothing on a single core but marks the loop as a spin.

### 2. Redistributor

```text
GICR_WAKER &= ~ProcessorSleep          announce that CPU 0 is awake
dsb sy
wait for GICR_WAKER.ChildrenAsleep == 0
```

A redistributor comes out of reset assuming its core is asleep and will not
forward interrupts. Clearing `ProcessorSleep` (bit 1) requests wake-up;
`ChildrenAsleep` (bit 2) is the hardware's acknowledgement that the wake-up has
completed. The `dsb sy` between them orders the MMIO write ahead of the polling
reads.

### 3. CPU system-register interface

```text
ICC_SRE_EL1    |= SRE                  enable the system-register interface
ICC_CTLR_EL1   &= ~EOImode             EOI performs drop + deactivate
ICC_PMR_EL1     = 0xFF                 lowest priority threshold: allow all
ICC_BPR1_EL1    = 0                    no priority grouping / preemption split
ICC_IGRPEN1_EL1 = 1                    enable Group 1 signaling to this CPU
```

`ICC_PMR_EL1` is a threshold: an interrupt is only signaled if its priority
value is numerically *lower* than the mask. Writing `0xFF`, the lowest possible
priority, admits every interrupt.

`EOImode = 0` means a single write to `ICC_EOIR1_EL1` performs both the
priority drop and the deactivation. The alternative, `EOImode = 1`, splits those
into `ICC_EOIR1_EL1` and `ICC_DIR_EL1` and is used by hypervisors; NXU has no
need for it.

Each `msr` that changes interrupt behavior is followed by `isb` so the effect is
visible to the instructions after it.

### Why this order

The distributor's routing model must be settled before the redistributor is
woken, because affinity routing changes how the redistributor interprets its
configuration. The redistributor must be awake before its CPU interface can
deliver anything. And `ICC_SRE_EL1.SRE` must precede every other `ICC_*` write.

## Enabling a PPI

`gic_enable_ppi(intid, priority)` configures one PPI through the
redistributor's SGI/PPI frame. Every register here is per-core, which is why
none of them lives in the distributor.

| Step | Register | Effect |
| --- | --- | --- |
| 1 | `GICR_IGROUPR0` | Set bit `intid`: place the interrupt in Non-secure Group 1 |
| 2 | `GICR_IPRIORITYR0 + intid` | One priority byte per interrupt; lower value = higher priority |
| 3 | `GICR_ICFGR1` | Two bits per PPI: `00` level-sensitive, `10` edge-triggered |
| 4 | `GICR_ICPENDR0` | Clear any stale pending state |
| 5 | `GICR_ISENABLER0` | Enable forwarding |
| 6 | — | `dsb sy` to complete all of the above |

`GICR_ICFGR1` covers INTIDs 16-31, so the bit position for a PPI is
`(intid - 16) * 2`. NXU clears both bits, selecting **level-sensitive**. That
choice is dictated by the generic timer: `CNTP_CTL_EL0` asserts a level that
remains high until the timer is rearmed, so the interrupt must be modeled as a
level rather than an edge. It is also why `timer_handle_interrupt()` rearms the
timer *before* incrementing its counter — deasserting the level is what prevents
an immediate re-entry.

`kern_init()` calls `gic_enable_ppi(30, 0x80)`. Priority `0x80` is the midpoint
of the 8-bit priority space; with a single interrupt source the value has no
practical effect.

## Interrupt acknowledgement and completion

```c
uint32_t gic_acknowledge_interrupt(void);   /* mrs ICC_IAR1_EL1 */
void     gic_end_interrupt(uint32_t intid); /* msr ICC_EOIR1_EL1 */
```

Reading `ICC_IAR1_EL1` is not a passive query. It atomically acknowledges the
highest-priority pending Group 1 interrupt, moves it to the active state, and
returns its INTID. NXU masks the result with `0x00FFFFFF` because the INTID
field is 24 bits.

Writing `ICC_EOIR1_EL1` with that same INTID signals completion. With
`EOImode = 0` this both drops the running priority and deactivates the
interrupt, allowing it to be signaled again.

Every acknowledged interrupt must be completed, and completed with the INTID
that was returned. Failing to do so leaves the running priority elevated and
silently blocks all subsequent interrupts at the same or lower priority.

## Spurious interrupts

`irq_handle()` in [`exception.c`](../../kern/arm64/exception.c) checks
for INTIDs 1020 and above:

```c
uint32_t intid = gic_acknowledge_interrupt();

if (intid >= 1020U) {
	return;
}
```

INTID 1023 is returned when the CPU acknowledges an interrupt that is no longer
pending — for instance because a level-sensitive source deasserted between
signaling and acknowledgement. Such a read must **not** be followed by an EOI,
because nothing was actually activated. NXU returns immediately without
writing `ICC_EOIR1_EL1`, which is the correct handling.

An INTID that is neither 30 nor spurious prints `irq: unhandled INTID <n>` and
is then completed normally, so an unexpected source cannot wedge the controller.

## Masking IRQs at the CPU

`arm64_enable_irqs()` and `arm64_disable_irqs()` manipulate `PSTATE.I` through
`DAIFClr, #2` and `DAIFSet, #2`, each followed by `isb`. These are declared in
`gic.h` because they are the CPU-side complement of the GIC's own masking, but
they are `PSTATE` operations and touch no GIC register.

There are two independent layers of masking, and both must be open for an
interrupt to be taken:

```text
source enabled at the redistributor  (GICR_ISENABLER0)
        AND priority passes ICC_PMR_EL1
        AND Group 1 enabled          (ICC_IGRPEN1_EL1, GICD_CTLR.EnableGrp1)
        AND PSTATE.I clear           (arm64_enable_irqs)
```

`arm64_disable_irqs()` is defined but never called.

## Initialization ordering

`gic_init()` runs near the end of `kern_init()`, after the MMU has mapped the
GIC frames as Device memory. Mapping matters: GIC registers must not be
cacheable or speculatively accessed, which is exactly what the Device memory
type guarantees. See [Virtual memory](../vm/virtual-memory.md).

`gic_enable_ppi()` and `timer_start_periodic()` follow, and `PSTATE.I` is
cleared last. Between arming the timer and unmasking IRQs the interrupt simply
accumulates as pending.

## Concurrency

`gic.c` holds no software state. Every function is a sequence of MMIO or
system-register accesses. That said:

- **Read-modify-write hazards.** `gic_enable_ppi()` performs read-modify-write
  on `GICR_IGROUPR0` and `GICR_ICFGR1`. On a single core with IRQs masked this
  is safe; it would not be safe against concurrent access.
- **`gic_init()` must not be called from interrupt context**, and neither should
  `gic_enable_ppi()`.
- **`gic_acknowledge_interrupt()` and `gic_end_interrupt()` are interrupt-context
  functions** and should not be called from ordinary kernel code.

## Current limitations

- **Hard-coded MMIO bases.** `GICD_BASE` and `GICR_BASE` are compile-time
  constants in `gic.c`, even though `platform_discover()` recovers both from the
  Device Tree into `platform->gic_distributor` and
  `platform->gic_redistributor`, and `vmm_init()` maps them from those
  discovered values. A source `TODO` records the intent to fix this. Any machine
  whose GIC sits elsewhere would be mapped correctly and then driven at the
  wrong addresses.
- **Single core only.** The driver assumes redistributor 0. There is no
  redistributor enumeration, no affinity computation, no SGI support and no way
  to target another core.
- **PPIs only.** No SPI can be enabled: there is no `GICD_ISENABLER`,
  `GICD_IPRIORITYR` or `GICD_IROUTER` support, so no shared peripheral —
  including the PL011's own interrupt — can be routed.
- **No failure reporting.** `gic_init()` returns `void`. The distributor `RWP`
  poll and the redistributor `ChildrenAsleep` poll have no timeout and will spin
  forever against unresponsive hardware.
- **No priority grouping.** `ICC_BPR1_EL1` is zero and no preemption model is
  implemented; with one interrupt source none is needed.
- **No interrupt registration.** `irq_handle()` dispatches with a hard-coded
  comparison against INTID 30 rather than through a handler table.

## Source files

- [`kern/arm64/gic.c`](../../kern/arm64/gic.c)
- [`kern/arm64/gic.h`](../../kern/arm64/gic.h)
- [`kern/arm64/exception.c`](../../kern/arm64/exception.c)

## Related documentation

- [ARM64 overview](overview.md)
- [Exceptions](exceptions.md)
- [Timer](timer.md)
- [System registers](system-registers.md)
- [Hardware discovery](../platform/hardware-discovery.md)
- [Initialization](../initialization.md)
