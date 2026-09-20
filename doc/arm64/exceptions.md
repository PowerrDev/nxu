# Exceptions

NXU installs a complete AArch64 exception vector table, saves a full
general-purpose register frame on entry, dispatches IRQs to the interrupt
handler, and treats every other exception as fatal.

## Vector table layout

The table is emitted into the `.text.vectors` section by
[`exception_vectors.S`](../../kern/arm64/exception_vectors.S) and placed
by the linker script at a 2048-byte boundary inside `.text`. `VBAR_EL1` requires
that alignment: the low 11 bits of the register are reserved, so the table base
must be a multiple of 2048.

An AArch64 vector table has 16 entries of 128 bytes each — 2048 bytes total.
The entries are grouped in fours by where the exception came from, and within
each group by exception type:

```text
VBAR_EL1 + 0x000  entry  0   Current EL, SP_EL0     Synchronous
VBAR_EL1 + 0x080  entry  1   Current EL, SP_EL0     IRQ
VBAR_EL1 + 0x100  entry  2   Current EL, SP_EL0     FIQ
VBAR_EL1 + 0x180  entry  3   Current EL, SP_EL0     SError

VBAR_EL1 + 0x200  entry  4   Current EL, SP_ELx     Synchronous
VBAR_EL1 + 0x280  entry  5   Current EL, SP_ELx     IRQ          <-- timer
VBAR_EL1 + 0x300  entry  6   Current EL, SP_ELx     FIQ
VBAR_EL1 + 0x380  entry  7   Current EL, SP_ELx     SError

VBAR_EL1 + 0x400  entry  8   Lower EL, AArch64      Synchronous  <-- future SVC
VBAR_EL1 + 0x480  entry  9   Lower EL, AArch64      IRQ
VBAR_EL1 + 0x500  entry 10   Lower EL, AArch64      FIQ
VBAR_EL1 + 0x580  entry 11   Lower EL, AArch64      SError

VBAR_EL1 + 0x600  entry 12   Lower EL, AArch32      Synchronous
VBAR_EL1 + 0x680  entry 13   Lower EL, AArch32      IRQ
VBAR_EL1 + 0x700  entry 14   Lower EL, AArch32      FIQ
VBAR_EL1 + 0x780  entry 15   Lower EL, AArch32      SError
```

All 16 entries are populated with the same three-instruction stub, differing
only in the vector number they record. Because `_start` selected `SP_EL1` and
NXU never enters EL0 or AArch32, only entries 4-7 are currently reachable, and
in practice only entry 5 ever fires.

The `VECTOR_ENTRY` macro emits exactly five instructions (20 bytes) and pads to
128 with `.space 0x80 - 20`:

```asm
sub sp, sp, #EXCEPTION_FRAME_SIZE   /* carve the frame */
str x16, [sp, #128]                 /* free one scratch register */
mov x16, #\number                   /* record which entry ran */
str x16, [sp, #288]
b   exception_vector_common
```

Each entry must fit in its 128-byte slot, which is why the per-entry work is
minimal and the common path is a separate branch target.

## Exception origin categories

The four groups above answer two questions the handler cannot otherwise infer:

- **Which stack pointer was in use.** Entries 0-3 are taken when the current EL
  was running on `SP_EL0`; entries 4-7 when it was on its own `SP_ELx`. NXU
  runs on `SP_EL1`, so it uses the second group.
- **Where the exception came from.** Entries 8-15 are taken when the exception
  originated at a *lower* exception level — that is, from EL0 — split by whether
  EL0 was executing AArch64 or AArch32 code.

`exception_handle()` uses the recorded vector number rather than decoding
`ESR_EL1` to classify IRQs, because IRQ entries are exactly those with
`vector_id & 3 == 1` in every group.

## Saved register frame

`arm64_exception_frame_t` in
[`exception.h`](../../kern/arm64/exception.h) describes the frame. It is
304 bytes and its layout is duplicated as byte offsets in the assembly, so the
two must be kept in agreement by hand.

