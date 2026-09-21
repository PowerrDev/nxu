# =============================================================================
# i386 (and, in time, x86_64) kernel
# =============================================================================
#
#   make i386        build BUILD/i386/kernel.elf (a Multiboot ELF)
#   make run-i386    boot it under qemu-system-i386 with the console on stdio
#   make test-i386   boot it once per trap test and check the reports
#
# Kept separate from the arm64 build until the shared sources are all
# arch-neutral enough to share one source list. Only the parts that already
# have an x86 implementation are listed here.

I386_BUILD := $(BUILD_ROOT)/i386

I386_KERNEL := $(I386_BUILD)/kernel.elf

I386_CC := clang

I386_CFLAGS := \
    --target=i386-none-elf \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic \
    -fno-omit-frame-pointer \
    -g \
    -mgeneral-regs-only \
    -Wall \
    -Wextra \
    -Werror \
    -O2 \
    -MMD \
    -MP \
    -I. \
    -Ilibk \
    $(EXTRA_CFLAGS)

I386_ASFLAGS := --target=i386-none-elf

I386_C_SOURCES := \
    kern/i386/gdt.c \
    kern/i386/i386_init.c \
    kern/i386/idt.c \
    kern/i386/timer.c \
    kern/i386/trap.c \
    platform/i386/uart.c \
    kern/console/console.c \
    libk/string.c \
    libk/udivmoddi4.c

I386_ASM_SOURCES := \
    kern/i386/start.S \
    kern/i386/trap_vectors.S

# Each area of the port adds its own sources, flags and targets in a fragment
# under makedefs/i386/, so two areas never edit the same lines of this file.
# A fragment appends to I386_C_SOURCES / I386_ASM_SOURCES / I386_CFLAGS; any
# source listed twice is built once.
include $(sort $(wildcard makedefs/i386/*.mk))

I386_OBJECTS := \
    $(sort $(I386_C_SOURCES:%.c=$(I386_BUILD)/%.o)) \
    $(sort $(I386_ASM_SOURCES:%.S=$(I386_BUILD)/%.o))

I386_QEMU_FLAGS ?= -display none -serial stdio -monitor none -no-reboot -device isa-debug-exit,iobase=0xf4,iosize=0x04

# run-i386 has a sound device on the host's speakers (the VirtIO Sound driver's
# /dev/audio0, see doc/drivers/virtio-sound.md); the tests use the wav backend.
I386_QEMU_AUDIO ?= -audiodev coreaudio,id=snd0 -device virtio-sound-pci,audiodev=snd0

.PHONY: i386 run-i386 test-i386

-include $(I386_OBJECTS:%.o=%.d)


$(I386_BUILD)/%.o: %.c

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "CC" "$<"

	$(Q)$(I386_CC) $(I386_CFLAGS) -c $< -o $@


$(I386_BUILD)/%.o: %.S

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "AS" "$<"

	$(Q)$(I386_CC) $(I386_ASFLAGS) -c $< -o $@


$(I386_KERNEL): $(I386_OBJECTS) makedefs/linker-i386.ld

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(LD) -m elf_i386 -z max-page-size=4096 --build-id=none -nostdlib -T makedefs/linker-i386.ld $(I386_OBJECTS) -o $@


i386: $(I386_KERNEL)


run-i386: $(I386_KERNEL)

	qemu-system-i386 \
		-kernel $(I386_KERNEL) \
		-m 128M \
		$(I386_QEMU_AUDIO) \
		$(I386_QEMU_FLAGS)


test-i386: $(I386_KERNEL)

	tools/test_i386.sh $(I386_KERNEL)
