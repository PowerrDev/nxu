# =============================================================================
# i386 port: integration glue
# =============================================================================
#
# Sources that belong to no single area: the seams between them, and (as the
# port is brought up end to end) the shared kernel sources every area assumed
# someone else would link. Included by makedefs/i386.mk.

# The threads area's weak stand-ins (mach/i386/threads_standins.c) are only
# for booting that area alone; with the VFS, loader and the rest linked here,
# every one of them has a real definition.
I386_THREADS_STANDINS := 0

I386_C_SOURCES += \
    mach/i386/kernel_init.c \
    mach/i386/mmio_map.c \
    mach/i386/userland_init.c \
    kern/boot/boot_args.c \
    kern/boot/boot_mode.c \
    drivers/video/display.c \
    kern/boot/nvram.c \
    kern/console/display_owner.c \
    kern/loader/elf.c \
    kern/tests/ipc_test.c \
    libk/crc32c.c \
    vfs/ext4.c \
    vfs/file.c \
    vfs/jbd2.c \
    vfs/path.c \
    vfs/ramfs.c \
    vfs/vfs.c \
    vfs/vfs_file.c \
    vfs/vnode.c
