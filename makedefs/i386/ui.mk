# =============================================================================
# i386 desktop: WindowServer.framework + UIService.framework on VirtIO-GPU
# =============================================================================
#
#   make i386-desktop        build BUILD-i386-desktop/i386/kernel.elf with the
#                             compositor linked in (also builds both
#                             frameworks for i686-nxu-none first)
#   make run-i386-desktop    boot it under qemu-system-i386 with virtio-gpu,
#                             keyboard and mouse over PCI
#
# platform/i386/services/ui_service.c and kern/aqua/window_server.c are the
# i386 side of the same kernel-embedded compositor bring-up arm64's "desktop"
# test id uses (see doc/testing.md): WindowServer and UIService run directly
# on the boot context via kern/i386/userland_init.c's "test=desktop" branch,
# not spawned through bootd. Both files degrade to a clean "not linked"
# return when NXU_UI_SERVICE/NXU_WINDOWSERVER are unset, so they are safe to
# always build -- only the ifeq blocks below, gated on the same UISERVICE/
# WINDOWSERVER toggles the arm64 integration above uses, actually pull in the
# Rust archives and switch the real bridge on. A plain `make i386` never sets
# either, so the default i386 build (and every existing test-i386-* target)
# is unaffected.
#
# i686-nxu-none.json (see both frameworks' Makefiles) is a custom bare-metal
# x86-32 Rust target with no prebuilt std, so their nxu-i386 targets build
# into their own BUILD-i386/build-i386 directories rather than the arm64
# integration's BUILD/build -- the two architectures' archives never collide,
# and a plain `make nxu`/`make apply-assets` for arm64 is untouched.

I386_C_SOURCES += \
    kern/aqua/window_server.c \
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

I386_DESKTOP_BUILD_ROOT ?= BUILD-i386-desktop
I386_DESKTOP_KERNEL := $(I386_DESKTOP_BUILD_ROOT)/i386/kernel.elf

.PHONY: i386-desktop run-i386-desktop

i386-desktop:

	$(MAKE) -C $(WINDOWSERVER_DIR) BUILD=BUILD-i386 nxu-i386

	$(MAKE) -C $(UISERVICE_DIR) nxu-i386

	$(MAKE) i386 \
		BUILD_ROOT=$(I386_DESKTOP_BUILD_ROOT) \
		UISERVICE=1 \
		WINDOWSERVER=1 \
		EXTRA_CFLAGS="-DNXU_UI_SERVICE_APP_VOYAGER"


run-i386-desktop: i386-desktop

	$(MAKE) i386-disk BUILD_ROOT=$(I386_DESKTOP_BUILD_ROOT)

	qemu-system-i386 -M pc \
		-kernel $(I386_DESKTOP_KERNEL) \
		-m 512M \
		-vga none \
		-device virtio-blk-pci,drive=d0,disable-modern=on \
		-drive if=none,format=raw,file=$(I386_DESKTOP_BUILD_ROOT)/i386/disk.img,id=d0 \
		-device virtio-keyboard-pci,disable-modern=on \
		-device virtio-mouse-pci,disable-modern=on \
		-device virtio-gpu-pci,xres=1024,yres=768 \
		-serial stdio -no-reboot \
		-append "test=desktop"
