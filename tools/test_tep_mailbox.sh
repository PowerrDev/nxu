#!/usr/bin/env bash
#
# tools/test_tep_mailbox.sh -- the tep-mailbox test with both machines.
#
# Boots the NXU tep-mailbox test kernel and tepOS (TrustedEnclaveProcessor
# repository, `make image`) in two QEMUs whose serial1 ports listen on Unix
# sockets, joined by that repository's tools/mailbox_link.py. Then:
#
#   1. waits for "tep_mailbox_test: protocol checks passed";
#   2. stops tepOS and waits for "tep_mailbox_test: fail-closed check passed";
#   3. restarts tepOS and waits for "tep_mailbox_test: passed".
#
# Like tools/check.sh it builds against a scratch disk, so the tracked
# disk.img and tools/DiskRoot are left alone.
#
#   TEP_DIR=<path>   the TrustedEnclaveProcessor checkout (default: next to
#                    this repository's main checkout)
#   KEEP=1           keep the scratch directory and logs

set -u

MAKE_CMD=${MAKE:-make}
ID=tep-mailbox

common_dir=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null) || { echo "test_tep_mailbox: not in a git checkout"; exit 1; }
# The directory holding the main checkout and its sibling repositories.
siblings=$(cd "$(dirname "$common_dir")/.." && pwd)
TEP_DIR=${TEP_DIR:-$siblings/TrustedEnclaveProcessor}
[ -f "$TEP_DIR/tools/mailbox_link.py" ] || { echo "test_tep_mailbox: no tepOS checkout at $TEP_DIR (set TEP_DIR)"; exit 1; }

# The disk's userland needs the sibling frameworks' headers. Relative paths,
# because the Makefile does not quote them and the siblings' absolute path
# may contain spaces; this also works from a linked worktree.
siblings_rel=$(python3 -c 'import os, sys; print(os.path.relpath(sys.argv[1]))' "$siblings")
FRAMEWORK_DIRS=(
	"UISERVICE_DIR=$siblings_rel/UIService.framework"
	"WINDOWSERVER_DIR=$siblings_rel/WindowServer.framework"
)

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/nxu-tep.XXXXXX") || exit 1
NXU_SOCK="$SCRATCH/nxu.sock"
TEP_SOCK="$SCRATCH/tepos.sock"
NXU_LOG="$SCRATCH/nxu.log"
TEP_LOG="$SCRATCH/tepos"
nxu_pid=
tep_pid=
link_pid=
result=1

cleanup() {
	for pid in "$nxu_pid" "$tep_pid" "$link_pid"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
	wait 2>/dev/null
	if [ "${KEEP:-0}" = 0 ] && [ "$result" = 0 ]; then rm -rf "$SCRATCH"; else echo "test_tep_mailbox: logs in $SCRATCH"; fi
}
trap cleanup EXIT

# wait_socket <path>: until a QEMU has created its listening socket.
wait_socket() {
	for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$1" ] && return 0; sleep 0.5; done
	echo "test_tep_mailbox: no socket $1"
	return 1
}

start_tepos() {
	rm -f "$TEP_SOCK"
	qemu-system-aarch64 -machine virt,secure=off,virtualization=off,gic-version=2 -cpu cortex-a53 -m 1024 \
		-nographic -serial file:"$TEP_LOG.$1.log" -serial unix:"$TEP_SOCK",server=on,wait=off \
		-monitor none $TEP_DEVICES -kernel "$TEP_DIR/build/boot/sel4-image.elf" >"$TEP_LOG.$1.stderr" 2>&1 </dev/null &
	tep_pid=$!
	wait_socket "$TEP_SOCK"
}

stop_tepos() {
	kill "$tep_pid" 2>/dev/null
	wait "$tep_pid" 2>/dev/null
	tep_pid=
}

# wait_for <line> <seconds>: poll the NXU serial log.
wait_for() {
	for ((i = 0; i < $2; i++)); do
		grep -qF "$1" "$NXU_LOG" 2>/dev/null && return 0
		if grep -qE 'tep_mailbox_test: failed|panic\(|kern_fail' "$NXU_LOG" 2>/dev/null; then break; fi
		kill -0 "$nxu_pid" 2>/dev/null || { echo "test_tep_mailbox: NXU's QEMU exited"; break; }
		sleep 1
	done
	echo "test_tep_mailbox: FAIL: no \"$1\" within $2 s"
	tail -30 "$NXU_LOG"
	return 1
}

