# i386 threads, ring 3 and system calls

Machine thread state, context switching, the scheduler on i386, the ring 3
transition and the system-call path. Shared code (`kern/process`,
`kern/sched_prism`, `kern/syscall`) is unchanged in behaviour; it reaches the
machine through `<kern/machine/thread.h>`, `<kern/machine/cpu.h>` and
`<kern/machine/system.h>`.

## User ABI (fixed)

| | |
|--|--|
| system call | `int $0x80` (a DPL 3 interrupt gate: interrupts are off inside the kernel) |
| number | `eax` |
| arguments | `ebx, ecx, edx, esi, edi, ebp` = arguments 0..5 (arm64 `x0..x5`), each zero-extended to the 64-bit request field |
| result | `eax` = low 32 bits of the arm64 result. Errors are `-SYSCALL_ERROR_*`: `-1 .. -12` = `0xFFFFFFFF .. 0xFFFFFFF4`. Success values are non-negative; 64-bit results (`uptime_us`, `seek` offset) are truncated to 32 bits |
| preserved | every register except `eax`. A wrapper needs only `"eax"` and `"memory"` clobbers |
| exit | `SYS_exit` never returns to the caller; the process is a zombie with the given status |

```c
static inline uint32_t sys3(uint32_t n, uint32_t a, uint32_t b, uint32_t c)
{
	uint32_t r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
	return r;
}
```

Pointers are 32-bit and the same number in ring 0 and ring 3 (flat
segments). A syscall that takes a 64-bit value in arm64 takes its low 32
bits here; `waitpid` still stores a 64-bit status at the user pointer.

### Initial user state

A new user thread starts with `eip = entry`, `esp = stack` exactly as given
(no return address is pushed; the arm64 rule that the stack is 16-byte
aligned still applies), `eax = initial argument` (0 for a process's first
thread, the caller's value for a pthread-style thread), every other general
register 0, `eflags = 0x202` (IF set, IOPL 0), `cs = 0x1B`, `ss = ds = es =
fs = gs = 0x23`. `entry`, `stack` and the argument must fit in 32 bits.

### Faults

An exception in ring 3 that nothing resolved (`i386_trap_page_fault` gets a
#PF first) terminates the whole process, prints a report, and the kernel
carries on. The exit status is `0x80000000 | vector` (a #GP is
`0x8000000D`); ordinary exit codes never set the top bit. A fault in ring 0
is still the fatal panic report.

## How it works

* **Kernel context**: callee-saved `ebp ebx esi edi` are pushed on the
  outgoing kernel stack, `esp` is saved in `machine.context.sp`, the incoming
  `esp` is loaded and the same four popped (`context_switch.S`). A new thread
  gets a fabricated frame whose return address is the scheduler's first-run
  trampoline, so `sched_thread_continue` runs on an ABI-aligned stack.
* **TSS esp0** is per thread (`machine.esp0`) and installed by every switch
  (`i386_gdt_set_kernel_stack`).
* **Entering ring 3** (`transition.S`): the kernel context is parked on the
  kernel stack, `machine.user_kernel_sp` remembers where, and `esp0` is set
  just below it so trap frames never overwrite it. Then `iret` with
  `ss/esp/eflags/cs/eip`, `eax` = argument, the rest zero.
* **Leaving ring 3 for good** (`SYS_exit`, or a killed process): the trap frame
  is redirected to `i386_user_return` with `eax = user_kernel_sp`; the stub's
  `iret` lands there in ring 0, the parked context is popped and
  `machine_thread_enter_user()` returns to `sched_thread_continue`, which
  exits the (now inactive) thread. This is arm64's `arm64_return_from_el0`.
* **Preemption**: `sched_tick()` (called from the timer IRQ, IF clear) only
  *requests* it. `i386_trap_exit` acts on the request on the way out of any
  trap if the interrupted context is user code or the idle thread. Kernel
  threads are cooperative, as on arm64.
* **Idle / parking**: `cpu_wait_for_interrupt()` and `cpu_wait_for_event()`
  are `hlt`; spin loops use `pause` (`cpu_relax()`).
* **Reset / power off**: `machine_system_reset()` pulses the keyboard
  controller (`outb 0x64, 0xFE`) then triple-faults; `machine_system_power_off()`
  writes QEMU's ACPI port (`outw 0x604, 0x2000`) then halts.

## Boot phase

`i386_init_threads` runs `proc_bootstrap` (idempotent), `thread_bootstrap`
and `sched_bootstrap`, then enables IF **if the interrupt controller driver is
linked** (`pic_init` present, i.e. the interrupts phase ran). On a build
without it IF stays clear: the PIC would still be at its BIOS base and IRQ0
would land on the #DF vector.

## Files

| File | Role |
|------|------|
| `kern/i386/thread.{h,c}` | `machine_thread_t` and the `machine_thread_*` API |
| `kern/i386/context_switch.S` | `i386_switch_context` |
| `kern/i386/transition.S` | `i386_enter_user`, `i386_user_return` |
| `kern/i386/syscall_trap.{h,c}` | strong `i386_trap_syscall`, `i386_trap_user_exception`, `i386_trap_exit` |
| `kern/i386/threads_init.c` | `i386_init_threads` |
| `kern/i386/threads_selftest.c` | `test=threads` |
| `kern/i386/{cpu,system}.h`, `kern/machine/{thread,cpu,system}.h` | dispatch and x86 flavours |
| `kern/i386/threads_standins.c` | **test-only** weak stand-ins, see below |
| `makedefs/i386/threads.mk`, `tools/test_i386_threads.sh` | build fragment, `make test-i386-threads` |

## Test-only stand-ins (drop them at integration)

`threads_standins.c` gives weak definitions of everything the shared sources
need from other areas so this area links and boots alone: `kmalloc/kcalloc/kfree`,
`vm_kern_allocate/free` (kernel stacks), `vm_address_space_*`, `vm_map_*`,
`vm_shm_*`, `vm_copy_*_user` (identity copies), the VFS (`vfs_*`, `filedesc_*`,
`file_*`, `vnode_rele`), block/input/display drivers, `boot_args_raw`,
`boot_mode_is_triage_os`, `loader_spawn`. A strong definition anywhere else
wins automatically (verified against the vm and interrupts areas); build with
`I386_THREADS_STANDINS=0` to remove the file.

## Testing

`make test-i386-threads BUILD_ROOT=<scratch>` (boot, and `test=threads`):
shared run-queue and MLFQ self-tests; `machine_thread` contracts; kernel-thread
ping-pong (FIFO order `ABC` x4, TSS esp0 per switch, termination, reaping);
MLFQ demotion and boost; `i386_trap_exit` with synthetic frames (user frame
preempts, kernel frame does not); the request builder; then ring 3 (paging
off): syscalls incl. register preservation and error values, initial register
state and argument, four faulting processes contained, and a user thread
preempted through the real trap path by software ticks (`int $22`, a test-only
hook). With paging on (the integrated kernel) the ring 3 stages print
"skipped": user code cannot run from kernel pages.

## Still to do at integration

* Ring 3 needs real user address spaces: the loader maps the image, the vm
  area's `vm_address_space_*`/`vm_copy_*_user` replace the stand-ins, and the
  `bootd` boot path exercises this end to end.
* FPU/SSE state is not saved per thread (the kernel is `-mgeneral-regs-only`;
  user floating point is unsupported until it is).
* The idle `hlt` path is not exercised by `test=threads` (it masks the PIC so
  the stages stay deterministic).
* `syscall_thread_create` etc. rely on `vm_map_lookup` for user stacks; same
  vm dependency.
