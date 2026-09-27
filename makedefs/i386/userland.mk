# =============================================================================
# i386 userland: non-UI programs, staged DiskRoot and ext4 root disk image
# =============================================================================
#
#   make i386-userland     build the 32-bit user programs into
#                          $(BUILD_ROOT)/i386/user
#   make i386-disk         stage them (with the service plists) into
#                          $(BUILD_ROOT)/i386/diskroot and mkfs.ext4 the tree
#                          into $(BUILD_ROOT)/i386/disk.img
#   make i386-disk-check   e2fsck -fn the image and list CoreServices
#   make test-i386-userland  build everything, check the ELF images, boot the
#                          loader self-test
#
# Everything here is separate from the arm64 userland rules in the top-level
# Makefile and never writes into tools/DiskRoot: the arm64 DiskRoot holds
# arm64 binaries and is only read (hello.txt, README.txt) for the image
# layout. See doc/i386/userland.md.

I386_USER_BUILD := $(I386_BUILD)/user

I386_DISKROOT := $(I386_BUILD)/diskroot

I386_DISK := $(I386_BUILD)/disk.img

I386_USER_STAMP := $(I386_BUILD)/diskroot.stamp

I386_USER_CC := clang

I386_USER_CFLAGS := \
    --target=i386-none-elf \
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
    -I.

I386_USER_ASFLAGS := --target=i386-none-elf

I386_USER_LDFLAGS := \
    -m elf_i386 \
    -z max-page-size=4096 \
    --build-id=none \
    --allow-multiple-definition \
    -T makedefs/user-i386.ld \
    -nostdlib \
    -static

# Kernel tree pieces the ELF loader can be checked against without the VFS/VM.
# elf_selftest.c only needs libk and the console; the loader itself
# (kern/loader/elf.c) needs vfs, vm and proc and is added to I386_C_SOURCES by
# the integrator once those exist. Until then i386-loader-check keeps it
# compiling for this target.
I386_C_SOURCES += kern/loader/elf_selftest.c

I386_USER_COMMON_OBJECTS := \
    $(I386_USER_BUILD)/frameworks/crt0_i386.o \
    $(I386_USER_BUILD)/frameworks/lib/syscall_i386.o \
    $(I386_USER_BUILD)/frameworks/lib/string.o \
    $(I386_USER_BUILD)/frameworks/lib/thread_i386.o

# User code is freestanding and the compiler lowers 64-bit / and % on i386 to
# __udivdi3 and friends. libk's implementation goes into an archive so a
# program only carries it if it actually divides 64-bit values.
I386_USER_AR ?= llvm-ar

I386_USER_RUNTIME := $(I386_USER_BUILD)/libnxurt.a

I386_USER_COREFOUNDATION_OBJECTS := \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/lib/plist/plist.o \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/service_config.o \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o

I386_USER_BOOTSTRAP_CLIENT := \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/lib/service/bootstrap_client.o

I386_USER_BOOTD_OBJECTS := \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/job.o \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/manager.o \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/registry.o \
    $(I386_USER_BUILD)/frameworks/CoreFoundation.framework/sbin/bootd/main.o

I386_USER_DAEMON_DIR := frameworks/BootDaemons.framework

I386_USER_PROGRAMS := \
    bootd \
    logd \
    patchd \
    ipctest_a \
    ipctest_b \
    threadtest \
    sockettest_server \
    sockettest_client \
    playsound

I386_USER_BINARIES := $(addprefix $(I386_USER_BUILD)/,$(I386_USER_PROGRAMS))

I386_USER_OBJECTS := \
    $(I386_USER_COMMON_OBJECTS) \
    $(I386_USER_BUILD)/libk/udivmoddi4.o \
    $(I386_USER_BUILD)/libk/wav.o \
    $(I386_USER_COREFOUNDATION_OBJECTS) \
    $(I386_USER_BOOTD_OBJECTS) \
    $(addprefix $(I386_USER_BUILD)/$(I386_USER_DAEMON_DIR)/,$(addsuffix .o,$(filter-out bootd,$(I386_USER_PROGRAMS))))

# bootd finds its services by plist (/disk/System/Library/BootDaemons/*.plist)
# and each plist names an absolute Program path under /disk/System/Library/
# CoreServices. Those paths are architecture-neutral, so the arm64 plists work
# unchanged for the programs built here; only the plists of the programs that
# exist on i386 are staged (the WindowServer and About sevOS ones would make
# bootd retry a launch that cannot succeed).
I386_USER_PLISTS := \
    frameworks/BootDaemons.framework/Services/com.nxu.logd.plist \
    frameworks/BootDaemons.framework/Services/com.nxu.patchd.plist

