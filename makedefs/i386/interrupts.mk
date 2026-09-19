# =============================================================================
# i386 interrupts: 8259 PIC, PIT tick, IRQ dispatch
# =============================================================================
#
#   make test-i386-interrupts   boot the normal path, test=interrupts and an
#                               unclaimed IRQ, and check the reports

I386_C_SOURCES += \
    kern/irq/irq.c \
    mach/i386/interrupts_test.c \
    mach/i386/irq.c \
    mach/i386/pic.c

.PHONY: test-i386-interrupts

test-i386-interrupts: $(I386_KERNEL)

	tools/test_i386_interrupts.sh $(I386_KERNEL)