| Offset | Size | Field | Written by |
| --- | --- | --- | --- |
| 0 | 248 | `x[0]` … `x[30]` | common path (`x16` by the vector entry) |
| 248 | 8 | `sp` | common path (`sp` *before* the frame was carved) |
| 256 | 8 | `elr` | common path, from `ELR_EL1` |
| 264 | 8 | `spsr` | common path, from `SPSR_EL1` |
| 272 | 8 | `far` | common path, from `FAR_EL1` |
| 280 | 8 | `esr` | common path, from `ESR_EL1` |
| 288 | 8 | `vector_id` | vector entry |
| 296 | 8 | `reserved` | nothing; pads the frame to a multiple of 16 |

The 16-byte total size is required: AArch64 enforces 16-byte stack alignment
whenever `sp` is used as a base register, and a misaligned frame would itself
cause a stack alignment fault.

The saved `sp` is computed as `sp + EXCEPTION_FRAME_SIZE`, recovering the value
the interrupted code was using. `x31` is not saved because it is the zero
register in this encoding, not a general-purpose register.

## Registers the hardware fills in

| Register | Meaning | Used by NXU for |
| --- | --- | --- |
| `ELR_EL1` | Address to resume at | Printed as `pc`, and as the panic `caller` |
| `SPSR_EL1` | `PSTATE` before the exception | Printed as `cpsr`; restored by `eret` |
| `ESR_EL1` | Why the exception was taken | Decoded into EC, IL and ISS |
| `FAR_EL1` | Faulting virtual address | Printed; meaningful for aborts only |

`ESR_EL1` is decoded as:

```text
63          32 31       26 25 24                      0
+-------------+-----------+--+-------------------------+
|     RES0    |    EC     |IL|          ISS            |
+-------------+-----------+--+-------------------------+
```

`exception_class()` extracts bits [31:26], `exception_instruction_length()`
extracts bit 25, and `exception_iss()` extracts bits [24:0].

The exception classes NXU names explicitly:

| EC | Name printed |
| --- | --- |
| `0x00` | Undefined Instruction |
| `0x15` | Supervisor Call |
| `0x20`, `0x21` | Instruction Abort |
| `0x22` | PC Alignment Fault |
| `0x24`, `0x25` | Data Abort |
| `0x26` | Stack Alignment Fault |
| `0x3C` | Breakpoint |
| anything else | Unhandled ARM64 Exception |

EC `0x20` versus `0x21` and `0x24` versus `0x25` distinguish a fault taken from
a lower EL from one taken at the current EL; NXU prints the same name for
both.

## Control flow

```text
exception taken
     |
     v
VBAR_EL1 + (entry * 0x80)
     |  sub sp, sp, #304
     |  save x16, store vector number
     v
exception_vector_common
     |  save x0-x15, x17-x30
     |  save pre-exception sp
     |  save ELR_EL1, SPSR_EL1, FAR_EL1, ESR_EL1
     |  x0 = sp
     v
exception_handle(frame)
     |
     +-- (vector_id & 3) == 1 ? ---- yes ---> irq_handle()
     |                                            |
     |                                            v
     |                                       ICC_IAR1_EL1
     |                                            |
     |                          INTID >= 1020 ----+---> return, no EOI
     |                          INTID == 30   ----+---> timer_handle_interrupt()
     |                          otherwise     ----+---> print "unhandled INTID"
     |                                            |
     |                                            v
     |                                       ICC_EOIR1_EL1
     |                                            |
     |  <-----------------------------------------+
     |
     +-- no ---> decode ESR, print panic report, wfe forever
     |
     v  (IRQ only)
restore x0-x15, x17-x30, then x16
add sp, sp, #304
eret
```

## Return path

`exception_handle()` returns only for handled IRQs. The common path then
restores the general-purpose registers in the reverse of the save order, with
`x16` deliberately last because it was the scratch register the vector entry
borrowed. It removes the frame and executes `eret`, which restores `PSTATE`
from `SPSR_EL1` and jumps to `ELR_EL1` — resuming the interrupted instruction
stream exactly.

Neither `ELR_EL1` nor `SPSR_EL1` is written back from the frame. They are read
into the frame for reporting, but `eret` uses the registers themselves. This is
correct today because NXU never nests exceptions and never modifies the return
context, and it is a constraint that must be revisited before EL0 exists.

## Panic behavior

A non-IRQ exception is fatal. `exception_handle()` prints a fixed report
consisting of a panic banner with the faulting PC and a trap-type number, the
NXU version and build strings, a full 64-bit thread-state dump, the decoded
exception syndrome, and three trailing status lines — then loops on `wfe`
forever.

