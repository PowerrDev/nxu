.DEFAULT_GOAL := all

BUILD_ROOT ?= BUILD
CONFIG ?= default
BUILD := $(BUILD_ROOT)/$(CONFIG)

USER_BUILD := $(BUILD_ROOT)/userland

BOOT_ARGS ?=

# Native virtio-gpu scanout resolution for the GUI boot targets: the host
# display's PHYSICAL pixel count (not its point/logical resolution). QEMU's
# cocoa backend (ui/cocoa.m resizeWindow) sizes a non-resizable window as
# guest_pixels / [NSWindow backingScaleFactor] and presents the framebuffer
# 1:1 against physical pixels, i.e. genuine HiDPI - no scaling, no blur.
# That path only runs when the window is NOT resizable, so leave zoom-to-fit
# and full-screen off; enabling either makes QEMU stretch a smaller raster
# to fill the window instead, which is blurry.
#
# Auto-detected below from the main screen's *visible* frame (full panel
# resolution minus the menu bar and Dock), converted to physical pixels via
# backingScaleFactor, so the guest scanout always matches the desktop area
# actually available on whatever machine is running `make`. Sizing against
# the full panel resolution instead (e.g. `system_profiler`'s "Resolution:"
# line) is still wrong even though it matches the display: QEMU's window sits
# below the menu bar like any other window, so a window as tall as the whole
# panel runs under the Dock and off the bottom of the usable desktop -- a
# mismatch here is not cosmetic, since QEMU sizes a non-resizable cocoa
# window 1:1 against these guest pixels / backingScaleFactor, and a guest
# resolution taller/wider than the *usable* area also throws off where the
# titlebar/chrome actually land on screen relative to where WindowServer
# thinks they are.
#
# Falls back to 2732x1536 (a 2x-Retina 1366x768-point display) if detection
# fails (non-macOS host, or `osascript`/AppKit unavailable). Override
# explicitly with e.g. `make test TEST=windowserver-about QEMU_GPU_XRES=... QEMU_GPU_YRES=...`
# if you want a different guest resolution than your host's usable desktop area.
DETECTED_RESOLUTION := $(shell osascript -l JavaScript -e 'ObjC.import("AppKit"); var s = $$.NSScreen.mainScreen; var f = s.visibleFrame; var k = s.backingScaleFactor; Math.round(f.size.width * k) + " " + Math.round(f.size.height * k);' 2>/dev/null)
DETECTED_GPU_XRES := $(word 1,$(DETECTED_RESOLUTION))
DETECTED_GPU_YRES := $(word 2,$(DETECTED_RESOLUTION))

QEMU_GPU_XRES ?= $(if $(DETECTED_GPU_XRES),$(DETECTED_GPU_XRES),2732)
QEMU_GPU_YRES ?= $(if $(DETECTED_GPU_YRES),$(DETECTED_GPU_YRES),1536)
QEMU_GPU_DEVICE := -device virtio-gpu-device,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES)

# The host's real content scale (physical pixels per point, e.g. 2.0 on a
# Retina display, 1.0 on a plain external monitor), as thousandths so it can
# travel as a plain integer boot arg. UIService.framework's window chrome,
# fonts and control metrics were all authored assuming a fixed 2x-Retina
# canvas (see WindowServer.framework's compositor doc comments); without
# this, that assumption silently breaks whenever this host's actual
# backingScaleFactor isn't exactly 2 -- QEMU still sizes its window to the
# usable desktop area correctly (per the resolution math above), but
# everything UIService draws inside it ends up rendered for a density that
# doesn't match, which reads as the whole scene being "zoomed" in or out.
# Passed through to the kernel below as `ui.scale=<permille>` (see
# `ui_service_bootstrap` and `ui_core::scale` on the UIService side) so it
# can rescale instead of assuming 2x unconditionally. Falls back to 2000
# (2.0x) under the same conditions QEMU_GPU_XRES/YRES fall back to 2732x1536.
DETECTED_SCALE_PERMILLE := $(shell osascript -l JavaScript -e 'ObjC.import("AppKit"); Math.round($$.NSScreen.mainScreen.backingScaleFactor * 1000);' 2>/dev/null)
QEMU_UI_SCALE_PERMILLE ?= $(if $(DETECTED_SCALE_PERMILLE),$(DETECTED_SCALE_PERMILLE),2000)

