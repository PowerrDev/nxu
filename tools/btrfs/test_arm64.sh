#!/bin/bash
#
# In-kernel Btrfs tests on arm64 (QEMU virt), behind `make test-arm64-btrfs`.
#
# The kernel boots headless with a private copy of the ext4 root disk as the
# first virtio-blk-device and the fixtures as extra virtio-blk-devices; the
# boot argument btrfs-test=<spec> (read by the guarded hook in
# kern/kern_init.c) makes it run vfs/btrfs/btrfs_selftest.c after the ext4
# mount. See tools/btrfs/test_lib.sh for the groups.
#
# usage: test_arm64.sh <kernel.bin> <root disk.img> <scratch dir> <btrfs_host>

set -u

KERNEL=${1:?usage: test_arm64.sh <kernel.bin> <root disk.img> <scratch dir> <btrfs_host>}
ROOT=${2:?}
SCRATCH=${3:?}
HOST=${4:?}
HERE=$(cd "$(dirname "$0")" && pwd)
FIX=$HERE/fixtures
QEMU_TIMEOUT=${BTRFS_TEST_TIMEOUT:-300}

for tool in zstd python3 qemu-system-aarch64 shasum; do
	command -v "$tool" > /dev/null 2>&1 || { echo "test_arm64: $tool not found"; exit 2; }
done

. "$HERE/test_lib.sh"

arch_boot() {
	local args=()
	local index path
	local reversed=""

	rm -f "$SCRATCH/root.img"
	cp "$ROOT" "$SCRATCH/root.img"

	# ARM_DEVICE_ORDER: see the note in doc/vfs/btrfs.md. QEMU virt hands out
	# virtio-mmio slots from the top of the bus down while the kernel probes
	# from the bottom up, so the device defined last is registered first: define
	# the fixtures in reverse and the root disk last.
	for path in $BT_IMAGES; do reversed="$path $reversed"; done
	index=$BT_INDEX
	for path in $reversed; do
		index=$((index - 1))
		args+=(-drive "if=none,format=raw,file=$path,id=d$index" -device "virtio-blk-device,drive=d$index")
	done
	args+=(-drive "if=none,format=raw,file=$SCRATCH/root.img,id=d0" -device virtio-blk-device,drive=d0)

	OUTPUT=$(perl "$HERE/../qemu_watchdog.pl" "$QEMU_TIMEOUT" qemu-system-aarch64 \
		-machine virt,gic-version=3 -cpu cortex-a72 -smp 1 -m 512M \
		-kernel "$KERNEL" -display none -serial stdio -monitor none \
		-global virtio-mmio.force-legacy=false \
		"${args[@]}" \
		-append "btrfs-test=$BT_SPEC" < /dev/null 2>&1)
	STATUS=$?
}

run_all_groups

printf '\n%d check(s), %d failure(s)\n' "$count" "$failures"
[ "$failures" -eq 0 ]
