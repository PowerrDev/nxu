# =============================================================================
# i386 port: threads area (machine threads, scheduler, ring 3, system calls)
# =============================================================================
#
#   make test-i386-threads   boot the kernel with test=threads and check it
#
# Included by makedefs/i386.mk. Adds the sources of this area to the i386
# kernel; nothing here changes how the arm64 kernel is built.
#
# Shared sources this area makes compile and link for i386 (unchanged
# semantics, only machine-neutral includes were needed):
#
#   kern/process/{proc,task,thread}.c        process manager, tasks, threads
#   kern/sched_prism/*.c                     run queue, processor, MLFQ scheduler
#   kern/syscall/syscall.c                   system-call dispatch
#   kern/ipc/{ipc_init,ipc_space,ipc_port,ipc_kmsg,shm_registry,socket}.c
#                                            referenced by proc.c and syscall.c
#
# What they need from other areas of the port, and where it comes from until
# those land: kmalloc/kfree (vm), vm_kern_allocate/free for kernel stacks
# (vm), vm_address_space_* and the user-copy routines (vm), the VFS, block,
# input and display drivers (devices), loader_spawn (userland). With none of
# them present the link needs the weak, TEST-ONLY stand-ins in
# mach/i386/threads_standins.c. Real definitions elsewhere in the link
# override them automatically; set I386_THREADS_STANDINS=0 to drop the file
# once everything it stands in for exists.

I386_THREADS_STANDINS ?= 1

I386_C_SOURCES += \
    mach/i386/thread.c \
    mach/i386/syscall_trap.c \
    mach/i386/threads_init.c \
    mach/i386/threads_selftest.c \
    kern/process/proc.c \
    kern/process/task.c \
    kern/process/thread.c \
    kern/sched_prism/processor.c \
    kern/sched_prism/run_queue.c \
    kern/sched_prism/sched.c \
    kern/syscall/syscall.c \
    kern/ipc/ipc_init.c \
    kern/ipc/ipc_kmsg.c \
    kern/ipc/ipc_port.c \
    kern/ipc/ipc_space.c \
    kern/ipc/shm_registry.c \
    kern/ipc/socket.c

I386_ASM_SOURCES += \
    mach/i386/context_switch.S \
    mach/i386/transition.S

ifneq ($(I386_THREADS_STANDINS),0)
I386_C_SOURCES += mach/i386/threads_standins.c
endif

.PHONY: test-i386-threads

test-i386-threads: $(I386_KERNEL)

	tools/test_i386_threads.sh $(I386_KERNEL)
