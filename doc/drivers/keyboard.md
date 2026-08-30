# Keyboard

## Overview

The keyboard driver is a transport-independent input class. It consumes
normalized `EV_KEY` events and maintains the state required to reason about a
physical keyboard independently of the bus which produced the event.

The current VirtIO Input transport supplies Linux evdev key codes. Another
transport must translate its native reports before entering the keyboard class
layer.

## State

The driver maintains:

- a key-down bitmap;
- left and right Shift state;
- left and right Control state;
- left and right Alt state;
- Caps Lock state;
- event counters;
- the last diagnostic character produced by the bring-up keymap.

Key press, release, and repeat values update this state before returning from
the event path.

## Attachment

The current kernel selects one keyboard as the system keyboard. Additional
keyboards can still be represented by the generic input layer, but class-state
aggregation across multiple keyboards is not implemented yet.

The selected device ID remains stable for the life of the attached input
device.

## Character translation

A small US-QWERTY table exists only for early UART diagnostics. It handles:

- alphabetic keys;
- digits;
- common punctuation;
- Space;
- Tab;
- Enter;
- Backspace;
- Escape.

This table is not the long-term text-input interface.

Keyboard layout, compose processing, dead keys, Unicode input methods, and
application-level shortcuts belong in userspace. The kernel should deliver key
identity and modifier state without turning one keyboard layout into kernel
policy.

The intended path is:

```text
raw key events
      |
      v
userspace input service
      |
layout / compose / IME
      |
      v
focused application
```

## Interrupt context

`keyboard_handle_event()` may run from a device interrupt routine. The path
must remain bounded, non-blocking, and allocation-free.

Heavy UART output is deliberately avoided on every key event. Diagnostics are
reported asynchronously from ordinary kernel execution.

## Invariants

- A released key is not present in the key-down bitmap.
- Modifier state is derived from current key state, except toggled state such
  as Caps Lock.
- Raw key events remain available independently of diagnostic character
  translation.
- Transport-specific reports never leak into keyboard policy code.
