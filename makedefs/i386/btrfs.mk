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
    vfs/btrfs/btrfs_list.c \
    vfs/btrfs/btrfs_lzo.c \
    vfs/btrfs/btrfs_root.c \
    vfs/btrfs/btrfs_selftest.c \
    vfs/btrfs/btrfs_super.c \
    vfs/btrfs/btrfs_tree.c \
    vfs/btrfs/btrfs_vfs.c \
    vfs/btrfs/btrfs_zlib.c

.PHONY: test-i386-btrfs

test-i386-btrfs: $(I386_KERNEL) i386-disk $(BTRFS_HOST_TOOL)

	tools/btrfs/test_i386.sh $(I386_KERNEL) $(BUILD_ROOT)/i386/disk.img $(BUILD_ROOT)/btrfs-i386 $(BTRFS_HOST_TOOL)


# Boot the i386 kernel on a Btrfs disk and look at it, the way run-i386 boots a
# bare kernel. The Btrfs image is the only disk; the kernel mounts it read-only,
# prints the tree (and/or one file) to the serial console and exits.
#
#   make run-i386-btrfs BTRFS_FIXTURE=tree                    a committed fixture (tools/btrfs/fixtures/<name>.img.zst)
#   make run-i386-btrfs BTRFS_IMAGE=/path/to/disk.img         any Btrfs image (never modified)
#
# Optional: BTRFS_CAT=/path/in/fs   print that file      BTRFS_LS=0  skip the tree listing
#           BTRFS_SUBVOL=<id>       mount that subvolume  BTRFS_NOVERIFY=1  skip data checksums
#           BTRFS_MAX=<n>           entries to print (default 512)   BTRFS_TIMEOUT=<seconds> (default 120)
.PHONY: run-i386-btrfs

run-i386-btrfs: $(I386_KERNEL)

	BTRFS_IMAGE="$(BTRFS_IMAGE)" BTRFS_FIXTURE="$(BTRFS_FIXTURE)" BTRFS_CAT="$(BTRFS_CAT)" BTRFS_LS="$(BTRFS_LS)" \
		BTRFS_SUBVOL="$(BTRFS_SUBVOL)" BTRFS_NOVERIFY="$(BTRFS_NOVERIFY)" BTRFS_MAX="$(BTRFS_MAX)" BTRFS_TIMEOUT="$(BTRFS_TIMEOUT)" \
		tools/btrfs/run_i386.sh $(I386_KERNEL)
