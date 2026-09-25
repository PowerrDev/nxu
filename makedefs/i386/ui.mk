# =============================================================================
# i386 desktop: WindowServer.framework + UIService.framework on VirtIO-GPU
# =============================================================================
#
#   make i386-desktop        build BUILD-i386-desktop/i386/kernel.elf with the
#                             compositor linked in (also builds both
#                             frameworks for i686-nxu-none first)
#   make run-i386-desktop    boot it with virtio-gpu, keyboard and mouse over
#                             PCI and -smp 4 -- under qemu-system-x86_64
#                             -accel hvf when that is actually available (see
#                             below), plain qemu-system-i386/tcg otherwise
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
    drivers/video/ui_service_login.c \
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

# Real SMP (see doc/i386/smp.md): CPUs beyond the boot one come up through
# ACPI MADT discovery and INIT-SIPI-SIPI, not a fixed assumption baked into
# the kernel, so this is just how many qemu-system-i386/x86_64 hands the
# guest -- override with e.g. `make run-i386-desktop I386_DESKTOP_SMP=1` to
# go back to a single CPU.
I386_DESKTOP_SMP ?= 4

# HVF only accelerates a guest whose architecture matches the host's, so it
# can never accelerate this port's aarch64 sibling on Apple Silicon -- but on
# any Mac (Intel or Apple Silicon) it CAN accelerate this i386 guest, since
# i386 and the host are both the x86 family. The catch: at least on this
# Homebrew QEMU build, the qemu-system-i386 *binary* was not compiled with
# HVF support at all (`qemu-system-i386 -accel help` lists only tcg) even
# though qemu-system-x86_64 has it. A 32-bit Multiboot kernel like this one
# boots identically under qemu-system-x86_64 -M pc -cpu qemu32 -- it is the
# same PC platform emulation, just a different top-level binary -- so that is
# what actually gets HVF: confirmed booting this kernel and rendering the
# desktop correctly under it, several seconds faster than under TCG. Detected
# once at parse time and only used if genuinely available, so a host without
# it (Linux, or a QEMU build that lacks HVF everywhere) falls back to the
# plain qemu-system-i386/tcg path with no user action needed.
I386_DESKTOP_HVF := $(shell qemu-system-x86_64 -accel help 2>/dev/null | grep -qx hvf && echo 1)

ifeq ($(I386_DESKTOP_HVF),1)
I386_DESKTOP_QEMU := qemu-system-x86_64 -accel hvf -cpu qemu32
else
I386_DESKTOP_QEMU := qemu-system-i386
endif

.PHONY: i386-desktop run-i386-desktop

i386-desktop:

	$(MAKE) -C $(WINDOWSERVER_DIR) BUILD=BUILD-i386 nxu-i386

	$(MAKE) -C $(UISERVICE_DIR) nxu-i386

	$(MAKE) i386 \
		BUILD_ROOT=$(I386_DESKTOP_BUILD_ROOT) \
		UISERVICE=1 \
		WINDOWSERVER=1 \
		EXTRA_CFLAGS="-DNXU_UI_SERVICE_APP_VOYAGER"


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
run-i386-desktop: i386-desktop

	$(MAKE) i386-disk BUILD_ROOT=$(I386_DESKTOP_BUILD_ROOT)

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
		-serial stdio -no-reboot \
		-append "test=desktop ui.scale=$(QEMU_UI_SCALE_PERMILLE)"
