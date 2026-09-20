# =============================================================================
# i386 virtual memory area
# =============================================================================
#
#   make test-i386-vm   boot the kernel under QEMU and run the VM self-tests
#                       plus the deliberate-fault cases (tools/test_i386_vm.sh)
#
# The arm64-only page-table code (vm/vmm*.c, vm/address_space.c) is not built
# here: kern/i386/{pmap,vmm_i386,address_space_i386}.c implement the same
# vm/vmm.h and vm/address_space.h interfaces for the x86 paging format. The
# rest of vm/ is shared with arm64 as is.

I386_C_SOURCES += \
    kern/i386/address_space_i386.c \
    kern/i386/memory_map.c \
    kern/i386/pmap.c \
    kern/i386/vm_fault.c \
    kern/i386/vm_init.c \
    kern/i386/vm_selftest.c \
    kern/i386/vmm_i386.c \
    vm/pmm.c \
    vm/user_copy.c \
    vm/vm_kern.c \
    vm/vm_map.c \
    vm/vm_shm.c \
    kern/memory/heap.c \
    kern/tests/vm_map_test.c \
    kern/tests/vm_shm_test.c

.PHONY: test-i386-vm

test-i386-vm: $(I386_KERNEL)

	tools/test_i386_vm.sh $(I386_KERNEL)
