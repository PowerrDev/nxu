# i386 SMP

Multi-CPU bring-up for the i386 port: ACPI/MP table CPU discovery, the Local
APIC, the real-mode AP trampoline, and per-CPU state (GDT/TSS/percpu
segment). See `port.md` for the port as a whole.

## What runs

* The "smp" boot phase (`kern/i386/smp.c`'s `i386_init_smp`), between
  "threads" and "drivers": `mp_table_scan()` (ACPI MADT, falling back to the
  legacy MP table) finds every CPU and its Local APIC id, `apic_init()` maps
  and enables the Local APIC, then each non-boot CPU is started with
  INIT-SIPI-SIPI and polled for ONLINE.
* A uniprocessor boot (no MP table, or one that lists only the boot CPU)
  makes this phase a clean no-op -- nothing about the rest of the kernel
  changes, and every CPU (boot CPU included) already goes through the same
  per-CPU GDT/TSS/percpu-segment setup regardless of CPU count.
* `test=smp` runs the self-test: CPU count against `expect-cpus=<n>`, and an
  `IPI_RESCHEDULE` round trip to every secondary.

## Files

| File | Role |
|------|------|
| `kern/i386/mp_table.{h,c}` | ACPI RSDP/RSDT/MADT and the legacy MP floating pointer/configuration table: `mp_table_scan()` |
| `kern/i386/apic.{h,c}` | Local APIC: mapping, IPIs, INIT-SIPI-SIPI |
| `kern/i386/smp_trampoline.S` | the code a secondary CPU runs from SIPI to its first C code |
| `kern/i386/smp.c` | the boot CPU's side of bring-up, IPI dispatch, `i386_init_smp` |
| `kern/i386/gdt.{h,c}` | per-CPU GDT/TSS and the `GDT_PERCPU_SEL` segment every CPU's `%fs` stays loaded with |
| `kern/i386/smp.h` | `<kern/machine/smp.h>`'s i386 implementation: `machine_cpu_local()` (an `%fs:0` read), `machine_ipi_raise()` |
| `makedefs/i386/smp.mk`, `tools/test_i386_smp.sh` | build fragment (unconditional, unlike UI) and test |

## Per-CPU state: the GDT, not a register

arm64 banks the per-CPU pointer in `TPIDR_EL1`, a real per-CPU hardware
register. i386 has nothing equivalent, so the per-CPU pointer instead lives
at `%fs:0`, where `%fs` is a fixed selector (`GDT_PERCPU_SEL`) that resolves
to a different base address on every CPU only because **every CPU loads its
own, separate copy of the GDT** -- not one shared table with per-CPU
descriptor slots, a genuinely different `g_gdt[cpu_id]` array per CPU (see
`gdt.h`'s file comment). The same mechanism gives every CPU its own main and
double-fault TSS: the old design was one shared TSS whose `esp0` got
overwritten on every context switch, safe only because there was ever one
CPU to race.

`i386_trap_common` (`trap_vectors.S`) reloads `%fs` to `GDT_PERCPU_SEL`
specifically -- separately from `%ds`/`%es`/`%gs`'s `KERNEL_DATA_SEL` -- on
every trap entry, since a trap from ring 3 arrives with the user's own `%fs`
loaded. Anything that fabricates a kernel-mode-resume trap frame by hand
(there is exactly one place that does, `i386_trap_user_terminate` in
`syscall_trap.c`, for a terminating process) must set `fs = GDT_PERCPU_SEL`
in it too, not `KERNEL_DATA_SEL` -- getting this wrong reliably page-faults
`current_processor()` (an `%fs:0` read) the moment code resumes there.

## CPU discovery: ACPI first, MP table as fallback

`qemu-system-i386 -M pc`'s firmware (SeaBIOS) was found, while bringing this
up, to publish only a token one-CPU legacy MP table regardless of `-smp N` or
`acpi=on`/`off` -- real topology only ever showed up in the ACPI MADT
(`RSDP -> RSDT -> MADT`, each a flat checksummed struct; no AML machinery
involved). `mp_table_scan()` tries that path first and only falls back to the
legacy MP table for firmware that offers no ACPI at all. Every physical
address table *content* points at (not the fixed low-memory regions scanned
directly) is bounds-checked against the direct map, and each table's
`length` field is sanity-bounded before it drives a checksum loop or entry
count -- an implausible length otherwise underflows (both unsigned) into a
walk off the end of mapped memory.

## The AP trampoline

Real mode can only start a CPU executing at a page-aligned physical address
below 1 MiB (the SIPI vector encodes it as `address >> 12`), so
`smp_trampoline.S` is compiled as an ordinary part of the kernel image (its
own linked address is irrelevant) but *copied* to `SMP_TRAMPOLINE_PHYS`
(physical `0x8000`) at runtime before a CPU is started. Every jump target
inside it is computed as `SMP_TRAMPOLINE_PHYS + (label - trampoline_start)`,
a compile-time constant from the label difference alone: only offsets
relative to that copy's own start mean anything once it is actually running
from there. `smp_trampoline_params_t`, a fixed-offset parameter block at
`SMP_TRAMPOLINE_PARAMS` (physical `0x8800`, the same page), carries the
boot CPU's GDTR (converted to a *physical* base -- the boot CPU's own GDTR
holds a virtual, higher-half address, meaningless in real mode with paging
off), CR3, this CPU's stack top, its logical id and its `processor_t`.

`i386_boot_pd[0]` (the master kernel page directory's temporary identity map
of physical `[0, 4 MiB)`, normally cleared right after boot -- see `start.S`
and `pmap.c`) is restored for the whole span of bringing up every requested
CPU and cleared again once bring-up finishes: the trampoline addresses its
physical page directly the whole way through, including immediately after
enabling paging but before the jump to its higher-half continuation.

Sequence: 16-bit real mode (load the GDTR from the parameter block, `CR0.PE`)
-> far jump into 32-bit protected mode, still physically addressed (`CR3`,
`CR4.PSE`, `CR0.PG`) -> an ordinary near jump to this CPU's real, linked,
higher-half `.text` address (the same trick `start.S`'s own
`i386_start_high` uses) -> `i386_smp_secondary_main(cpu_id, processor)`,
which sets up this CPU's own GDT/TSS/percpu cell, loads the (shared) IDT,
enables its Local APIC and interrupts, and joins the scheduler's idle loop.
Never returns.

## No IOAPIC, no per-CPU LAPIC timer

Every existing device IRQ stays on the legacy 8259 PIC, delivered to the
boot CPU only, exactly as before SMP existed (see `doc/i386/devices.md`).
The Local APIC is used only for IPIs and INIT-SIPI-SIPI. A secondary's
scheduler tick is `smp_tick_others()`, a broadcast IPI from the boot CPU's
own PIT handler (`kern/i386/irq.c`) -- not a real per-CPU hardware timer, so
a secondary's tick rate and jitter both follow the boot CPU's PIT exactly.

## Boot arguments (self-test)

`expect-cpus=<n>` (default 0, so a plain boot never fails here): requires at
least `n` CPUs online.

## QEMU and test

```
qemu-system-i386 -M pc -smp 4 -kernel kernel.elf -m 512M -display none \
  -serial stdio -monitor none -no-reboot \
  -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
  -append "test=smp qemu-exit=1 expect-cpus=4"
```

`make test-i386-smp` (or `tools/test_i386_smp.sh <kernel.elf>`) boots `-smp
4` (CPU count, every secondary online, an IPI round trip to each), `-smp 1`
(the MP table lists one CPU, `smp` stays a clean no-op) and a plain boot with
no `test=` at all (SMP linked in does not change a normal boot). Timeouts
throughout are generous (10 s bring-up, 500 ms IPI round trip): this is TCG,
soft-realtime at best, and a busy host stalling one virtual CPU well past a
tight deadline is not the delivery mechanism being broken -- the same
tolerance this port's other timing-sensitive tests already give PIT ticks
dropping on a busy host.

## Known gaps

No IOAPIC (see above -- a deliberate scope choice, not a missing piece), no
LAPIC timer mode, no CPU hot-plug, no NUMA/topology awareness beyond a flat
list of Local APIC ids, `smp_cpu_mpidr()`/`machine_cpu_mpidr()` reads
CPUID's initial APIC id rather than the Local APIC's own ID register (the
two agree in practice; the register read would need `apic.c` linked, and
`machine_cpu_mpidr()` is called unconditionally by `processor_register()`,
even in isolated `test-i386-threads`-only builds that never link SMP at
all).
