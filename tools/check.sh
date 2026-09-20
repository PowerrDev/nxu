#!/bin/bash
#
# Behind `make check`: build and boot every headless kernel test from the
# registry in makedefs/tests.mk (through `make check-list`), then run the i386
# suites, and print one pass/fail table. Exit status is 0 only if all passed.
#
# It never touches the tracked disk.img or tools/DiskRoot. Each test gets a
# fresh disk image built from a scratch copy of tools/DiskRoot, with its own
# stage stamp: the stamp normally lives in the shared build directory, so
# without the override an up-to-date stamp would skip staging into the scratch
# root and the test would boot stale userland binaries.
#
#   make check                          every headless test, then the i386 suites
#   make check CHECK_ONLY="ipc-process default"    just those (ids from check-list)
#   make check CHECK_ONLY=i386          just the i386 suites (or name one: test-i386-vm)
#   make check CHECK_I386=0             skip the i386 suites
#
# Plain bash 3.2 (what macOS ships): no associative arrays.

set -u

cd "$(dirname "$0")/.." || exit 1

MAKE_CMD=${MAKE:-make}
ONLY=${CHECK_ONLY:-}
RUN_I386=${CHECK_I386:-1}

command -v qemu-system-aarch64 >/dev/null 2>&1 || { echo "check: qemu-system-aarch64 not found in PATH"; exit 1; }

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/nxu-check.XXXXXX") || exit 1
trap 'if [ "${NXU_CHECK_KEEP:-0}" = 0 ] && [ "$failures" -eq 0 ]; then rm -rf "$SCRATCH"; fi' EXIT

failures=0
count=0
rows=()

cp -R tools/DiskRoot "$SCRATCH/DiskRoot" || exit 1

DISK_OVERRIDES=(
	"DISK=$SCRATCH/disk.img"
	"DISK_ROOT=$SCRATCH/DiskRoot"
	"DISK_FORMAT_STAMP=$SCRATCH/format.stamp"
	"USER_STAGE_STAMP=$SCRATCH/staged.stamp"
)

selected() {
	[ -z "$ONLY" ] && return 0
	case " $ONLY " in
		*" $1 "*) return 0 ;;
	esac
	return 1
}

record() {
	count=$((count + 1))
	rows+=("$1")
	case "$1" in
		FAIL*) failures=$((failures + 1)) ;;
	esac
}

# run_kernel_test <id> <timeout> <pass string> <kernel defines>
run_kernel_test() {
	local id=$1 timeout=$2 pass=$3 cflags=$4
	local build_log="$SCRATCH/$id.build.log" serial="$SCRATCH/$id.serial.log"
	local started=$SECONDS qpid i

	rm -f "$SCRATCH/disk.img" "$SCRATCH/format.stamp" "$SCRATCH/staged.stamp"

	if ! "$MAKE_CMD" --no-print-directory "${DISK_OVERRIDES[@]}" "$SCRATCH/disk.img" >"$build_log" 2>&1 </dev/null; then
		record "FAIL  $id  disk image build failed (log: $build_log)"
		tail -15 "$build_log"
		return
	fi

	if ! "$MAKE_CMD" --no-print-directory "${DISK_OVERRIDES[@]}" BUILD_ROOT=BUILD CONFIG="$id" \
			EXTRA_CFLAGS="$cflags" all >>"$build_log" 2>&1 </dev/null; then
		record "FAIL  $id  kernel build failed (log: $build_log)"
		grep -E 'error' "$build_log" | head -10
		return
	fi

	: >"$serial"
	qemu-system-aarch64 \
		-machine virt,gic-version=3 -cpu cortex-a72 -smp 1 -m 512M \
		-kernel "BUILD/$id/kernel.bin" \
		-display none -global virtio-mmio.force-legacy=false \
		-drive if=none,format=raw,file="$SCRATCH/disk.img",id=nxudisk \
		-device virtio-blk-device,drive=nxudisk \
		-device virtio-gpu-device -device virtio-keyboard-device -device virtio-mouse-device \
		-serial file:"$serial" -monitor none -no-reboot >/dev/null 2>&1 </dev/null &
	qpid=$!

	for ((i = 0; i < timeout; i++)); do
		sleep 1
		if grep -qF -e "$pass" "$serial" 2>/dev/null; then break; fi
		if grep -qE 'FAILED|panic\(|kern_fail' "$serial" 2>/dev/null; then break; fi
		kill -0 "$qpid" 2>/dev/null || break
	done
	sleep 1
	kill "$qpid" 2>/dev/null
	wait "$qpid" 2>/dev/null

	local took=$((SECONDS - started))
	if grep -qF -e "$pass" "$serial"; then
		record "ok    $id  (${took}s)"
	else
		record "FAIL  $id  no pass line within ${timeout}s (serial log: $serial)"
		tail -25 "$serial"
	fi
}

registry=$("$MAKE_CMD" --no-print-directory -s check-list) || { echo "check: make check-list failed"; exit 1; }

while IFS=$'\t' read -r id timeout pass cflags; do
	[ -n "$id" ] || continue
	selected "$id" || continue
	echo "check: $id"
	run_kernel_test "$id" "$timeout" "$pass" "${cflags:-}"
done <<EOF
$registry
EOF

# The i386 suites: one make target each, so a failure names the suite. They keep
# their disks under BUILD/i386 and do not touch the tracked disk image.
if [ "$RUN_I386" != 0 ]; then
	i386_targets=$("$MAKE_CMD" --no-print-directory -s check-i386-list) || { echo "check: make check-i386-list failed"; exit 1; }
	for target in $i386_targets; do
		selected "$target" || selected i386 || continue
		echo "check: $target"
		started=$SECONDS
		if "$MAKE_CMD" --no-print-directory "$target" >"$SCRATCH/$target.log" 2>&1 </dev/null; then
			record "ok    $target  ($((SECONDS - started))s)"
		else
			record "FAIL  $target  (log: $SCRATCH/$target.log)"
			tail -30 "$SCRATCH/$target.log"
		fi
	done
fi

echo
echo "== make check"
for row in "${rows[@]}"; do
	echo "$row"
done
echo
echo "$count check(s), $failures failure(s)"
[ "$failures" -gt 0 ] && echo "logs kept in $SCRATCH"

[ "$count" -gt 0 ] && [ "$failures" -eq 0 ]
