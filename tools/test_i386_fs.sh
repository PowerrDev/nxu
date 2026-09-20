#!/bin/bash
#
# ext4 write and journal tests for the i386 kernel, behind `make test-i386-fs`.
#
# The guest does the work (fs-test=<mode>, see kern/i386/fs_test.h) and the
# HOST checks the disk image independently with e2fsck and debugfs, so the
# kernel is never only grading itself. One disk image is carried through the
# cases in order, because the later ones depend on what the earlier left:
#
#   fixture          the image is a clean ext4 filesystem to start with
#   write            create/grow/truncate/sparse-extend/unlink; host fsck is
#                    clean and write-test.bin matches the expected bytes
#   write-again      a second boot finds the persistence marker; still clean
#   journal-crash    a transaction is committed, power is cut before the
#                    checkpoint: the image must be flagged needs_recovery and
#                    the write must NOT be in the home blocks yet
#   journal-verify   the next mount replays the journal: the write appears,
#                    needs_recovery is cleared, host fsck is clean
#   bootd-after      the userland services boot on the used image and leave it
#                    consistent
#
# Needs debugfs, dumpe2fs and e2fsck (e2fsprogs). Plain bash 3.2.

set -u

KERNEL=${1:?usage: test_i386_fs.sh <kernel.elf> <disk.img>}
DISK=${2:?usage: test_i386_fs.sh <kernel.elf> <disk.img>}
TIMEOUT=${I386_TEST_TIMEOUT:-60}
HERE=$(dirname "$0")

# e2fsprogs' output is localised; the checks below match English.
export LC_ALL=C
export PATH="/usr/local/opt/e2fsprogs/sbin:/opt/homebrew/opt/e2fsprogs/sbin:$PATH"

for tool in debugfs dumpe2fs e2fsck; do
	command -v "$tool" > /dev/null 2>&1 || { echo "test_i386_fs: $tool not found (install e2fsprogs)"; exit 2; }
done

STATUS_CLEAN=1

