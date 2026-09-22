# =============================================================================
# i386 SMP: MP table discovery, Local APIC, secondary CPU bring-up
# =============================================================================
#
#   make test-i386-smp   boot the kernel under QEMU with -smp 4 and check the
#                         smp phase self-test (cpu count, an IPI round trip)
#
# Unconditional, unlike UI (makedefs/i386/ui.mk): SMP is a normal part of the
# i386 kernel from here on, unlike the compositor, which stays opt-in behind
# UISERVICE=1/WINDOWSERVER=1. A uniprocessor boot (no MP table, or a QEMU
# `-smp 1`) still links all of this; mp_table_scan() reporting no secondary
# CPUs makes i386_init_smp() a no-op, exactly as before this fragment existed.
#
# machine_ipi_raise/machine_ipi_raise_others and machine_cpu_mpidr (see
# smp.h) have link-safe defaults independent of this fragment -- a weak
# no-op pair in trap.c and a CPUID-only inline in smp.h, respectively -- so
# every isolated test-i386-<area> build that does not include this fragment
# still links, exactly as it did before SMP.

I386_C_SOURCES += \
    kern/i386/apic.c \
    kern/i386/mp_table.c \
    kern/i386/smp.c

I386_ASM_SOURCES += \
    kern/i386/smp_trampoline.S

.PHONY: test-i386-smp

test-i386-smp: $(I386_KERNEL)

	tools/test_i386_smp.sh $(I386_KERNEL)