# Host display backend; run-console and the graphical tests both use it.
QEMU_DISPLAY_BACKEND ?= cocoa

QEMU_DISPLAY := -display $(QEMU_DISPLAY_BACKEND)

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

DISK_ROOT ?= tools/DiskRoot

DISK_FORMAT_STAMP ?= .nxu-ext4-jbd2-format

MKFS_EXT4 ?= mkfs.ext4

E2FSCK ?= e2fsck

CC := clang

LD := ld.lld

OBJCOPY := llvm-objcopy

USER_CC := ccache clang

USER_LD := ld.lld

RECOVERY_GENERATED_FONT_DIR := tools/Recovery/Generated

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

USER_LDFLAGS := --allow-multiple-definition -T makedefs/user.ld -nostdlib -static

# Use all host logical CPUs for recursive kernel/framework builds by default.
BUILD_JOBS ?= $(shell sysctl -n hw.logicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)

export CARGO_BUILD_JOBS ?= $(BUILD_JOBS)

# Quiet-by-default build output: short "TAG  path" status lines instead of
# full compiler/linker invocations (pass V=1 to print the real commands).
ifeq ($(V),1)
Q :=
QUIET_PRINT = @:
else
Q := @
QUIET_PRINT = @printf "  %-7s %s\n"
endif


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

WINDOWSERVER_INCLUDE := $(WINDOWSERVER_DIR)/include

# Userland archive: windowserver-nxu's compositor built as a standalone
# staticlib (WindowServer.framework's nxu/ wrapper crate) for linking into
# a real NXU process instead of the kernel -- see
# frameworks/BootDaemons.framework/windowserver_service.c.
WINDOWSERVER_SERVICE_LIB := $(WINDOWSERVER_DIR)/BUILD/libWindowServerService.a

USER_CFLAGS += -I$(WINDOWSERVER_INCLUDE)


# =============================================================================
# Framework linker state
# =============================================================================

EXTRA_LIBS :=

UISERVICE_DEPS :=



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



endif


LDFLAGS := \
    -T makedefs/linker.ld \
    -nostdlib \
    -static


# =============================================================================
# Kernel sources
# =============================================================================

C_SOURCES := \
    kern/arm64/cache.c \
    kern/arm64/exception.c \
    kern/arm64/gic.c \
    kern/arm64/thread.c \
    kern/arm64/user.c \
    kern/arm64/timer.c \
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
    drivers/virtio/virtio_sound.c \
    drivers/virtio/virtio_sound_core.c \
    drivers/virtio/virtqueue.c \
    kern/boot/boot_args.c \
    kern/boot/boot_mode.c \
    kern/boot/nvram.c \
    kern/boot/splash.c \
    kern/boot/splash_asset.c \
    kern/console/console.c \
    kern/console/bootlog.c \
    kern/console/font8x16.c \
    kern/console/ioregistry.c \
    kern/loader/elf.c \
    kern/irq/irq.c \
    kern/kern_init.c \
    kern/process/proc.c \
    kern/process/signal.c \
    kern/sched_prism/processor.c \
    kern/sched_prism/run_queue.c \
    kern/sched_prism/waitq.c \
    kern/sched_prism/sched.c \
    kern/process/task.c \
    kern/memory/heap.c \
    kern/ipc/ipc_init.c \
    kern/ipc/ipc_kmsg.c \
    kern/ipc/ipc_port.c \
    kern/ipc/ipc_space.c \
    kern/ipc/shm_registry.c \
    kern/ipc/socket.c \
    kern/console/display_owner.c \
    kern/tests/ipc_test.c \
    kern/tests/post.c \
    kern/tests/post_storage.c \
    kern/tests/boot_test.c \
    kern/tests/vm_shm_test.c \
    kern/tests/vm_map_test.c \
    kern/tests/ipc_process_test.c \
    kern/tests/thread_process_test.c \
    kern/tests/fault_process_test.c \
    kern/tests/process_control_test.c \
    kern/tests/socket_process_test.c \
    kern/tests/xamethyst_process_test.c \
    kern/tests/windowserver_process_test.c \
    kern/tests/about_sevos_process_test.c \
    kern/syscall/syscall.c \
    kern/process/thread.c \
    vfs/btrfs/btrfs_chunk.c \
    vfs/btrfs/btrfs_csum.c \
    vfs/btrfs/btrfs_dir.c \
    vfs/btrfs/btrfs_file.c \
    vfs/btrfs/btrfs_fs.c \
    vfs/btrfs/btrfs_inode.c \
    vfs/btrfs/btrfs_io.c \
    vfs/btrfs/btrfs_io_block.c \
    vfs/btrfs/btrfs_root.c \
    vfs/btrfs/btrfs_selftest.c \
    vfs/btrfs/btrfs_super.c \
    vfs/btrfs/btrfs_tree.c \
    vfs/btrfs/btrfs_vfs.c \
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
    vm/vm_shm.c \
    vm/vm_map.c \
    vm/vm_fault.c \
    vm/vmm_tables.c \
    vm/vmm_ttbr1.c \
    vm/vmm_debug.c \
    vm/vm_kern.c \
    vm/user_copy.c \
    platform/arm64/driverkit.c \
    platform/arm64/dtb.c \
    platform/dtb_chosen.c \
    platform/arm64/fw_cfg.c \
    platform/arm64/platform.c \
    platform/arm64/rtc.c \
    platform/arm64/uart.c \
    libk/crc32c.c \
    libk/string.c