WORK=$(mktemp -d "${TMPDIR:-/tmp}/i386fs.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

IMG=$WORK/fs.img
cp "$DISK" "$IMG"

failures=0
count=0
OUTPUT=
STATUS=0

# boot <boot args>: boot the kernel on $IMG; sets $OUTPUT and $STATUS.
boot() {
	OUTPUT=$(perl "$HERE/qemu_watchdog.pl" "$TIMEOUT" qemu-system-i386 -M pc \
		-kernel "$KERNEL" -m 512M \
		-display none -serial stdio -monitor none -no-reboot \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-drive "if=none,format=raw,file=$IMG,id=d0" \
		-device virtio-blk-pci,drive=d0,disable-modern=on \
		-append "$1" < /dev/null 2>&1)
	STATUS=$?
}

# Result of the current case, reported by finish_case.
case_ok=1
case_name=

begin_case() {
	case_name=$1
	case_ok=1
	count=$((count + 1))
}

expect() {
	local description=$1
	shift

	if ! "$@"; then
		case_ok=0
		printf 'FAIL  %-16s %s\n' "$case_name" "$description"
	fi
}

finish_case() {
	if [ "$case_ok" -eq 1 ]; then
		printf 'ok    %-16s\n' "$case_name"
	else
		failures=$((failures + 1))
		printf '%s\n' "$OUTPUT" | tail -25 | sed 's/^/      | /'
	fi
}

out_has() { printf '%s\n' "$OUTPUT" | grep -qF -- "$1"; }
status_is() { [ "$STATUS" -eq "$1" ]; }
fsck_clean() { e2fsck -fn "$IMG" > "$WORK/fsck.log" 2>&1 || { sed 's/^/      fsck | /' "$WORK/fsck.log" | tail -15; return 1; }; }
needs_recovery() { dumpe2fs -h "$IMG" 2>/dev/null | grep -q '^Filesystem features:.*needs_recovery'; }
no_recovery_needed() { ! needs_recovery; }

# The bytes write-test.bin must hold: 5000 of a chunk^index pattern, zeros up to
# 8192, a tail string, then zeros to 9000 (the extend-to-9000 truncate wins over
# the shorter write past the end).
expected_write_test() {
	perl -e 'for $c (0..11){ print pack("C*", map { ($c ^ $_) & 255 } 0..1023) }' | head -c 5000
	head -c 3192 /dev/zero
	printf 'tail-after-sparse-growth'
	head -c 784 /dev/zero
}

write_test_matches() {
	rm -f "$WORK/got.bin"
	debugfs -R "dump /NXU/write-test.bin $WORK/got.bin" "$IMG" > /dev/null 2>&1
	expected_write_test > "$WORK/want.bin"
	cmp -s "$WORK/got.bin" "$WORK/want.bin"
}

file_absent() { debugfs -R "stat $1" "$IMG" 2>&1 | grep -q 'File not found'; }
file_is() { [ "$(debugfs -R "cat $1" "$IMG" 2>/dev/null)" = "$2" ]; }
file_bytes() { [ "$(debugfs -R "cat $1" "$IMG" 2>/dev/null | wc -c | tr -d ' ')" -eq "$2" ]; }

JOURNAL_MESSAGE='NXU JBD2 committed metadata survived the crash.'

# ---- fixture ---------------------------------------------------------------
begin_case fixture
expect "fresh image is a clean ext4 filesystem" fsck_clean
expect "fresh image does not need recovery" no_recovery_needed
finish_case

# ---- write -----------------------------------------------------------------
begin_case write
boot "fs-test=write qemu-exit=1"
expect "qemu exit status" status_is $STATUS_CLEAN
expect "guest reports success" out_has "i386_fs_test: write test passed"
expect "persistence marker created" out_has "i386_fs_test: persistence marker created"
expect "host fsck is clean after the guest's writes" fsck_clean
expect "write-test.bin is byte-identical to the expected image" write_test_matches
expect "unlinked file is gone from the host's view" file_absent /NXU/delete-me.txt
expect "persistence marker content" file_is /NXU/persistent.txt "NXU persistent ext4 marker"
finish_case

# ---- write-again -----------------------------------------------------------
begin_case write-again
boot "fs-test=write qemu-exit=1"
expect "qemu exit status" status_is $STATUS_CLEAN
expect "marker recovered from the previous boot" out_has "i386_fs_test: persistence marker recovered from previous boot"
expect "host fsck is clean" fsck_clean
expect "write-test.bin unchanged by the second run" write_test_matches
finish_case

# ---- journal-crash ---------------------------------------------------------
begin_case journal-crash
boot "fs-test=journal-crash qemu-exit=1"
expect "qemu exit status" status_is $STATUS_CLEAN
expect "transaction committed, checkpoint skipped" out_has "journal transaction committed, checkpoint intentionally skipped"
expect "image is flagged needs_recovery after the power loss" needs_recovery
expect "the write is only in the journal, not in its home blocks" file_bytes /NXU/journal-recovery.txt 0
finish_case

# ---- journal-verify --------------------------------------------------------
begin_case journal-verify
boot "fs-test=journal-verify qemu-exit=1"
expect "qemu exit status" status_is $STATUS_CLEAN
expect "guest saw the recovered transaction" out_has "i386_fs_test: committed crash-test transaction recovered"
expect "needs_recovery cleared by the replay" no_recovery_needed
expect "host fsck is clean after recovery" fsck_clean
expect "host reads the recovered content" file_is /NXU/journal-recovery.txt "$JOURNAL_MESSAGE"
finish_case

# ---- bootd-after -----------------------------------------------------------
begin_case bootd-after
boot "run-seconds=8 qemu-exit=1"
expect "qemu exit status" status_is $STATUS_CLEAN
expect "bootd started as PID 1" out_has "bootd: userspace service manager started as PID 1"
expect "logd and patchd started" out_has "bootd: com.nxu.patchd: started"
expect "host fsck is clean after the services ran" fsck_clean
finish_case

printf '\n%d case(s), %d failure(s)\n' "$count" "$failures"

[ "$failures" -eq 0 ]
