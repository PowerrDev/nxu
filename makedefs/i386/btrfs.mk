# =============================================================================
# i386 port: read-only Btrfs driver
# =============================================================================
#
# The driver is shared with arm64 (vfs/btrfs/); this fragment only adds its
# sources to the i386 kernel and the in-kernel test target. Included by
# makedefs/i386.mk.
#
#   make test-i386-btrfs BUILD_ROOT=<scratch>
#
# boots the i386 kernel (ext4 root on disk0) with Btrfs fixtures attached as
# extra virtio-blk-pci disks and the boot argument btrfs-test=<spec>, see
# vfs/btrfs/btrfs_selftest.h and tools/btrfs/test_i386.sh.

I386_C_SOURCES += \
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
    vfs/btrfs/btrfs_vfs.c

.PHONY: test-i386-btrfs

test-i386-btrfs: $(I386_KERNEL) i386-disk $(BTRFS_HOST_TOOL)

	tools/btrfs/test_i386.sh $(I386_KERNEL) $(BUILD_ROOT)/i386/disk.img $(BUILD_ROOT)/btrfs-i386 $(BTRFS_HOST_TOOL)
