#
# Copyright (c) 2026 NXU Project. All rights reserved.
#
# File:        makedefs/nbl.mk
#
# NBL, the NXU Boot Loader: one UEFI application per firmware architecture
# (boot/uefi), built on macOS with LLVM (no GNU cross toolchain required), and
# the GPT/FAT32 boot volumes that carry it with a kernel, the boot arguments
# and the boot chime.
#
#   make nbl                  BOOTX64.EFI   (x64 firmware: i386 and x86-64 kernels)
#   make nbl-arm64            BOOTAA64.EFI  (AArch64 firmware: arm64 kernel)
#   make nbl-image            boot volume with the i386 kernel
#   make nbl-image-x86_64     boot volume with the x86-64 kernel
#   make nbl-image-arm64      boot volume with the arm64 kernel
#   make run-i386-firmware    lean i386 boot through firmware and NBL
#   make run-arm64-firmware   arm64 console boot through firmware and NBL

NBL_DIR := $(BUILD_ROOT)/nbl
NBL_EFI := $(NBL_DIR)/BOOTX64.EFI
NBL_IMAGE := $(NBL_DIR)/nxu-boot.img
NBL_FIRMWARE ?= $(shell qemu-system-x86_64 -L help 2>/dev/null | sed -n '2p')/edk2-x86_64-code.fd
NBL_VARS ?= $(dir $(NBL_FIRMWARE))edk2-i386-vars.fd
# Keep PCI MMIO below 4 GiB for the current non-PAE i386 kernel.
NBL_QEMU_PCI_FLAGS ?= -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0
NBL_SHQUOTE = '$(subst ','"'"',$(1))'
NBL_ARGS ?= recovery=1 virtio-irq=1 log.trace=0 log.stats_ms=0

# The sound NBL plays while it loads the kernel: the system volume's boot chime.
# Empty builds a boot volume without one, and the kernel plays the chime itself.
NBL_CHIME ?= tools/DiskRoot/System/Library/Resources/Audio/Boot_Audio.wav
NBL_CHIME_FLAG = $(if $(NBL_CHIME),--chime $(NBL_CHIME))

NBL_ARM64_EFI := $(NBL_DIR)/BOOTAA64.EFI
# The arm64 kernel of this CONFIG, so every test kernel gets its own boot volume.
NBL_ARM64_IMAGE := $(BUILD)/nxu-arm64-boot.img
NBL_ARM64_KERNEL := $(BUILD)/kernel-nbl.elf
NBL_ARM64_UEFI_VARS := $(BUILD)/uefi-vars-arm64.fd
NBL_ARM64_FIRMWARE ?= $(shell qemu-system-aarch64 -L help 2>/dev/null | sed -n '2p')/edk2-aarch64-code.fd
NBL_ARM64_VARS ?= $(dir $(NBL_ARM64_FIRMWARE))edk2-arm-vars.fd
NBL_ARM64_ARGS ?= $(BOOT_ARGS) ui.scale=$(QEMU_UI_SCALE_PERMILLE)

# What an arm64 QEMU needs in place of -kernel/-append to boot through NBL.
# The firmware hands NBL a Device Tree only with ACPI off (see the machine
# option in the rules that use this); the boot volume is a USB stick, which
# the kernel's VirtIO-MMIO scan never sees.
NBL_ARM64_QEMU_BOOT = \
	-drive if=pflash,format=raw,readonly=on,file="$(NBL_ARM64_FIRMWARE)" \
	-drive if=pflash,format=raw,file=$(NBL_ARM64_UEFI_VARS) \
	-drive if=none,format=raw,file=$(NBL_ARM64_IMAGE),id=nbl \
	-device qemu-xhci -device usb-storage,drive=nbl,bootindex=0

NBL_SOURCES_COMMON := main.c chime.c
NBL_X64_OBJECTS := $(addprefix $(NBL_DIR)/x64/,$(NBL_SOURCES_COMMON:.c=.obj) x86.obj virtio_pci.obj wav.obj transition_x86.obj)
NBL_ARM64_OBJECTS := $(addprefix $(NBL_DIR)/arm64/,$(NBL_SOURCES_COMMON:.c=.obj) arm64.obj fdt.obj virtio_mmio.obj wav.obj enter_arm64.obj)

NBL_CFLAGS := -ffreestanding -fno-stack-protector -fno-builtin -O2 -Wall -Wextra -Werror -I. -MMD -MP
NBL_X64_CFLAGS := --target=x86_64-pc-windows-msvc -mno-red-zone $(NBL_CFLAGS)
NBL_ARM64_CFLAGS := --target=aarch64-pc-windows-msvc $(NBL_CFLAGS)

-include $(NBL_X64_OBJECTS:.obj=.d) $(NBL_ARM64_OBJECTS:.obj=.d)

$(NBL_DIR)/x64/%.obj: boot/uefi/%.c
	$(Q)mkdir -p $(dir $@)
	$(Q)clang $(NBL_X64_CFLAGS) -c $< -o $@

$(NBL_DIR)/x64/%.obj: libk/%.c
	$(Q)mkdir -p $(dir $@)
	$(Q)clang $(NBL_X64_CFLAGS) -c $< -o $@

