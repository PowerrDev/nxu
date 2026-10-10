# i386 desktop runs WindowServer and DesktopService in user processes.
# UISERVICE/WINDOWSERVER remain available for legacy kernel bring-up builds.

I386_C_SOURCES += \
    kern/aqua/window_server.c \
    drivers/video/ui_service_login.c \
    drivers/video/trusted_login.c \
    drivers/video/ui_service_activity.c \
    drivers/video/ui_service_bridge.c \
    platform/i386/services/ui_service.c

UISERVICE_I386_INCLUDE := $(UISERVICE_DIR)/build-i386
UISERVICE_I386_LIB := $(UISERVICE_I386_INCLUDE)/libUIService.a
WINDOWSERVER_I386_SERVICE_LIB := $(WINDOWSERVER_DIR)/BUILD-i386/libWindowServerService.a

ifeq ($(UISERVICE),1)
I386_CFLAGS += -DNXU_UI_SERVICE -I$(UISERVICE_I386_INCLUDE)
I386_EXTRA_LIBS += $(UISERVICE_I386_LIB)
endif

ifeq ($(WINDOWSERVER),1)
I386_CFLAGS += -DNXU_WINDOWSERVER -I$(WINDOWSERVER_INCLUDE)
I386_EXTRA_LIBS += $(WINDOWSERVER_I386_SERVICE_LIB)
endif

I386_DESKTOP_BUILD_ROOT ?= BUILD/BUILD-i386-desktop
I386_DESKTOP_KERNEL := $(I386_DESKTOP_BUILD_ROOT)/i386/kernel.elf

# Real SMP (see doc/i386/smp.md): CPUs beyond the boot one come up through
# ACPI MADT discovery and INIT-SIPI-SIPI, not a fixed assumption baked into
# the kernel, so this is just how many qemu-system-i386/x86_64 hands the
# guest -- override with e.g. `make run-i386-desktop I386_DESKTOP_SMP=1` to
# go back to a single CPU.
I386_DESKTOP_SMP ?= 4

# HVF requires an x86 host. Other hosts use TCG for the i386 guest.
I386_DESKTOP_HVF := $(shell test "$$(uname -m)" = x86_64 && qemu-system-x86_64 -accel help 2>/dev/null | grep -qx hvf && echo 1)

ifeq ($(I386_DESKTOP_HVF),1)
I386_DESKTOP_QEMU := qemu-system-x86_64 -accel hvf -cpu qemu32
else
I386_DESKTOP_QEMU := qemu-system-i386
endif

.PHONY: i386-desktop run-i386-desktop

i386-desktop:
	$(MAKE) i386 i386-frameworks \
		BUILD_ROOT=$(I386_DESKTOP_BUILD_ROOT) \
		UISERVICE=0 WINDOWSERVER=0 I386_GUI_FRAMEWORKS=1

# QEMU_GPU_XRES/YRES and QEMU_UI_SCALE_PERMILLE (root Makefile) are the same
# host-resolution/backingScaleFactor detection arm64's `make run` uses, not
# i386-specific: reusing them here (instead of some other fixed resolution)
# is what keeps the window sized to the host's real usable desktop area and
# UIService's chrome/fonts scaled correctly for it, rather than picking an
# arbitrary size that happens to render but isn't actually correct for this
# host.
#
# Like run-i386 it boots tepOS alongside (tools/with_tepos.sh, TEP=0 to skip),
# so the desktop is locked behind the tepOS passcode, and it has the host's
# speakers as a VirtIO sound device for the boot chime.
#
# virtio-irq=1: the VirtIO devices interrupt (PCI INTx) instead of being
# polled. Polled, the mouse was only read when the desktop's 10 ms timer tick
# woke it, about 50 frames a second however fast the pointer reported; an
# input interrupt wakes it at once (80-110 frames a second measured). The
# kernel's own default stays polling (doc/i386/devices.md).
run-i386-desktop: i386-desktop

	$(MAKE) i386-disk BUILD_ROOT=$(I386_DESKTOP_BUILD_ROOT) I386_DESKTOP_APPS=1 I386_GUI_FRAMEWORKS=1 I386_DISK_SIZE=64M

	$(I386_TEP_WRAPPER) $(I386_DESKTOP_QEMU) -M pc -smp $(I386_DESKTOP_SMP) \
		-kernel $(I386_DESKTOP_KERNEL) \
		-m 512M \
		-vga none \
		-device virtio-blk-pci,drive=d0,disable-modern=on \
		-drive if=none,format=raw,file=$(I386_DESKTOP_BUILD_ROOT)/i386/disk.img,id=d0 \
		-device virtio-keyboard-pci,disable-modern=on \
		-device virtio-mouse-pci,disable-modern=on \
		-device virtio-gpu-pci,xres=$(QEMU_GPU_XRES),yres=$(QEMU_GPU_YRES) \
		$(I386_QEMU_AUDIO) \
		-serial stdio \
		-append "$(BOOT_ARGS) test=desktop ui.userland=1 virtio-irq=1 log.trace=0 log.stats_ms=0 ui.scale=$(QEMU_UI_SCALE_PERMILLE)"

$(I386_BUILD)/user/reporter_fonts.h: tools/generate_reporter_fonts.py tools/generate_recovery_fonts.py $(USER_SYSTEM_FONTS)
	$(Q)python3 tools/generate_reporter_fonts.py --output $@

$(I386_BUILD)/drivers/video/trusted_login.o $(I386_BUILD)/kern/security/sap.o: $(I386_BUILD)/user/reporter_fonts.h
$(I386_BUILD)/drivers/video/trusted_login.o $(I386_BUILD)/kern/security/sap.o: I386_CFLAGS += -I$(I386_USER_BUILD)
