.DEFAULT_GOAL := all

BUILD_ROOT ?= BUILD
CONFIG ?= default
BUILD := $(BUILD_ROOT)/$(CONFIG)

USER_BUILD := $(BUILD_ROOT)/userland

BOOT_ARGS ?=

RAMFB ?= 0

ifeq ($(RAMFB),1)

QEMU_RAMFB_DEVICE := -device ramfb

else

QEMU_RAMFB_DEVICE :=

endif

KERNEL := $(BUILD)/kernel.elf

KERNEL_IMAGE := $(BUILD)/kernel.bin

DISK ?= disk.img

DISK_SIZE ?= 16M

DISK_ROOT ?= assets/DiskRoot

DISK_FORMAT_STAMP ?= .nxu-ext4-jbd2-format

MKFS_EXT4 ?= mkfs.ext4

E2FSCK ?= e2fsck

CC := clang

LD := ld.lld

OBJCOPY := llvm-objcopy

USER_CC := clang

USER_LD := ld.lld

RECOVERY_GENERATED_FONT_DIR := assets/Recovery/Generated

RECOVERY_SANS_SOURCE := frameworks/Recovery.framework/StartupOptionsUI/font_sans.c

RECOVERY_MONO_SOURCE := frameworks/Recovery.framework/StartupOptionsUI/font_mono.c

ARMOS_ARCHIVE ?= $(HOME)/Downloads/armOS-main.zip

ifneq ($(wildcard $(RECOVERY_GENERATED_FONT_DIR)/font_sans.c),)

RECOVERY_SANS_SOURCE := $(RECOVERY_GENERATED_FONT_DIR)/font_sans.c

endif

ifneq ($(wildcard $(RECOVERY_GENERATED_FONT_DIR)/font_mono.c),)

RECOVERY_MONO_SOURCE := $(RECOVERY_GENERATED_FONT_DIR)/font_mono.c

endif

USER_CFLAGS := \
    --target=aarch64-none-elf \
    -ffreestanding \
    -fno-stack-protector \
    -fno-pic \
    -mgeneral-regs-only \
    -Wall \
    -Wextra \
    -Werror \
    -O2 \
    -MMD \
    -MP \
    -Iframeworks/include \
    -Iframeworks/Recovery.framework/include \
    -Iframeworks/Recovery.framework/StartupOptionsUI \
    -Iframeworks/Recovery.framework/triageOS \
    -I.

USER_LDFLAGS := -T makedefs/user.ld -nostdlib -static

# Use all host logical CPUs for recursive kernel/framework builds by default.
BUILD_JOBS ?= $(shell sysctl -n hw.logicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)

export CARGO_BUILD_JOBS ?= $(BUILD_JOBS)


# =============================================================================
# UIService.framework
# =============================================================================

UISERVICE ?= 0

UISERVICE_DIR ?= ../UIService.framework

UISERVICE_LIB := $(UISERVICE_DIR)/build/libUIService.a

UISERVICE_INCLUDE := $(UISERVICE_DIR)/build


# =============================================================================
# WindowServer.framework
# =============================================================================

WINDOWSERVER ?= 0

WINDOWSERVER_DIR ?= ../WindowServer.framework

WINDOWSERVER_LIB := $(WINDOWSERVER_DIR)/BUILD/libWindowServer.a

WINDOWSERVER_INCLUDE := $(WINDOWSERVER_DIR)/include


# =============================================================================
# Framework linker state
# =============================================================================

EXTRA_LIBS :=

UISERVICE_DEPS :=

WINDOWSERVER_DEPS :=


# =============================================================================
# Kernel compiler flags
# =============================================================================

CFLAGS := \
    --target=aarch64-none-elf \
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
    $(EXTRA_CFLAGS)

ASFLAGS := \
    --target=aarch64-none-elf

CFLAGS += \
    -I. \
    -Ilibk


# =============================================================================
# UIService.framework integration
# =============================================================================

ifeq ($(UISERVICE),1)

CFLAGS += -DNXU_UI_SERVICE -I$(UISERVICE_INCLUDE)

EXTRA_LIBS += $(UISERVICE_LIB)

UISERVICE_DEPS += uiservice-build

endif


# =============================================================================
# WindowServer.framework integration
# =============================================================================

ifeq ($(WINDOWSERVER),1)

CFLAGS += -DNXU_WINDOWSERVER -I$(WINDOWSERVER_INCLUDE)

EXTRA_LIBS += $(WINDOWSERVER_LIB)

