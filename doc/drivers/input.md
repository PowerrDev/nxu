# Input

## Overview

The input subsystem defines the kernel boundary between transport-specific
input delivery and device-class state.

```text
transport driver
      |
      v
input_event_t
   /      \
  v        v
keyboard  mouse
   \      /
    v    v
raw event queue
      |
      v
future userspace input service
```

`input_event_t` preserves the event triplet used by the current VirtIO Input
transport:

```c
uint16_t type;
uint16_t code;
int32_t value;
```

It also carries an NXU input-device identifier and device class. Consumers
do not need to know which bus delivered the event.

## Initialization

`input_init()` initializes a fixed-capacity raw event queue. Keyboard and mouse
class state are initialized separately and attached when VirtIO Input devices
are classified.

The event queue is intentionally available before userspace exists so input
bring-up can be validated without changing the eventual event boundary.

## Publication

`input_publish()` is callable from interrupt context. It:

- performs no dynamic allocation;
- does not block;
- preserves FIFO event order;
- drops the incoming event when the queue is full;
- increments a drop counter for diagnostics.

Class state is updated independently of whether the raw queue had capacity.
A temporary userspace consumer stall must not make modifier or pointer state
incorrect.

## Raw event queue

The queue currently contains 256 events. It is protected by locally masking
IRQs on the single supported CPU.

`input_read()` is non-blocking. The future userspace interface should place a
thread on a wait queue when no event is available rather than polling this
primitive directly.

## Device-class policy

The input layer does not interpret keyboard layouts or screen coordinates.
Those policies belong above the raw device boundary.

The kernel currently maintains enough class state for device correctness and
bring-up:

- keyboard key-down and modifier state;
- mouse button state;
- relative pointer deltas grouped by synchronization boundaries.

The small kernel key-to-character table is diagnostic only.

## Userspace boundary

The raw event queue is intended to feed a blocking userspace interface. The
exact mechanism is not fixed yet. Candidates include:

- a VFS character device;
- an input-specific syscall;
- an IPC endpoint owned by an input/window server.

Whichever interface is selected must preserve:

- device identity;
- event ordering;
- synchronization boundaries;
- blocking wakeup semantics;
- bounded kernel memory use.

## Locking and context

Input publication and class handling may occur in IRQ context. These paths
must not sleep or allocate from a blocking allocator.

The present single-CPU implementation uses IRQ masking for queue/state
serialization. SMP requires per-device or subsystem locks with interrupt-safe
acquisition rules.

## Invariants

- Transport code publishes normalized events only.
- Device-class code does not perform VirtIO MMIO operations.
- Raw events remain ordered by publication.
- Queue overflow never corrupts keyboard or mouse state.
- Text layout and graphical cursor policy remain outside the transport layer.
