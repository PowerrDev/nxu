#
# Copyright (c) 2026 NXU Project. All rights reserved.
#
# File:        makedefs/x86_64.mk
#
# Build and run the first native x86-64 kernel entry milestone.

X86_64_BUILD := $(BUILD_ROOT)/x86_64
X86_64_KERNEL := $(X86_64_BUILD)/kernel.elf
X86_64_CC := clang

X86_64_CFLAGS := \
	--target=x86_64-none-elf \
	-ffreestanding \
	-fno-stack-protector \
	-fno-pic \
	-fno-omit-frame-pointer \
	-mno-red-zone \
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

X86_64_ASFLAGS := --target=x86_64-none-elf -mno-red-zone

X86_64_C_SOURCES := \
	kern/x86_64/boot.c \
	kern/x86_64/cpu_local.c \
	kern/x86_64/gdt.c \
	kern/x86_64/idt.c \
	kern/x86_64/trap.c \
	kern/x86_64/timer.c \
	kern/x86_64/smp.c \
	vm/x86_64/bootstrap.c \
	vm/x86_64/vm_init.c \
	vm/x86_64/vmm.c \
	vm/address_space.c \
	vm/arch/x86_64/pmap.c \
	vm/pmm.c \
	vm/dma.c \
	vm/vm_kern.c \
	vm/vm_map.c \
	vm/vm_fault.c \
	vm/vm_shm.c \
	vm/user_copy.c \
	platform/x86_64/memory_map.c \
	platform/x86_64/uart.c \
	kern/console/console.c \
	kern/memory/heap.c \
	libk/string.c

X86_64_ASM_SOURCES := \
	kern/x86_64/start.S \
	kern/x86_64/gdt_load.S \
	kern/x86_64/trap_vectors.S

X86_64_OBJECTS := \
	$(X86_64_C_SOURCES:%.c=$(X86_64_BUILD)/%.o) \
	$(X86_64_ASM_SOURCES:%.S=$(X86_64_BUILD)/%.o)

.PHONY: x86_64 nxuboot-image-x86_64 run-x86_64 run-x86_64-firmware

-include $(X86_64_OBJECTS:%.o=%.d)

$(X86_64_BUILD)/compiler-flags:
	@mkdir -p $(dir $@)
	@printf '%s\n' '$(X86_64_CC) $(X86_64_CFLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ || cp $@.tmp $@
	@rm -f $@.tmp

$(X86_64_BUILD)/%.o: %.c $(X86_64_BUILD)/compiler-flags
	@mkdir -p $(dir $@)
	$(QUIET_PRINT) "CC" "$<"
	$(Q)$(X86_64_CC) $(X86_64_CFLAGS) -c $< -o $@

$(X86_64_BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(QUIET_PRINT) "AS" "$<"
	$(Q)$(X86_64_CC) $(X86_64_ASFLAGS) -c $< -o $@

$(X86_64_KERNEL): $(X86_64_OBJECTS) makedefs/linker-x86_64.ld
	@mkdir -p $(dir $@)
	$(QUIET_PRINT) "LD" "$@"
	$(Q)$(LD) -m elf_x86_64 -z max-page-size=4096 --build-id=none -nostdlib -T makedefs/linker-x86_64.ld $(X86_64_OBJECTS) -o $@

x86_64: $(X86_64_KERNEL)

nxuboot-image-x86_64: $(NXUBOOT_EFI) $(X86_64_KERNEL)
	$(Q)python3 tools/make_uefi_image.py --loader $(NXUBOOT_EFI) --kernel $(X86_64_KERNEL) \
		--output $(NXUBOOT_DIR)/nxu-x86_64-boot.img --bootargs "$(NXUBOOT_ARGS)"

run-x86_64-firmware: nxuboot-image-x86_64 nxuboot-check-firmware
	$(Q)cp "$(NXUBOOT_VARS)" $(NXUBOOT_DIR)/uefi-vars-x86_64.fd
	$(Q)qemu-system-x86_64 -machine q35 -cpu qemu64 -m 512M -smp 1 \
		-drive if=pflash,format=raw,readonly=on,file="$(NXUBOOT_FIRMWARE)" \
		-drive if=pflash,format=raw,file=$(NXUBOOT_DIR)/uefi-vars-x86_64.fd \
		-drive if=none,format=raw,file=$(NXUBOOT_DIR)/nxu-x86_64-boot.img,id=nxuboot \
		-device qemu-xhci -device usb-storage,drive=nxuboot -serial stdio

run-x86_64: run-x86_64-firmware