WINDOWSERVER_DEPS += windowserver-build

endif


LDFLAGS := \
    -T makedefs/linker.ld \
    -nostdlib \
    -static


# =============================================================================
# Kernel sources
# =============================================================================

C_SOURCES := \
    arch/arm64/cache.c \
    arch/arm64/exception.c \
    arch/arm64/gic.c \
    arch/arm64/thread.c \
    arch/arm64/timer.c \
    drivers/input/input.c \
    drivers/input/keyboard.c \
    drivers/input/mouse.c \
    drivers/block/block_device.c \
    drivers/block/partition.c \
    drivers/video/display.c \
    drivers/video/ramfb_console.c \
    platform/arm64/services/ui_service.c \
    kern/aqua/window_server.c \
    drivers/virtio/virtio.c \
    drivers/virtio/virtio_block.c \
    drivers/virtio/virtio_gpu.c \
    drivers/virtio/virtio_input.c \
    drivers/virtio/virtio_mmio.c \
    drivers/virtio/virtqueue.c \
    kern/boot/boot_args.c \
    kern/boot/boot_mode.c \
    kern/boot/nvram.c \
    kern/boot/splash.c \
    kern/boot/splash_asset.c \
    kern/console/console.c \
    kern/console/bootlog.c \
    kern/exec/elf.c \
    kern/irq/irq.c \
    kern/kern_init.c \
    kern/process/proc.c \
    kern/sched_prism/processor.c \
    kern/sched_prism/run_queue.c \
    kern/sched_prism/sched.c \
    kern/process/task.c \
    kern/memory/heap.c \
    kern/ipc/ipc_init.c \
    kern/ipc/ipc_kmsg.c \
    kern/ipc/ipc_port.c \
    kern/tests/ipc_test.c \
    kern/syscall/syscall.c \
    kern/process/thread.c \
    vfs/ext4.c \
    vfs/jbd2.c \
    vfs/file.c \
    vfs/path.c \
    vfs/ramfs.c \
    vfs/vfs.c \
    vfs/vfs_file.c \
    vfs/vnode.c \
    vm/pmm.c \
    vm/vmm.c \
    vm/address_space.c \
    vm/vmm_tables.c \
    vm/vmm_ttbr1.c \
    vm/vmm_debug.c \
    vm/vm_kern.c \
    vm/user_copy.c \
    platform/arm64/dtb.c \
    platform/dtb_chosen.c \
    platform/arm64/fw_cfg.c \
    platform/arm64/platform.c \
    platform/arm64/uart.c \
    libk/crc32c.c \
    libk/string.c


ASM_SOURCES := \
    arch/arm64/start.S \
    arch/arm64/context_switch.S \
    arch/arm64/exception_vectors.S \
    arch/arm64/transition.S


OBJECTS := \
    $(C_SOURCES:%.c=$(BUILD)/%.o) \
    $(ASM_SOURCES:%.S=$(BUILD)/%.o)


DEPENDENCIES := \
    $(C_SOURCES:%.c=$(BUILD)/%.d)


# =============================================================================
# Userland
# =============================================================================

USER_C_SOURCES := \
    frameworks/lib/syscall.c \
    frameworks/lib/string.c \
    frameworks/CoreFoundation.framework/lib/plist/plist.c \
    frameworks/CoreFoundation.framework/lib/service/service_config.c \
    frameworks/CoreFoundation.framework/sbin/bootd/job.c \
    frameworks/CoreFoundation.framework/sbin/bootd/manager.c \
    frameworks/CoreFoundation.framework/sbin/bootd/main.c \
    frameworks/BootDaemons.framework/logd.c \
    frameworks/BootDaemons.framework/patchd.c \
    frameworks/Recovery.framework/lib/RecoveryServices.c \
    frameworks/Recovery.framework/StartupOptionsUI/drawing.c \
    $(RECOVERY_SANS_SOURCE) \
    $(RECOVERY_MONO_SOURCE) \
    frameworks/Recovery.framework/StartupOptionsUI/font_ttf.c \
    frameworks/Recovery.framework/triageOS/shell.c \
    frameworks/Recovery.framework/triageOS/ui.c \
    frameworks/Recovery.framework/triageOS/main.c


USER_DEPENDENCIES := $(USER_C_SOURCES:%.c=$(USER_BUILD)/%.d)


