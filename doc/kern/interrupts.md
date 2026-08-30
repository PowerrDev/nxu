# Interrupt Dispatch

## Overview

NXU separates interrupt-controller mechanics from device interrupt handling.
Exception entry owns GIC acknowledge and end-of-interrupt. The generic IRQ
layer owns only the mapping from architectural INTID to a registered driver
routine.

```text
AArch64 IRQ vector
      |
      v
gic_acknowledge_interrupt()
      |
      v
irq_dispatch(INTID)
      |
      v
driver interrupt routine
      |
      v
gic_end_interrupt(INTID)
```

A device driver does not acknowledge or EOI the GIC. It acknowledges only the
interrupt source implemented by its own device.

## Registration

`irq_register()` installs one handler/context pair for an INTID. Registration
fails if the slot is already owned.

`irq_unregister()` removes a binding only when the caller supplies the same
handler and context currently installed. This prevents one driver from
silently removing another driver's interrupt source.

The current table covers architectural INTIDs 0 through 1019. Values 1020
through 1023 are reserved by the GIC architecture for special/spurious
responses and are not registered as ordinary device interrupts.

## Interrupt context

Driver interrupt routines execute with IRQs masked by exception entry.
They must not:

- sleep;
- block on a wait queue;
- perform a scheduler operation which can wait;
- allocate through an allocator which may sleep;
- hold an interrupt-disabled critical section for unbounded time.

Short queue manipulation, MMIO acknowledgement, bounded event processing, and
wakeup requests are appropriate operations for this context.

## Locking

The registration table is protected by locally masking IRQs. This is
sufficient for the current single-processor kernel because an interrupt
routine cannot race registration on another CPU.

SMP requires replacing this assumption with an interrupt-safe lock or another
per-source ownership mechanism.

## GIC SPIs

`gic_enable_spi()` configures shared peripheral interrupts used by MMIO
devices. The current CPU-0 implementation:

- assigns Non-secure Group 1;
- sets the interrupt priority;
- programs edge or level triggering from the Device Tree flags;
- routes the interrupt to affinity 0;
- clears stale pending state;
- enables the SPI.

The physical timer remains PPI 30 and continues through the existing PPI
configuration path.

## Invariants

- GIC acknowledge and EOI occur exactly once in exception context.
- A registered INTID has at most one owner.
- Device handlers acknowledge their device before returning when required by
  the device protocol.
- Drivers do not perform GIC EOI themselves.
- Spurious INTIDs never enter the generic dispatch table.
