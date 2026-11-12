# Kernel Console

`kern/console.c` is the machine-independent NXU diagnostic output layer.
Kernel code writes to it instead of writing directly to a hardware console.

UART is always the primary early sink. Additional sinks can be registered later
without changing any logging call site. This is the bridge that lets the future
graphical console display the same kernel log while serial output remains
available for debugging.

## Output path

```text
kernel subsystem
      |
      | kprintf(), kputs(), kputhex64(), ...
      v
 kern/console.c
      |
      +---------------------> PL011 UART
      |
      +---------------------> registered sink 0
      |
      +---------------------> registered sink 1
```

A subsystem never emits the same message twice. Fan-out belongs to the console
layer.

## Boot-history replay

The console retains the most recent 256 KiB of characters in a static circular
buffer. A sink registered with `replay_history=true` first receives that retained
history and then begins receiving live output.

This matters for graphics: a framebuffer or VirtIO-GPU console cannot exist at
the first C instruction, but once it registers it can still draw the earlier
PMM, VMM, platform and driver logs that UART already printed.

Replay is sent only to the newly registered sink. It is not written to UART a
second time.

## `kprintf`

`kprintf()` is a deliberately small freestanding formatter. It supports:

```text
%c  character
%s  string
%d  signed decimal
%i  signed decimal
%u  unsigned decimal
%x  lowercase hexadecimal
%X  uppercase hexadecimal
%p  pointer
%%  literal percent
```

`l` and `ll` length modifiers are supported for integer conversions. Field
widths, precision, floating point and dynamic allocation are intentionally not
implemented.

## Verbose tier

Boot output comes in two tiers. `kprintf()` is the milestone log: what
happened and whether it passed, one fact per line. `kverbosef()` takes the
same formats and is disabled by default, including before boot arguments are
parsed.

## Line prefixes

The prefix is never written by hand. `kprintf()` and `kverbosef()` are macros
that pass the caller's `__func__`, and the console writes it, followed by `: `,
at the start of every line the call begins:

```c
kprintf("devfs mounted at /dev\n");    /* vfs_mount: devfs mounted at /dev */
```

So the prefix is always the function that actually printed the line, and it
follows the code when functions are renamed or split. A line continued by a
later call (`kprintf(" %p", ...)` after a call that did not end in `\n`) gets
no second prefix. Logging more detail from inside the function that did the
work, rather than from its caller, gives each line its own prefix instead of
repeating the caller's.

Two variants cover the other cases:

* `kprintf_for(function, format, ...)` logs under the name of another
  function, usually the one just called when the caller reports how it went
  (`kprintf_for(ipc_process_test, "passed\n")` prints
  `ipc_process_test: passed`). The name is checked at compile time.
* `IOLog()` (and `IOLogVerbose()`) print exactly what they are given, with no
  prefix: IOKit-style family tags such as `VirtIOFamily:` or
  `[com.butterscotch.xpc]:`, the version banner, panic reports, tables, and
  the rest of a line something else started.

`kputs()`/`kputc()` stay raw; userland text reaching the console through them
keeps its own prefix (`bootd: ...`).

## Verbose display

With `-v` the boot display shows the console instead of the logo. The text
already printed when the display comes up is drawn in one go (one paint, one
present) rather than scrolled through, and later lines follow live. The
display leaves out the `[ seconds ]` stamps the serial log carries.

## Verbose switches

`-v` or `log.verbose=1` explicitly enables
detail; `log.verbose=0` selects milestone output. Verbose boots expose `-v` to
userland services.

The diagnostic thread prints actual CPU/scheduler, interrupt/fault, physical
memory, process/thread, VM, IPC port and VirtIO counters every second. Tables
are independently sampled. High-frequency syscalls report interval counts;
other syscalls trace entry arguments and completion results. Counters are
cumulative unless labelled `interval`; errors may complete in a later interval
than their calls. No synthetic subsystem activity is emitted.

`log.stats_ms=250` increases snapshot frequency (250–60000 ms); `0` disables
periodic snapshots. `log.trace=0` disables individual syscall traces. Serial
output and the boot display use the existing console sinks, and logd can read
the console history. Verbosity adds execution and output overhead, especially
with large process/thread tables.

```sh
make run BOOT_ARGS=-v
```

The older fixed-width helpers remain useful for panic and register output:

```text
kputhex_byte()
kputhex32()
kputhex64()
kputu64()
kputi64()
```

## Sink contract

A registered sink is a character callback:

```c
void sink(char character, void *context);
```

A sink must not call the kernel console recursively. The first graphical text
console should write glyphs directly into its framebuffer and register itself
with history replay enabled.

## Current limitations

- Four secondary sinks maximum.
- 256 KiB retained history; older output is overwritten.
- Complete console records are serialized with the console lock.
- No ANSI parser or terminal escape sequences.
- No format width, precision or floating-point formatting.
- UART remains permanently enabled as the primary diagnostic sink.

These constraints keep the console usable during very early boot and panic
handling while giving the graphical stack one stable output interface.