USER_COMMON_OBJECTS := \
    $(USER_BUILD)/frameworks/crt0.o \
    $(USER_BUILD)/frameworks/lib/syscall.o \
    $(USER_BUILD)/frameworks/lib/string.o


USER_COREFOUNDATION_OBJECTS := \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/plist/plist.o \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/service_config.o


USER_BOOTD_OBJECTS := \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/job.o \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/manager.o \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/main.o


USER_RECOVERY_OBJECTS := \
    $(USER_BUILD)/frameworks/Recovery.framework/lib/RecoveryServices.o \
    $(USER_BUILD)/frameworks/Recovery.framework/StartupOptionsUI/drawing.o \
    $(USER_BUILD)/$(RECOVERY_SANS_SOURCE:.c=.o) \
    $(USER_BUILD)/$(RECOVERY_MONO_SOURCE:.c=.o) \
    $(USER_BUILD)/frameworks/Recovery.framework/StartupOptionsUI/font_ttf.o \
    $(USER_BUILD)/frameworks/Recovery.framework/triageOS/shell.o \
    $(USER_BUILD)/frameworks/Recovery.framework/triageOS/ui.o \
    $(USER_BUILD)/frameworks/Recovery.framework/triageOS/main.o


USER_DAEMONS := \
    $(USER_BUILD)/bootd \
    $(USER_BUILD)/logd \
    $(USER_BUILD)/patchd \
    $(USER_BUILD)/triageOS


USER_SERVICE_PLISTS := \
    frameworks/BootDaemons.framework/Services/com.nxu.logd.plist \
    frameworks/BootDaemons.framework/Services/com.nxu.patchd.plist


USER_STAGE_STAMP := $(USER_BUILD)/.staged


.PHONY: all \
        userland \
        run \
        uiservice-build \
        uiservice-about \
        windowserver-build \
        windowserver-about \
        ui-about \
        apply-assets \
        recovery-assets \
        clean \
        symbolize \
        disk-reset \
        disk-check \
        journal-crash \
        journal-recover


-include $(DEPENDENCIES) $(USER_DEPENDENCIES)


all: $(KERNEL_IMAGE)


symbolize: $(KERNEL)

	@if [ -z "$(ADDR)" ]; then \
		echo "Usage: make symbolize ADDR=0xFFFFFF8040080000"; \
		exit 1; \
	fi

	llvm-addr2line \
		-e $(KERNEL) \
		-f \
		-C \
		$(ADDR)


# =============================================================================
# Compilation rules
# =============================================================================

$(BUILD)/%.o: %.c

	@mkdir -p $(dir $@)

	$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/%.o: %.S

	@mkdir -p $(dir $@)

	$(CC) $(ASFLAGS) -c $< -o $@


$(USER_BUILD)/%.o: %.c

	@mkdir -p $(dir $@)

	$(USER_CC) $(USER_CFLAGS) -c $< -o $@


$(USER_BUILD)/%.o: %.S

	@mkdir -p $(dir $@)

	$(USER_CC) --target=aarch64-none-elf -c $< -o $@


# =============================================================================
# Userland binaries
# =============================================================================

$(USER_BUILD)/bootd: $(USER_COMMON_OBJECTS) $(USER_COREFOUNDATION_OBJECTS) $(USER_BOOTD_OBJECTS)

	$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/logd: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/logd.o

	$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/patchd: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/patchd.o

	$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/triageOS: $(USER_COMMON_OBJECTS) $(USER_RECOVERY_OBJECTS)

	$(USER_LD) $(USER_LDFLAGS) $^ -o $@


userland: $(USER_DAEMONS)


$(USER_STAGE_STAMP): $(USER_DAEMONS) $(USER_SERVICE_PLISTS)

	@mkdir -p $(DISK_ROOT)/System/Library/CoreServices $(DISK_ROOT)/System/Library/BootDaemons $(DISK_ROOT)/System/Recovery $(DISK_ROOT)/var/log $(DISK_ROOT)/var/db/patchd

	cp $(USER_BUILD)/bootd $(DISK_ROOT)/System/Library/CoreServices/bootd

	cp $(USER_BUILD)/bootd $(DISK_ROOT)/System/Library/CoreServices/bootd.recovery

	cp $(USER_BUILD)/logd $(DISK_ROOT)/System/Library/CoreServices/logd

	cp $(USER_BUILD)/logd $(DISK_ROOT)/System/Library/CoreServices/logd.recovery

	cp $(USER_BUILD)/patchd $(DISK_ROOT)/System/Library/CoreServices/patchd

	cp $(USER_BUILD)/patchd $(DISK_ROOT)/System/Library/CoreServices/patchd.recovery

	cp $(USER_BUILD)/triageOS $(DISK_ROOT)/System/Recovery/triageOS

	cp $(USER_SERVICE_PLISTS) $(DISK_ROOT)/System/Library/BootDaemons/

	touch $@