ASM_SOURCES := \
    kern/arm64/start.S \
    kern/arm64/context_switch.S \
    kern/arm64/exception_vectors.S \
    kern/arm64/transition.S


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
    frameworks/lib/thread.c \
    frameworks/CoreFoundation.framework/lib/plist/plist.c \
    frameworks/CoreFoundation.framework/lib/service/service_config.c \
    frameworks/CoreFoundation.framework/lib/service/bootstrap_client.c \
    frameworks/CoreFoundation.framework/sbin/bootd/job.c \
    frameworks/CoreFoundation.framework/sbin/bootd/manager.c \
    frameworks/CoreFoundation.framework/sbin/bootd/registry.c \
    frameworks/CoreFoundation.framework/sbin/bootd/main.c \
    frameworks/BootDaemons.framework/logd.c \
    frameworks/BootDaemons.framework/patchd.c \
    frameworks/BootDaemons.framework/ipctest_a.c \
    frameworks/BootDaemons.framework/ipctest_b.c \
    frameworks/BootDaemons.framework/threadtest.c \
    frameworks/BootDaemons.framework/faulttest.c \
    frameworks/BootDaemons.framework/proctest.c \
    frameworks/BootDaemons.framework/execchild.c \
    frameworks/BootDaemons.framework/privtest.c \
    frameworks/BootDaemons.framework/sockettest_server.c \
    frameworks/BootDaemons.framework/sockettest_client.c \
    frameworks/BootDaemons.framework/xamethyst.c \
    frameworks/BootDaemons.framework/x11test_handshake.c \
    frameworks/BootDaemons.framework/x11test_input.c \
    frameworks/BootDaemons.framework/windowserver_service.c \
    frameworks/BootDaemons.framework/wstest_client.c \
    frameworks/BootDaemons.framework/about_sevos_service.c \
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
    $(USER_BUILD)/frameworks/lib/string.o \
    $(USER_BUILD)/frameworks/lib/thread.o


USER_COREFOUNDATION_OBJECTS := \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/plist/plist.o \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/service_config.o \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o