# Files whose contents do not depend on the architecture, taken from the
# arm64 DiskRoot without modifying it.
I386_DISK_STATIC_FILES := \
    tools/DiskRoot/hello.txt \
    tools/DiskRoot/System/README.txt \
    tools/DiskRoot/System/Library/Resources/Audio/Boot_Audio.mp3 \
    tools/DiskRoot/System/Library/Resources/Audio/Boot_Audio.wav

I386_DISK_SIZE ?= 16M

I386_DISK_LABEL ?= NXU

# e2fsprogs is keg-only on macOS, so the tools are usually not on PATH.
I386_E2FS_PATH := /usr/local/opt/e2fsprogs/sbin:/opt/homebrew/opt/e2fsprogs/sbin

I386_MKFS_EXT4 ?= mkfs.ext4

I386_E2FSCK ?= e2fsck

I386_DEBUGFS ?= debugfs

.PHONY: i386-userland i386-disk i386-disk-check i386-loader-check test-i386-userland

-include $(I386_USER_OBJECTS:%.o=%.d)

$(I386_USER_BUILD)/%.o: %.c

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "CC" "$<"

	$(Q)$(I386_USER_CC) $(I386_USER_CFLAGS) -c $< -o $@

$(I386_USER_BUILD)/%.o: %.S

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "AS" "$<"

	$(Q)$(I386_USER_CC) $(I386_USER_ASFLAGS) -c $< -o $@

# -----------------------------------------------------------------------------
# Programs
# -----------------------------------------------------------------------------

$(I386_USER_RUNTIME): $(I386_USER_BUILD)/libk/udivmoddi4.o

	$(QUIET_PRINT) "AR" "$@"

	$(Q)rm -f $@

	$(Q)$(I386_USER_AR) rcs $@ $^

$(I386_USER_BUILD)/bootd: $(I386_USER_COMMON_OBJECTS) $(I386_USER_COREFOUNDATION_OBJECTS) $(I386_USER_BOOTD_OBJECTS) $(I386_USER_RUNTIME) makedefs/user-i386.ld

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(LD) $(I386_USER_LDFLAGS) $(filter %.o,$^) $(I386_USER_RUNTIME) -o $@

$(I386_USER_BUILD)/logd $(I386_USER_BUILD)/patchd $(I386_USER_BUILD)/threadtest $(I386_USER_BUILD)/sockettest_server $(I386_USER_BUILD)/sockettest_client: $(I386_USER_BUILD)/%: $(I386_USER_COMMON_OBJECTS) $(I386_USER_BUILD)/$(I386_USER_DAEMON_DIR)/%.o $(I386_USER_RUNTIME) makedefs/user-i386.ld

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(LD) $(I386_USER_LDFLAGS) $(filter %.o,$^) $(I386_USER_RUNTIME) -o $@

# playsound reads WAV files with the kernel's own reader, built for user space too.
$(I386_USER_BUILD)/playsound: $(I386_USER_COMMON_OBJECTS) $(I386_USER_BUILD)/$(I386_USER_DAEMON_DIR)/playsound.o $(I386_USER_BUILD)/libk/wav.o $(I386_USER_RUNTIME) makedefs/user-i386.ld

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(LD) $(I386_USER_LDFLAGS) $(filter %.o,$^) $(I386_USER_RUNTIME) -o $@

$(I386_USER_BUILD)/ipctest_a $(I386_USER_BUILD)/ipctest_b: $(I386_USER_BUILD)/%: $(I386_USER_COMMON_OBJECTS) $(I386_USER_BOOTSTRAP_CLIENT) $(I386_USER_BUILD)/$(I386_USER_DAEMON_DIR)/%.o $(I386_USER_RUNTIME) makedefs/user-i386.ld

	@mkdir -p $(dir $@)

	$(QUIET_PRINT) "LD" "$@"

	$(Q)$(LD) $(I386_USER_LDFLAGS) $(filter %.o,$^) $(I386_USER_RUNTIME) -o $@

i386-userland: $(I386_USER_BINARIES)

# -----------------------------------------------------------------------------
# DiskRoot and disk image
# -----------------------------------------------------------------------------

