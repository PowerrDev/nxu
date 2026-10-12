# Native UEFI boot, built on macOS with LLVM (no GNU cross toolchain required).
NXUBOOT_DIR := $(BUILD_ROOT)/nxuboot
NXUBOOT_EFI := $(NXUBOOT_DIR)/BOOTX64.EFI
NXUBOOT_IMAGE := $(NXUBOOT_DIR)/nxu-boot.img
NXUBOOT_FIRMWARE ?= $(shell qemu-system-x86_64 -L help 2>/dev/null | sed -n '2p')/edk2-x86_64-code.fd
NXUBOOT_VARS ?= $(dir $(NXUBOOT_FIRMWARE))edk2-i386-vars.fd
# Keep PCI MMIO below 4 GiB for the current non-PAE i386 kernel.
NXUBOOT_QEMU_PCI_FLAGS ?= -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=0
NXUBOOT_SHQUOTE = '$(subst ','"'"',$(1))'
NXUBOOT_ARGS ?= recovery=1 virtio-irq=1 log.trace=0 log.stats_ms=0

$(NXUBOOT_DIR)/main.obj: boot/uefi/main.c boot/uefi/efi.h boot/handoff.h
	$(Q)mkdir -p $(NXUBOOT_DIR)
	$(Q)clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-stack-protector -fno-builtin -mno-red-zone -O2 -Wall \
		-Wextra -Werror -I. -c $< -o $@

$(NXUBOOT_DIR)/transition.obj: boot/uefi/transition.S
	$(Q)mkdir -p $(NXUBOOT_DIR)
	$(Q)clang --target=x86_64-pc-windows-msvc -c $< -o $@

$(NXUBOOT_EFI): $(NXUBOOT_DIR)/main.obj $(NXUBOOT_DIR)/transition.obj
	$(Q)lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:x64 /out:$@ $^

.PHONY: nxuboot nxuboot-image nxuboot-check-firmware run-i386-firmware

nxuboot: $(NXUBOOT_EFI)

nxuboot-image: $(NXUBOOT_EFI) $(I386_KERNEL)
	$(Q)python3 tools/make_uefi_image.py --loader $(NXUBOOT_EFI) --kernel $(I386_KERNEL) --output $(NXUBOOT_IMAGE) \
		--bootargs $(call NXUBOOT_SHQUOTE,$(NXUBOOT_ARGS))

nxuboot-check-firmware:
	$(Q)test -f "$(NXUBOOT_FIRMWARE)" && test -f "$(NXUBOOT_VARS)" || { echo \
		'NxuBoot: set NXUBOOT_FIRMWARE and NXUBOOT_VARS to your paired QEMU UEFI firmware files'; exit 1; }

run-i386-firmware: nxuboot-image i386-disk nxuboot-check-firmware
	$(Q)cp "$(NXUBOOT_VARS)" $(NXUBOOT_DIR)/uefi-vars.fd
	$(Q)qemu-system-x86_64 -machine q35 -cpu qemu64,phys-bits=32 -global q35-pcihost.pci-hole64-size=0 -m 512M -smp 2 \
		$(NXUBOOT_QEMU_PCI_FLAGS) -drive if=pflash,format=raw,readonly=on,file="$(NXUBOOT_FIRMWARE)" -drive \
		if=pflash,format=raw,file=$(NXUBOOT_DIR)/uefi-vars.fd -drive if=none,format=raw,file=$(NXUBOOT_IMAGE),id=nxuboot \
		-device qemu-xhci -device usb-storage,drive=nxuboot -drive if=none,format=raw,file=$(I386_DISK),id=d0 -device \
		virtio-blk-pci,drive=d0,disable-modern=on -device virtio-keyboard-pci,disable-modern=on -device \
		virtio-mouse-pci,disable-modern=on -serial stdio
