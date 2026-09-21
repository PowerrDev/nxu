#!/bin/bash
#
# Boot the i386 kernel on a Btrfs disk and print what is on it: behind
# `make run-i386-btrfs` (see makedefs/i386/btrfs.mk for the options).
#
# The image is attached with snapshot=on, so the kernel (which never writes to
# it anyway) cannot change your file either way. Plain bash 3.2.

set -u

KERNEL=${1:?usage: run_i386.sh <kernel.elf>   (options are environment variables, see makedefs/i386/btrfs.mk)}
HERE=$(dirname "$0")
TIMEOUT=${BTRFS_TIMEOUT:-120}

if [ -n "${BTRFS_IMAGE:-}" ] && [ -n "${BTRFS_FIXTURE:-}" ]; then
	echo "run_i386: give BTRFS_IMAGE or BTRFS_FIXTURE, not both" >&2
	exit 2
fi

if [ -z "${BTRFS_IMAGE:-}" ] && [ -z "${BTRFS_FIXTURE:-}" ]; then
	cat >&2 <<USAGE
usage: make run-i386-btrfs BTRFS_FIXTURE=<name>       one of $(ls "$HERE/fixtures" | sed -n 's/\.img\.zst$//p' | tr '\n' ' ')
       make run-i386-btrfs BTRFS_IMAGE=<file>         any Btrfs disk image
options: BTRFS_CAT=/path  BTRFS_LS=0  BTRFS_SUBVOL=<id>  BTRFS_NOVERIFY=1  BTRFS_MAX=<n>  BTRFS_TIMEOUT=<s>
USAGE
	exit 2
fi

for tool in qemu-system-i386 perl; do
	command -v "$tool" > /dev/null 2>&1 || { echo "run_i386: $tool not found" >&2; exit 2; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/btrfsrun.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

if [ -n "${BTRFS_FIXTURE:-}" ]; then
	FIXTURE_FILE="$HERE/fixtures/$BTRFS_FIXTURE.img.zst"

	[ -f "$FIXTURE_FILE" ] || { echo "run_i386: no fixture named '$BTRFS_FIXTURE' in $HERE/fixtures" >&2; exit 2; }
	command -v zstd > /dev/null 2>&1 || { echo "run_i386: zstd not found (needed for fixtures)" >&2; exit 2; }

	IMAGE=$WORK/disk.img
	zstd -dq "$FIXTURE_FILE" -o "$IMAGE" || exit 2
else
	IMAGE=$BTRFS_IMAGE
	[ -f "$IMAGE" ] || { echo "run_i386: no such image: $IMAGE" >&2; exit 2; }
fi

APPEND="qemu-exit=1"

# The tree listing is the default; BTRFS_LS=0 turns it off (e.g. to only cat a file).
if [ "${BTRFS_LS:-1}" != 0 ]; then APPEND="btrfs-ls=1 $APPEND"; fi
if [ -n "${BTRFS_CAT:-}" ]; then APPEND="btrfs-cat=$BTRFS_CAT $APPEND"; fi
if [ -n "${BTRFS_SUBVOL:-}" ]; then APPEND="btrfs-subvol=$BTRFS_SUBVOL $APPEND"; fi
if [ -n "${BTRFS_NOVERIFY:-}" ] && [ "${BTRFS_NOVERIFY:-0}" != 0 ]; then APPEND="btrfs-noverify=1 $APPEND"; fi
if [ -n "${BTRFS_MAX:-}" ]; then APPEND="btrfs-max=$BTRFS_MAX $APPEND"; fi

case "$APPEND" in
	*btrfs-ls=1*|*btrfs-cat=*) ;;
	*) echo "run_i386: nothing to do (BTRFS_LS=0 and no BTRFS_CAT)" >&2; exit 2 ;;
esac

echo "run_i386: booting the i386 kernel on $(basename "$IMAGE"): -append \"$APPEND\""

perl "$HERE/../qemu_watchdog.pl" "$TIMEOUT" qemu-system-i386 -M pc \
	-kernel "$KERNEL" -m 512M \
	-display none -serial stdio -monitor none -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
	-drive "if=none,format=raw,file=$IMAGE,snapshot=on,id=d0" \
	-device virtio-blk-pci,drive=d0,disable-modern=on \
	-append "$APPEND" < /dev/null
status=$?

# isa-debug-exit reports (code << 1) | 1: 1 is a clean run, 3 a failed phase.
case $status in
	1) exit 0 ;;
	3) echo "run_i386: the kernel reported a failure (see above)" >&2; exit 1 ;;
	124) echo "run_i386: timed out after ${TIMEOUT}s" >&2; exit 1 ;;
	*) exit $status ;;
esac