$(I386_USER_STAMP): $(I386_USER_BINARIES) $(I386_USER_PLISTS) $(I386_DISK_STATIC_FILES)

	$(QUIET_PRINT) "STAGE" "$(I386_DISKROOT)"

	$(Q)rm -rf $(I386_DISKROOT)

	$(Q)mkdir -p $(I386_DISKROOT)/System/Library/CoreServices $(I386_DISKROOT)/System/Library/BootDaemons $(I386_DISKROOT)/System/Recovery $(I386_DISKROOT)/System/Library/Resources/Audio $(I386_DISKROOT)/var/log $(I386_DISKROOT)/var/db/patchd

	$(Q)cp tools/DiskRoot/hello.txt $(I386_DISKROOT)/hello.txt

	$(Q)cp tools/DiskRoot/System/README.txt $(I386_DISKROOT)/System/README.txt

	$(Q)cp tools/DiskRoot/System/Library/Resources/Audio/Boot_Audio.mp3 tools/DiskRoot/System/Library/Resources/Audio/Boot_Audio.wav $(I386_DISKROOT)/System/Library/Resources/Audio/

	$(Q)cp $(I386_USER_BUILD)/bootd $(I386_DISKROOT)/System/Library/CoreServices/bootd

	$(Q)tools/sign_boot_image.sh $(I386_DISKROOT)/System/Library/CoreServices/bootd bootd $(I386_DISKROOT)/System/Library/CoreServices/bootd.manifest

	$(Q)cp $(I386_USER_BUILD)/bootd $(I386_DISKROOT)/System/Library/CoreServices/bootd.recovery

	$(Q)cp $(I386_USER_BUILD)/logd $(I386_DISKROOT)/System/Library/CoreServices/logd

	$(Q)cp $(I386_USER_BUILD)/logd $(I386_DISKROOT)/System/Library/CoreServices/logd.recovery

	$(Q)cp $(I386_USER_BUILD)/patchd $(I386_DISKROOT)/System/Library/CoreServices/patchd

	$(Q)cp $(I386_USER_BUILD)/patchd $(I386_DISKROOT)/System/Library/CoreServices/patchd.recovery

	$(Q)cp $(I386_USER_BUILD)/ipctest_a $(I386_USER_BUILD)/ipctest_b $(I386_USER_BUILD)/threadtest $(I386_USER_BUILD)/sockettest_server $(I386_USER_BUILD)/sockettest_client $(I386_USER_BUILD)/playsound $(I386_DISKROOT)/System/Library/CoreServices/

	$(Q)cp $(I386_USER_PLISTS) $(I386_DISKROOT)/System/Library/BootDaemons/

	$(I386_USER_EXTRA_STAGE)

	@touch $@

# Same mkfs.ext4 options as the arm64 `disk.img` rule in the top-level Makefile.
$(I386_DISK): $(I386_USER_STAMP)

	@PATH="$(I386_E2FS_PATH):$$PATH"; command -v $(I386_MKFS_EXT4) >/dev/null 2>&1 || { \
		echo "mkfs.ext4 is required (install e2fsprogs)"; \
		exit 1; \
	}

	$(QUIET_PRINT) "MKFS" "$@"

	$(Q)rm -f $@

	$(Q)truncate -s $(I386_DISK_SIZE) $@

	$(Q)PATH="$(I386_E2FS_PATH):$$PATH" $(I386_MKFS_EXT4) -F -q -b 4096 -I 256 \
		-O has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit \
		-E lazy_itable_init=0,lazy_journal_init=0 \
		-L $(I386_DISK_LABEL) -d $(I386_DISKROOT) $@

i386-disk: $(I386_DISK)

i386-disk-check: $(I386_DISK)

	@PATH="$(I386_E2FS_PATH):$$PATH"; command -v $(I386_E2FSCK) >/dev/null 2>&1 || { \
		echo "e2fsck is required (install e2fsprogs)"; \
		exit 1; \
	}

	PATH="$(I386_E2FS_PATH):$$PATH" $(I386_E2FSCK) -fn $(I386_DISK)

	PATH="$(I386_E2FS_PATH):$$PATH" $(I386_DEBUGFS) -R "ls -l /System/Library/CoreServices" $(I386_DISK)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------

# The loader itself cannot be linked into the i386 kernel until vfs, vm and
# proc exist for i386; compiling it here (with the kernel's -Werror flags)
# proves it stays a valid i386 translation unit in the meantime.
i386-loader-check: $(I386_BUILD)/kern/loader/elf.o

test-i386-userland: $(I386_KERNEL) i386-userland i386-loader-check i386-disk-check

	tools/check_i386_user.sh $(I386_USER_BUILD)

	tools/test_i386_userland.sh $(I386_KERNEL)
