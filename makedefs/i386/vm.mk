# =============================================================================
# i386 virtual memory area
# =============================================================================
#
#   make test-i386-vm   boot the kernel under QEMU and run the VM self-tests
#                       plus the deliberate-fault cases (tools/test_i386_vm.sh)
#
# The arm64 page-table code (vm/vmm*.c) is not built here: i386's VMM backend is
# kern/i386/{pmap,vmm_i386}.c plus the unified-core backend vm/arch/i386/pmap.c,
# which implement vm/vmm.h and vm/pmap.h for the x86 paging format. The rest of
# vm/ — the neutral core (vm/address_space.c, vm_fault.c, vm_map.c, vm_shm.c,
# pmm.c, vm_kern.c, vm_kmap.c, user_copy.c, dma.c) — is the same source shared
# with arm64 and x86_64.

I386_C_SOURCES += \
    vm/arch/i386/pmap.c \
    kern/i386/memory_map.c \
    kern/i386/pmap.c \
    kern/i386/vm_fault.c \
    kern/i386/vm_init.c \
    kern/i386/vm_selftest.c \
    kern/i386/vmm_i386.c \
    vm/address_space.c \
    vm/pmm.c \
    vm/dma.c \
    vm/user_copy.c \
    vm/vm_fault.c \
    vm/vm_kern.c \
    vm/vm_kmap.c \
    vm/vm_map.c \
    vm/vm_shm.c \
    kern/memory/heap.c \
    kern/tests/vm_map_test.c \
    kern/tests/vm_shm_test.c

.PHONY: test-i386-vm

test-i386-vm: $(I386_KERNEL)

	tools/test_i386_vm.sh $(I386_KERNEL)

I386_ASM_SOURCES += kern/i386/vm_probe.S
