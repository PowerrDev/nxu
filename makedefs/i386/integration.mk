# =============================================================================
# i386 port: integration glue
# =============================================================================
#
# Sources that belong to no single area: the seams between them, and (as the
# port is brought up end to end) the shared kernel sources every area assumed
# someone else would link. Included by makedefs/i386.mk.

# The threads area's weak stand-ins (kern/i386/threads_standins.c) are only
# for booting that area alone; with the VFS, loader and the rest linked here,
# every one of them has a real definition.
I386_THREADS_STANDINS := 0

I386_C_SOURCES += \
    kern/i386/fs_test.c \
    kern/i386/kernel_init.c \
    kern/i386/mmio_map.c \
    kern/i386/userland_init.c \
    kern/boot/boot_args.c \
    kern/boot/boot_mode.c \
    drivers/video/display.c \
    kern/boot/nvram.c \
    kern/console/display_owner.c \
    kern/loader/elf.c \
    kern/tests/ipc_process_test.c \
    kern/tests/ipc_test.c \
    kern/tests/socket_process_test.c \
    kern/tests/thread_process_test.c \
    libk/crc32c.c \
    vfs/ext4.c \
    vfs/file.c \
    vfs/jbd2.c \
    vfs/path.c \
    vfs/ramfs.c \
    vfs/vfs.c \
    vfs/vfs_file.c \
    vfs/vnode.c

# End-to-end boots of the whole kernel with userland from the ext4 image:
# bootd as PID 1, and the cross-process IPC, thread and socket tests.
.PHONY: test-i386-boot test-i386-fs

test-i386-boot: $(I386_KERNEL) i386-disk

	tools/test_i386_boot.sh $(I386_KERNEL) $(BUILD_ROOT)/i386/disk.img


# ext4 write and journal tests: the guest writes and crashes, the host's e2fsck
# and debugfs check the image (needs e2fsprogs).
test-i386-fs: $(I386_KERNEL) i386-disk

	tools/test_i386_fs.sh $(I386_KERNEL) $(BUILD_ROOT)/i386/disk.img