The trap types are NXU categories, assigned by `panic_trap_type()`, and are
**not** architectural ESR values:

| Trap type | Condition |
| --- | --- |
| 0 | Anything not listed below |
| 1 | EC `0x00` — Undefined Instruction |
| 2 | EC `0x20`/`0x21` — Instruction Abort |
| 3 | EC `0x24`/`0x25` — Data Abort |
| 4 | EC `0x15` — Supervisor Call |

The report format is deliberately styled after a conventional kernel panic dump
so it is easy to read. It is NXU output and nothing more: it does not
implement, and should not be described as compatible with, any other operating
system's panic format or debugging protocol.

The `caller` field currently shows `ELR_EL1`, the faulting PC, because NXU has
neither symbol lookup nor stack unwinding. `make symbolize ADDR=<pc>` resolves
that address against `kernel.elf`; see [Build system](../build-system.md).

The trailing lines `Kernel Extensions in backtrace: none` and
`System uptime: unavailable` are literal constants. NXU has no loadable module
mechanism, and `exception_handle()` does not consult the timer.

## Interrupt-context restrictions

`exception_handle()` and everything it calls run with `PSTATE.I` set — the CPU
masks IRQs automatically when taking one, and NXU never re-enables them inside
the handler. The following restrictions therefore apply to any code reachable
from an exception:

- **No allocation.** `kmalloc()`, `pmm_allocate_page()` and
  `vm_kern_allocate()` are not reentrant and take no lock. None of them may be
  called from an exception handler.
- **No mapping changes.** `vmm_map_page()`, `vmm_unmap_page()` and
  `vmm_protect_page()` mutate live translation tables without synchronization.
- **UART output is permitted**, and is used, because `uart_putc()` is a polled
  MMIO write loop over a hardware FIFO with no software state.
- **Restricted preemption from exception context.** AArch64 context switching is implemented. Timer IRQs may preempt EL0 or the idle thread; arbitrary EL1 kernel preemption remains deferred until kernel critical sections gain preemption-disable accounting.

## Current limitations

- Only IRQs are handled. Synchronous exceptions, FIQs and SErrors all panic. In
  practice FIQ and SError cannot occur because `PSTATE.F` and `PSTATE.A` are
  never cleared.
- Exceptions do not nest. A fault inside the handler would re-enter the vector
  table on the same stack and, since the handler never returns for a fault,
  simply panic again on the second exception's frame.
- The frame layout is duplicated between C and assembly as raw byte offsets with
  no compile-time cross-check.
- No stack switch on entry. An exception taken with a nearly exhausted stack
  will overflow while carving its 304-byte frame.
- No symbol lookup, no backtrace, no register-state recovery, no debugger stub.
- `irq_handle()` returns early on INTIDs 1020-1023 without writing
  `ICC_EOIR1_EL1`, which is correct for spurious INTIDs but means no other
  special INTID is handled.

## Future relevance

Entry 8 — synchronous exception from a lower EL in AArch64 — is the syscall
entry point. When EL0 exists, an `SVC` instruction executed there will produce
EC `0x15` at that vector, and `exception_handle()`'s current behavior of
panicking on trap type 4 will be replaced by a dispatch into a system-call
table. Instruction and data aborts from EL0 (EC `0x20` and `0x24`) will need to
become recoverable rather than fatal, and the return path will need to write
`ELR_EL1` and `SPSR_EL1` back from the frame so a handler can alter where the
interrupted thread resumes. None of this exists; see
[Roadmap to userland](../roadmap-to-userland.md).

## Source files

- [`kern/arm64/exception_vectors.S`](../../kern/arm64/exception_vectors.S)
- [`kern/arm64/exception.c`](../../kern/arm64/exception.c)
- [`kern/arm64/exception.h`](../../kern/arm64/exception.h)
- [`kern/arm64/system.h`](../../kern/arm64/system.h)
- [`makedefs/linker.ld`](../../makedefs/linker.ld)

## Related documentation

- [ARM64 overview](overview.md)
- [Interrupt controller](interrupt-controller.md)
- [Timer](timer.md)
- [System registers](system-registers.md)
- [Build system](../build-system.md)
- [Roadmap to userland](../roadmap-to-userland.md)
