# Mouse

## Overview

NXU currently supports one relative pointer device. The class driver consumes
normalized relative-axis, button, and synchronization events.

Recognized events include:

- `REL_X`;
- `REL_Y`;
- `REL_WHEEL`;
- `BTN_LEFT`;
- `BTN_RIGHT`;
- `BTN_MIDDLE`;
- `EV_SYN / SYN_REPORT`.

## Packet boundaries

A physical pointer update is commonly delivered as several events followed by
a synchronization event:

```text
EV_REL REL_X      +4
EV_REL REL_Y      -2
EV_KEY BTN_LEFT    1
EV_SYN SYN_REPORT  0
```

The driver accumulates relative movement and button changes until
`SYN_REPORT`. Only then is the pending update committed as one pointer packet.

This prevents consumers from observing half of a device update.

## Coordinates

The current `mouse_x()` and `mouse_y()` values are accumulated relative motion,
not framebuffer coordinates.

The kernel does not perform:

- pointer acceleration;
- screen clipping;
- display transforms;
- cursor hit testing;
- focus routing.

Those policies belong to the graphical consumer. The desktop host
(`platform/<arch>/services/ui_service.c`, through
`drivers/video/ui_service_pointer.h`) takes the motion as the host's points
(QEMU forwards the host's own, already accelerated, deltas) and multiplies it
by the screen's content scale, so on a 2x screen the pointer goes as far as the
host's cursor would. It adds no acceleration of its own.

## Buttons

Button state is maintained as a compact bitmask containing left, right, and
middle button state. Button changes participate in the same synchronization
boundary as relative motion.

## Interrupt context

`mouse_handle_event()` may execute in IRQ context. It performs no blocking
operation and no dynamic allocation.

The class driver does not access VirtIO MMIO registers. It receives only
normalized events from the input transport.

## Invariants

- Relative events are accumulated until `SYN_REPORT`.
- A committed packet represents one coherent device update.
- Button state reflects the most recently committed input stream.
- Screen-space cursor policy remains outside the kernel mouse class.