$(NBL_DIR)/x64/%.obj: boot/uefi/%.S
	$(Q)mkdir -p $(dir $@)
	$(Q)clang --target=x86_64-pc-windows-msvc -c $< -o $@

$(NBL_DIR)/arm64/%.obj: boot/uefi/%.c
	$(Q)mkdir -p $(dir $@)
	$(Q)clang $(NBL_ARM64_CFLAGS) -c $< -o $@

$(NBL_DIR)/arm64/%.obj: libk/%.c
	$(Q)mkdir -p $(dir $@)
	$(Q)clang $(NBL_ARM64_CFLAGS) -c $< -o $@

$(NBL_DIR)/arm64/%.obj: boot/uefi/%.S
	$(Q)mkdir -p $(dir $@)
	$(Q)clang --target=aarch64-pc-windows-msvc -c $< -o $@

$(NBL_EFI): $(NBL_X64_OBJECTS)
	$(Q)lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 /out:$@ $^

$(NBL_ARM64_EFI): $(NBL_ARM64_OBJECTS)
	$(Q)lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:arm64 /out:$@ $^

.PHONY: nbl nbl-arm64 nbl-image nbl-image-arm64 nbl-check-firmware nbl-check-firmware-arm64 run-i386-firmware \
	run-arm64-firmware

nbl: $(NBL_EFI)

nbl-arm64: $(NBL_ARM64_EFI)

nbl-image: $(NBL_EFI) $(I386_KERNEL)
	$(Q)python3 tools/make_uefi_image.py --loader $(NBL_EFI) --kernel $(I386_KERNEL) --output $(NBL_IMAGE) \
		$(NBL_CHIME_FLAG) --bootargs $(call NBL_SHQUOTE,$(NBL_ARGS))

nbl-check-firmware:
	$(Q)test -f "$(NBL_FIRMWARE)" && test -f "$(NBL_VARS)" || { echo \
		'NBL: set NBL_FIRMWARE and NBL_VARS to your paired QEMU UEFI firmware files'; exit 1; }

run-i386-firmware: nbl-image i386-disk nbl-check-firmware
	$(Q)cp "$(NBL_VARS)" $(NBL_DIR)/uefi-vars.fd
	$(Q)qemu-system-x86_64 -machine q35 -cpu qemu64,phys-bits=32 -global q35-pcihost.pci-hole64-size=0 -m 512M -smp 2 \
		$(NBL_QEMU_PCI_FLAGS) -drive if=pflash,format=raw,readonly=on,file="$(NBL_FIRMWARE)" -drive \
		if=pflash,format=raw,file=$(NBL_DIR)/uefi-vars.fd -drive if=none,format=raw,file=$(NBL_IMAGE),id=nbl \
		-device qemu-xhci -device usb-storage,drive=nbl -drive if=none,format=raw,file=$(I386_DISK),id=d0 -device \
		virtio-blk-pci,drive=d0,disable-modern=on -device virtio-keyboard-pci,disable-modern=on -device \
		virtio-mouse-pci,disable-modern=on $(I386_QEMU_AUDIO) -serial stdio

# The boot volume carries the kernel without its debug sections: the firmware
# reads it from a USB stick, and NBL only needs the loadable segments.
$(NBL_ARM64_KERNEL): $(KERNEL)
	$(Q)$(OBJCOPY) --strip-debug $< $@

nbl-image-arm64: $(NBL_ARM64_EFI) $(NBL_ARM64_KERNEL)
	$(Q)python3 tools/make_uefi_image.py --loader $(NBL_ARM64_EFI) --loader-name BOOTAA64.EFI \
		--kernel $(NBL_ARM64_KERNEL) --output $(NBL_ARM64_IMAGE) $(NBL_CHIME_FLAG) \
		--bootargs $(call NBL_SHQUOTE,$(NBL_ARM64_ARGS))

nbl-check-firmware-arm64:
	$(Q)test -f "$(NBL_ARM64_FIRMWARE)" && test -f "$(NBL_ARM64_VARS)" || { echo \
		'NBL: set NBL_ARM64_FIRMWARE and NBL_ARM64_VARS to your paired QEMU AArch64 UEFI firmware files'; exit 1; }
	$(Q)cp "$(NBL_ARM64_VARS)" $(NBL_ARM64_UEFI_VARS)

# The console boot of run-console, through firmware and NBL instead of QEMU's -kernel.
run-arm64-firmware: nbl-image-arm64 nbl-check-firmware-arm64 $(DISK) $(DISK_FORMAT_STAMP)
	$(TEP_WRAPPER) qemu-system-aarch64 \
		-machine virt,gic-version=3,acpi=off \
		-cpu cortex-a72 \
		-smp 1 \
		-m 512M \
		$(NBL_ARM64_QEMU_BOOT) \
		$(QEMU_DISPLAY) \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		$(QEMU_RAMFB_DEVICE) \
		-device virtio-blk-device,drive=nxudisk \
		$(QEMU_GPU_DEVICE) \
		$(QEMU_NETWORK) \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		$(QEMU_AUDIODEV) \
		$(QEMU_SOUND_DEVICE) \
		-serial stdio \
		-monitor none
