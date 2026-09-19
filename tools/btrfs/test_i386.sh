#!/bin/bash
#
# In-kernel Btrfs tests on i386, behind `make test-i386-btrfs`.
#
# The kernel boots with the ext4 root disk as disk0 and the fixtures as extra
# virtio-blk-pci disks; the boot argument btrfs-test=<spec> makes
# i386_init_userland() run vfs/btrfs/btrfs_selftest.c instead of launching
# userland. The host checks the guest's verdict, and that no fixture disk was
# written. See tools/btrfs/test_lib.sh for the groups.
#
# usage: test_i386.sh <kernel.elf> <root disk.img> <scratch dir> <btrfs_host>

set -u

KERNEL=${1:?usage: test_i386.sh <kernel.elf> <root disk.img> <scratch dir> <btrfs_host>}
ROOT=${2:?}
SCRATCH=${3:?}
HOST=${4:?}
HERE=$(cd "$(dirname "$0")" && pwd)
FIX=$HERE/fixtures
QEMU_TIMEOUT=${BTRFS_TEST_TIMEOUT:-240}

for tool in zstd python3 qemu-system-i386 shasum; do
	command -v "$tool" > /dev/null 2>&1 || { echo "test_i386: $tool not found"; exit 2; }
done

. "$HERE/test_lib.sh"

arch_boot() {
	local args=()
	local index=1 path

	rm -f "$SCRATCH/root.img"
	cp "$ROOT" "$SCRATCH/root.img"
	args+=(-drive "if=none,format=raw,file=$SCRATCH/root.img,id=d0" -device virtio-blk-pci,drive=d0,disable-modern=on)

	for path in $BT_IMAGES; do
		args+=(-drive "if=none,format=raw,file=$path,id=d$index" -device "virtio-blk-pci,drive=d$index,disable-modern=on")
		index=$((index + 1))
	done

	OUTPUT=$(perl "$HERE/../qemu_watchdog.pl" "$QEMU_TIMEOUT" qemu-system-i386 -M pc \
		-kernel "$KERNEL" -m 512M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		"${args[@]}" \
		-append "btrfs-test=$BT_SPEC qemu-exit=1" < /dev/null 2>&1)
	STATUS=$?
}

run_all_groups

printf '\n%d check(s), %d failure(s)\n' "$count" "$failures"
[ "$failures" -eq 0 ]