USER_BOOTD_OBJECTS := \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/job.o \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/manager.o \
    $(USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/registry.o \
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
    $(USER_BUILD)/triageOS \
    $(USER_BUILD)/ipctest_a \
    $(USER_BUILD)/ipctest_b \
    $(USER_BUILD)/threadtest \
    $(USER_BUILD)/faulttest \
    $(USER_BUILD)/proctest \
    $(USER_BUILD)/execchild \
    $(USER_BUILD)/privtest \
    $(USER_BUILD)/sockettest_server \
    $(USER_BUILD)/sockettest_client \
    $(USER_BUILD)/xamethyst \
    $(USER_BUILD)/x11test_handshake \
    $(USER_BUILD)/x11test_input \
    $(USER_BUILD)/windowserver_service \
    $(USER_BUILD)/wstest_client \
    $(USER_BUILD)/about_sevos_service


USER_SERVICE_PLISTS := \
    frameworks/BootDaemons.framework/Services/com.nxu.logd.plist \
    frameworks/BootDaemons.framework/Services/com.nxu.patchd.plist \
    frameworks/BootDaemons.framework/Services/com.nxu.windowserver.plist \
    frameworks/BootDaemons.framework/Services/com.nxu.about-sevos.plist


USER_STAGE_STAMP := $(USER_BUILD)/.staged


.PHONY: all \
        userland \
        run \
        run-console \
        uiservice-build \
        windowserver-build \
        apply-assets \
        recovery-assets \
        clean \
        symbolize \
        disk-reset \
        disk-check


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

	$(QUIET_PRINT) "CC" "$<"

	$(Q)$(CC) $(CFLAGS) -c $< -o $@


$(BUILD)/%.o: %.S

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "AS" "$<"

	$(Q)$(CC) $(ASFLAGS) -c $< -o $@


$(USER_BUILD)/%.o: %.c

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "CC" "$<"

	$(Q)$(USER_CC) $(USER_CFLAGS) -c $< -o $@


$(USER_BUILD)/%.o: %.S

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "AS" "$<"

	$(Q)$(USER_CC) --target=aarch64-none-elf -c $< -o $@


# =============================================================================
# Userland binaries
# =============================================================================

$(USER_BUILD)/bootd: $(USER_COMMON_OBJECTS) $(USER_COREFOUNDATION_OBJECTS) $(USER_BOOTD_OBJECTS)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/logd: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/logd.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/patchd: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/patchd.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/triageOS: $(USER_COMMON_OBJECTS) $(USER_RECOVERY_OBJECTS)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/ipctest_a: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o $(USER_BUILD)/frameworks/BootDaemons.framework/ipctest_a.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/ipctest_b: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o $(USER_BUILD)/frameworks/BootDaemons.framework/ipctest_b.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/threadtest: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/threadtest.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/faulttest: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/faulttest.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/proctest: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/proctest.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/execchild: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/execchild.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/privtest: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/privtest.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/sockettest_server: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/sockettest_server.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/sockettest_client: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/sockettest_client.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/xamethyst: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/xamethyst.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/x11test_handshake: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/x11test_handshake.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/x11test_input: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/BootDaemons.framework/x11test_input.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/windowserver_service: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o $(USER_BUILD)/frameworks/BootDaemons.framework/windowserver_service.o $(WINDOWSERVER_SERVICE_LIB) | windowserver-build

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/wstest_client: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o $(USER_BUILD)/frameworks/BootDaemons.framework/wstest_client.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


$(USER_BUILD)/about_sevos_service: $(USER_COMMON_OBJECTS) $(USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o $(USER_BUILD)/frameworks/BootDaemons.framework/about_sevos_service.o

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(USER_LD) $(USER_LDFLAGS) $^ -o $@


userland: $(USER_DAEMONS)


$(USER_STAGE_STAMP): $(USER_DAEMONS) $(USER_SERVICE_PLISTS)

	@mkdir -p $(DISK_ROOT)/System/Library/CoreServices $(DISK_ROOT)/System/Library/BootDaemons $(DISK_ROOT)/System/Recovery $(DISK_ROOT)/var/log $(DISK_ROOT)/var/db/patchd

	$(QUIET_PRINT) "STAGE" "$(DISK_ROOT)"

	$(Q)cp $(USER_BUILD)/bootd $(DISK_ROOT)/System/Library/CoreServices/bootd

	$(Q)cp $(USER_BUILD)/bootd $(DISK_ROOT)/System/Library/CoreServices/bootd.recovery

	$(Q)cp $(USER_BUILD)/logd $(DISK_ROOT)/System/Library/CoreServices/logd

	$(Q)cp $(USER_BUILD)/logd $(DISK_ROOT)/System/Library/CoreServices/logd.recovery

	$(Q)cp $(USER_BUILD)/patchd $(DISK_ROOT)/System/Library/CoreServices/patchd

	$(Q)cp $(USER_BUILD)/patchd $(DISK_ROOT)/System/Library/CoreServices/patchd.recovery

	$(Q)cp $(USER_BUILD)/triageOS $(DISK_ROOT)/System/Recovery/triageOS

	$(Q)cp $(USER_BUILD)/ipctest_a $(DISK_ROOT)/System/Library/CoreServices/ipctest_a

	$(Q)cp $(USER_BUILD)/ipctest_b $(DISK_ROOT)/System/Library/CoreServices/ipctest_b

	$(Q)cp $(USER_BUILD)/threadtest $(DISK_ROOT)/System/Library/CoreServices/threadtest

	$(Q)cp $(USER_BUILD)/faulttest $(DISK_ROOT)/System/Library/CoreServices/faulttest

	$(Q)cp $(USER_BUILD)/proctest $(DISK_ROOT)/System/Library/CoreServices/proctest

	$(Q)cp $(USER_BUILD)/execchild $(DISK_ROOT)/System/Library/CoreServices/execchild

	$(Q)cp $(USER_BUILD)/privtest $(DISK_ROOT)/System/Library/CoreServices/privtest

	$(Q)cp $(USER_BUILD)/sockettest_server $(DISK_ROOT)/System/Library/CoreServices/sockettest_server

	$(Q)cp $(USER_BUILD)/sockettest_client $(DISK_ROOT)/System/Library/CoreServices/sockettest_client

	$(Q)cp $(USER_BUILD)/xamethyst $(DISK_ROOT)/System/Library/CoreServices/xamethyst

	$(Q)cp $(USER_BUILD)/x11test_handshake $(DISK_ROOT)/System/Library/CoreServices/x11test_handshake

	$(Q)cp $(USER_BUILD)/x11test_input $(DISK_ROOT)/System/Library/CoreServices/x11test_input

	$(Q)cp $(USER_BUILD)/windowserver_service $(DISK_ROOT)/System/Library/CoreServices/windowserver_service

	$(Q)cp $(USER_BUILD)/wstest_client $(DISK_ROOT)/System/Library/CoreServices/wstest_client

	$(Q)cp $(USER_BUILD)/about_sevos_service $(DISK_ROOT)/System/Library/CoreServices/about_sevos_service

	$(Q)cp $(USER_SERVICE_PLISTS) $(DISK_ROOT)/System/Library/BootDaemons/

	@touch $@


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

	cp tools/UI/Cursors/*.cur "$(UISERVICE_DIR)/assets/Cursors/"

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

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(LD) $(LDFLAGS) $(OBJECTS) $(EXTRA_LIBS) -o $@


$(KERNEL_IMAGE): $(KERNEL)

	$(QUIET_PRINT) "OBJCOPY" "$@"

	$(Q)$(OBJCOPY) -O binary $(KERNEL) $(KERNEL_IMAGE)


# =============================================================================
# Disk image
# =============================================================================

$(DISK): $(USER_STAGE_STAMP)

	@command -v $(MKFS_EXT4) >/dev/null 2>&1 || { \
		echo "mkfs.ext4 is required (install e2fsprogs)"; \
		exit 1; \
	}

	$(QUIET_PRINT) "MKFS" "$@"

	$(Q)truncate -s $(DISK_SIZE) $(DISK)

	$(Q)$(MKFS_EXT4) -F -q -b 4096 -I 256 \
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

# `run` boots the full graphical stack (WindowServer + UIService, Voyager app)
# -- equivalent to `make test TEST=ui-voyager` (see makedefs/tests.mk). For the plain kernel-only console boot (no sibling framework
# repos required; what `run` used to be), use `run-console`.
run:

	$(MAKE) test TEST=ui-voyager


run-console: $(KERNEL_IMAGE) $(DISK) $(DISK_FORMAT_STAMP)

	qemu-system-aarch64 \
		-machine virt,gic-version=3 \
		-cpu cortex-a72 \
		-smp 1 \
		-m 512M \
		-kernel $(KERNEL_IMAGE) \
		-append "$(BOOT_ARGS) ui.scale=$(QEMU_UI_SCALE_PERMILLE)" \
		$(QEMU_DISPLAY) \
		-global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file=$(DISK),id=nxudisk \
		$(QEMU_RAMFB_DEVICE) \
		-device virtio-blk-device,drive=nxudisk \
		$(QEMU_GPU_DEVICE) \
		-device virtio-keyboard-device \
		-device virtio-mouse-device \
		-serial stdio \
		-monitor none


# =============================================================================
# Version
# =============================================================================

# Recompute NXU_VERSION and NXU_BUILD in kern/logging/version.h from the number
# of commits (see tools/version.sh for the scheme). Run it before tagging or
# demoing a build; it only rewrites the header.
.PHONY: version

version:

	tools/version.sh --write


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
# Btrfs (read-only driver): host and arm64 tests
# =============================================================================
#
#   make test-btrfs-host    the pure core natively with ASan+UBSan over every
#                           fixture (tools/btrfs/test_host.sh)
#   make test-arm64-btrfs   boot the arm64 kernel headless with fixtures on
#                           extra virtio-blk-device disks (tools/btrfs/test_arm64.sh)
#   make test-i386-btrfs    the same on i386 (makedefs/i386/btrfs.mk)
#
# Nothing here touches disk.img or tools/DiskRoot; images are private copies.

BTRFS_HOST_TOOL := $(BUILD_ROOT)/btrfs-host/btrfs_host

BTRFS_CORE_SOURCES := \
    vfs/btrfs/btrfs_chunk.c \
    vfs/btrfs/btrfs_csum.c \
    vfs/btrfs/btrfs_dir.c \
    vfs/btrfs/btrfs_file.c \
    vfs/btrfs/btrfs_fs.c \
    vfs/btrfs/btrfs_inode.c \
    vfs/btrfs/btrfs_io.c \
    vfs/btrfs/btrfs_io_host.c \
    vfs/btrfs/btrfs_root.c \
    vfs/btrfs/btrfs_super.c \
    vfs/btrfs/btrfs_tree.c

BTRFS_HOST_CC ?= cc

BTRFS_HOST_CFLAGS := -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror

.PHONY: btrfs-host test-btrfs-host test-arm64-btrfs

btrfs-host: $(BTRFS_HOST_TOOL)

$(BTRFS_HOST_TOOL): tools/btrfs/host/btrfs_host.c $(BTRFS_CORE_SOURCES) $(wildcard vfs/btrfs/*.h)

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "CC" "$@"

	$(Q)$(BTRFS_HOST_CC) $(BTRFS_HOST_CFLAGS) tools/btrfs/host/btrfs_host.c $(BTRFS_CORE_SOURCES) -o $@


test-btrfs-host: $(BTRFS_HOST_TOOL)

	tools/btrfs/test_host.sh $(BTRFS_HOST_TOOL) $(BUILD_ROOT)/btrfs-host/scratch


test-arm64-btrfs: $(KERNEL_IMAGE) $(BTRFS_HOST_TOOL)

	tools/btrfs/test_arm64.sh $(KERNEL_IMAGE) $(DISK) $(BUILD_ROOT)/btrfs-arm64 $(BTRFS_HOST_TOOL)


# =============================================================================
# Audio: host tests
# =============================================================================
#
#   make test-audio-host    the pure audio code (sound core, WAV parser) natively
#                           with ASan+UBSan (tools/audio/test_host.sh)
#
# The kernel-side tests (a tone and the boot chime played through the VirtIO
# Sound driver into QEMU's wav backend, then checked on the host) are in the
# registry in makedefs/tests.mk.

.PHONY: test-audio-host

test-audio-host:

	tools/audio/test_host.sh $(BUILD_ROOT)/audio-host


include makedefs/tests.mk

include makedefs/i386.mk


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