echo "test_tep_mailbox: building tepOS in $TEP_DIR"
"$MAKE_CMD" -C "$TEP_DIR" image >"$SCRATCH/tepos-build.log" 2>&1 </dev/null || { tail -20 "$SCRATCH/tepos-build.log"; exit 1; }
# tepOS's own devices (virtio-rng, ...), as its Makefile defines them.
TEP_DEVICES=$("$MAKE_CMD" -s -C "$TEP_DIR" qemu-devices 2>/dev/null)

echo "test_tep_mailbox: building the $ID kernel and a scratch disk"
cp -R tools/DiskRoot "$SCRATCH/DiskRoot" || exit 1
DISK_OVERRIDES=(
	"DISK=$SCRATCH/disk.img"
	"DISK_ROOT=$SCRATCH/DiskRoot"
	"DISK_FORMAT_STAMP=$SCRATCH/format.stamp"
	"USER_STAGE_STAMP=$SCRATCH/staged.stamp"
	"${FRAMEWORK_DIRS[@]}"
)
"$MAKE_CMD" --no-print-directory "${DISK_OVERRIDES[@]}" "$SCRATCH/disk.img" >"$SCRATCH/nxu-build.log" 2>&1 </dev/null || { tail -20 "$SCRATCH/nxu-build.log"; exit 1; }
"$MAKE_CMD" --no-print-directory "${DISK_OVERRIDES[@]}" BUILD_ROOT=BUILD CONFIG="$ID" \
	EXTRA_CFLAGS="-DNXU_TEP_MAILBOX_TEST" all >>"$SCRATCH/nxu-build.log" 2>&1 </dev/null || { grep error "$SCRATCH/nxu-build.log" | head; exit 1; }

echo "test_tep_mailbox: booting NXU"
: >"$NXU_LOG"
qemu-system-aarch64 \
	-machine virt,gic-version=3 -cpu cortex-a72 -smp 1 -m 512M \
	-kernel "BUILD/$ID/kernel.bin" \
	-display none -global virtio-mmio.force-legacy=false \
	-drive if=none,format=raw,file="$SCRATCH/disk.img",id=nxudisk \
	-device virtio-blk-device,drive=nxudisk \
	-device virtio-gpu-device -device virtio-keyboard-device -device virtio-mouse-device \
	-serial file:"$NXU_LOG" \
	-chardev socket,id=tep,path="$NXU_SOCK",server=on,wait=off -serial chardev:tep \
	-monitor none -no-reboot >"$SCRATCH/nxu.stderr" 2>&1 </dev/null &
nxu_pid=$!
wait_socket "$NXU_SOCK" || exit 1

echo "test_tep_mailbox: booting tepOS"
start_tepos 1 || exit 1

python3 "$TEP_DIR/tools/mailbox_link.py" --nxu "$NXU_SOCK" --tepos "$TEP_SOCK" 2>"$SCRATCH/link.log" &
link_pid=$!

wait_for "tep_mailbox_test: protocol checks passed" 180 || exit 1
echo "test_tep_mailbox: protocol checks passed; stopping tepOS"
stop_tepos

wait_for "tep_mailbox_test: fail-closed check passed" 60 || exit 1
echo "test_tep_mailbox: fail-closed check passed; restarting tepOS"
start_tepos 2 || exit 1

wait_for "tep_mailbox_test: passed" 90 || exit 1

# A QEMU that aborted would still have let earlier phases pass: check both are clean.
if grep -v "terminating on signal" "$SCRATCH/nxu.stderr" "$TEP_LOG".*.stderr | grep -q .; then
	echo "test_tep_mailbox: FAIL: unexpected QEMU stderr"
	cat "$SCRATCH/nxu.stderr" "$TEP_LOG".*.stderr
	exit 1
fi

grep -E "tep_" "$NXU_LOG" | sed 's/^/  nxu | /'
sed 's/^/  link | /' "$SCRATCH/link.log"
echo "test_tep_mailbox: passed"
result=0