# =============================================================================
# Framework build ordering
# =============================================================================

ifeq ($(UISERVICE),1)

# Only the host bridge includes the generated UIService ABI header.
$(BUILD)/platform/arm64/services/ui_service.o: | uiservice-build

endif


ifeq ($(WINDOWSERVER),1)

# Only the WindowServer host bridge includes the generated WindowServer ABI header.
$(BUILD)/kern/aqua/window_server.o: | windowserver-build

endif


# =============================================================================
# Assets
# =============================================================================

apply-assets:

	@test -d "$(UISERVICE_DIR)" || { echo "UIService.framework not found at $(UISERVICE_DIR)"; exit 1; }

	@mkdir -p "$(UISERVICE_DIR)/assets/Cursors"

	cp assets/UI/Cursors/*.cur "$(UISERVICE_DIR)/assets/Cursors/"

	@echo "Applied sevOS UIService assets to $(UISERVICE_DIR)/assets"


recovery-assets:

	@command -v unzip >/dev/null 2>&1 || { echo "unzip is required to import ArmOS recovery fonts"; exit 1; }

	@command -v python3 >/dev/null 2>&1 || { echo "python3 is required to generate recovery fonts"; exit 1; }

	@python3 -c 'import PIL' >/dev/null 2>&1 || { echo "Pillow is required: python3 -m pip install Pillow"; exit 1; }

	@test -f "$(ARMOS_ARCHIVE)" || { echo "ArmOS archive not found: $(ARMOS_ARCHIVE)"; echo "Use make recovery-assets ARMOS_ARCHIVE=/path/to/armOS-main.zip"; exit 1; }

	@tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT; \
	sans=$$(unzip -Z1 "$(ARMOS_ARCHIVE)" | grep '/recovery-root/System/Library/Fonts/Inter-Regular.ttf$$' | head -1); \
	mono=$$(unzip -Z1 "$(ARMOS_ARCHIVE)" | grep '/recovery-root/System/Library/Fonts/TriageMono-Regular.ttf$$' | head -1); \
	test -n "$$sans" -a -n "$$mono" || { echo "Recovery fonts were not found in $(ARMOS_ARCHIVE)"; exit 1; }; \
	unzip -p "$(ARMOS_ARCHIVE)" "$$sans" > "$$tmp/Inter-Regular.ttf"; \
	unzip -p "$(ARMOS_ARCHIVE)" "$$mono" > "$$tmp/TriageMono-Regular.ttf"; \
	python3 tools/generate_recovery_fonts.py \
		--sans "$$tmp/Inter-Regular.ttf" \
		--mono "$$tmp/TriageMono-Regular.ttf" \
		--out-dir "$(RECOVERY_GENERATED_FONT_DIR)"

	@echo "Generated private Recovery.framework Inter/TriageMono raster assets"

	@echo "Run make clean && make -j$(BUILD_JOBS) all userland before booting triageOS"


# =============================================================================
# Framework builders
# =============================================================================

uiservice-build:

	$(MAKE) -C $(UISERVICE_DIR) BUILD_JOBS=$(BUILD_JOBS) nxu


windowserver-build:

	$(MAKE) -C $(WINDOWSERVER_DIR) BUILD_JOBS=$(BUILD_JOBS) nxu


# =============================================================================
# Kernel linking
# =============================================================================

$(KERNEL): $(OBJECTS) $(UISERVICE_DEPS) $(WINDOWSERVER_DEPS)

	@mkdir -p $(dir $@)

	$(LD) $(LDFLAGS) $(OBJECTS) $(EXTRA_LIBS) -o $@


$(KERNEL_IMAGE): $(KERNEL)

	$(OBJCOPY) -O binary $(KERNEL) $(KERNEL_IMAGE)


# =============================================================================
# Disk image
# =============================================================================

$(DISK): $(USER_STAGE_STAMP)

	@command -v $(MKFS_EXT4) >/dev/null 2>&1 || { \
		echo "mkfs.ext4 is required (install e2fsprogs)"; \
		exit 1; \
	}

	truncate -s $(DISK_SIZE) $(DISK)

	$(MKFS_EXT4) -F -q -b 4096 -I 256 \
		-O has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit \
		-E lazy_itable_init=0,lazy_journal_init=0 \
		-L NXU -d $(DISK_ROOT) $(DISK)


$(DISK_FORMAT_STAMP):

	rm -f $(DISK)

	$(MAKE) $(DISK)

	touch $(DISK_FORMAT_STAMP)


# =============================================================================
# Normal boot
# =============================================================================

run: $(KERNEL_IMAGE) $(DISK) $(DISK_FORMAT_STAMP)

	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-smp 1 \
		-m 512M \
		-kernel $(KERNEL_IMAGE) \
		-append "$(BOOT_ARGS)" \
		-display cocoa \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		$(QEMU_RAMFB_DEVICE) \
		-device virtio-blk-device,drive=nxudisk \
		-device virtio-gpu-device \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		-serial stdio \
		-monitor none


# =============================================================================
# UIService boot test
# =============================================================================

uiservice-about: $(DISK) $(DISK_FORMAT_STAMP)

	@echo "NXU: building UIService boot target with $(BUILD_JOBS) host job(s)"

	$(MAKE) -j$(BUILD_JOBS) \
		BUILD_ROOT=BUILD CONFIG=uiservice \
		UISERVICE=1 \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS) -DNXU_UI_SERVICE_BOOT_TEST" \
		all

	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-smp 1 \
		-m 512M \
		-kernel build-uiservice/kernel.bin \
		-append "$(BOOT_ARGS)" \
		-display cocoa \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		$(QEMU_RAMFB_DEVICE) \
		-device virtio-blk-device,drive=nxudisk \
		-device virtio-gpu-device \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		-serial stdio \
		-monitor none


# =============================================================================
# WindowServer boot test
# =============================================================================

windowserver-about: $(DISK) $(DISK_FORMAT_STAMP)

	@echo "NXU: building WindowServer boot target with $(BUILD_JOBS) host job(s)"

	$(MAKE) -j$(BUILD_JOBS) \
		BUILD_ROOT=BUILD CONFIG=windowserver \
		WINDOWSERVER=1 \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS) -DNXU_WINDOWSERVER_BOOT_TEST" \
		all

	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-smp 1 \
		-m 512M \
		-kernel BUILD/windowserver/kernel.bin \
		-append "$(BOOT_ARGS)" \
		-display cocoa \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		$(QEMU_RAMFB_DEVICE) \
		-device virtio-blk-device,drive=nxudisk \
		-device virtio-gpu-device \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		-serial stdio \
		-monitor none


# =============================================================================
# Disk tools
# =============================================================================

disk-reset:

	rm -f $(DISK) $(DISK_FORMAT_STAMP)

	$(MAKE) $(DISK_FORMAT_STAMP)


disk-check:

	@command -v $(E2FSCK) >/dev/null 2>&1 || { \
		echo "e2fsck is required (install e2fsprogs)"; \
		exit 1; \
	}

	$(E2FSCK) -fn $(DISK)


# =============================================================================
# Journal tests
# =============================================================================

journal-crash: $(DISK) $(DISK_FORMAT_STAMP)

	$(MAKE) BUILD_ROOT=BUILD CONFIG=journal-crash EXTRA_CFLAGS=-DNXU_JOURNAL_CRASH_TEST all

	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-smp 1 \
		-m 512M \
		-kernel BUILD/journal-crash/kernel.bin \
		-append "$(BOOT_ARGS)" \
		-display gtk \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		-device virtio-blk-device,drive=nxudisk \
		-device virtio-gpu-device \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		-serial stdio \
		-monitor none


journal-recover: run


# =============================================================================
# Cleanup
# =============================================================================

clean:

	rm -rf $(BUILD_ROOT)

	rm -f $(DISK_ROOT)/System/Library/CoreServices/bootd

	rm -f $(DISK_ROOT)/System/Library/CoreServices/bootd.recovery

	rm -f $(DISK_ROOT)/System/Library/CoreServices/logd

	rm -f $(DISK_ROOT)/System/Library/CoreServices/logd.recovery

	rm -f $(DISK_ROOT)/System/Library/CoreServices/patchd

	rm -f $(DISK_ROOT)/System/Library/CoreServices/patchd.recovery

	rm -f $(DISK_ROOT)/System/Recovery/triageOS

	rm -rf $(DISK_ROOT)/System/Library/BootDaemons


ui-about: uiservice-about
