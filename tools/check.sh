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

# pass_lines_shown <serial log> <pass strings>: 0 when every one of the pass
# strings (several are separated by "|", a test that must print more than one
# line) is in the log.
pass_lines_shown() {
	local serial=$1 rest=$2 one

	while [ -n "$rest" ]; do
		one=${rest%%|*}
		case "$rest" in
			*"|"*) rest=${rest#*|} ;;
			*) rest="" ;;
		esac
		grep -qF -e "$one" "$serial" 2>/dev/null || return 1
	done

	return 0
}

# first_missing_pass_line <serial log> <pass strings>: the first that is not in the log.
first_missing_pass_line() {
	local serial=$1 rest=$2 one

	while [ -n "$rest" ]; do
		one=${rest%%|*}
		case "$rest" in
			*"|"*) rest=${rest#*|} ;;
			*) rest="" ;;
		esac
		grep -qF -e "$one" "$serial" 2>/dev/null || { echo "$one"; return; }
	done
}

# run_kernel_test <id> <timeout> <pass strings> <kernel defines> <capture: 1|-> <verify command|-> <frameworks|->
# "-" stands for an empty column (read would fold empty fields together).
run_kernel_test() {
	local id=$1 timeout=$2 pass=$3 cflags=$4 capture=$5 verify=$6 frameworks=$7
	local build_log="$SCRATCH/$id.build.log" serial="$SCRATCH/$id.serial.log"
	local started=$SECONDS qpid i
	local audio="-audiodev none,id=snd0" recording="$SCRATCH/$id.wav"

	[ "$cflags" = "-" ] && cflags=""
	[ "$frameworks" = "-" ] && frameworks=""
	[ "$capture" = "1" ] && audio="-audiodev wav,id=snd0,path=$recording"

	rm -f "$SCRATCH/disk.img" "$SCRATCH/format.stamp" "$SCRATCH/staged.stamp"

	if ! "$MAKE_CMD" --no-print-directory "${DISK_OVERRIDES[@]}" "$SCRATCH/disk.img" >"$build_log" 2>&1 </dev/null; then
		record "FAIL  $id  disk image build failed (log: $build_log)"
		tail -15 "$build_log"
		return
	fi

	if ! "$MAKE_CMD" --no-print-directory "${DISK_OVERRIDES[@]}" BUILD_ROOT=BUILD CONFIG="$id" \
			$frameworks EXTRA_CFLAGS="$cflags" all >>"$build_log" 2>&1 </dev/null; then
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
		$audio -device virtio-sound-device,audiodev=snd0 \
		-serial file:"$serial" -monitor none -no-reboot >/dev/null 2>&1 </dev/null &
	qpid=$!

	for ((i = 0; i < timeout; i++)); do
		sleep 1
		if pass_lines_shown "$serial" "$pass"; then break; fi
		if grep -qE 'FAILED|panic\(|kern_fail|: failed[[:space:]]*$' "$serial" 2>/dev/null; then break; fi
		kill -0 "$qpid" 2>/dev/null || break
	done
	sleep 1
	kill "$qpid" 2>/dev/null
	wait "$qpid" 2>/dev/null

	local took=$((SECONDS - started))
	if pass_lines_shown "$serial" "$pass" && [ "$verify" != "-" ]; then
		# The recording of a test that plays sound: what QEMU's audio backend got, checked on the host.
		local command=${verify//@CAPTURE@/$recording}

		if $command >"$SCRATCH/$id.verify.log" 2>&1; then
			tail -1 "$SCRATCH/$id.verify.log" | sed 's/^/      /'
			record "ok    $id  (${took}s, recording verified)"
		else
			cat "$SCRATCH/$id.verify.log"
			record "FAIL  $id  the recording did not verify (log: $SCRATCH/$id.verify.log)"
		fi
	elif pass_lines_shown "$serial" "$pass"; then
		record "ok    $id  (${took}s)"
	elif grep -qE 'FAILED|panic\(|kern_fail|: failed[[:space:]]*$' "$serial"; then
		record "FAIL  $id  reported a failure after ${took}s (serial log: $serial)"
		tail -25 "$serial"
	else
		record "FAIL  $id  no pass line \"$(first_missing_pass_line "$serial" "$pass")\" within ${timeout}s (serial log: $serial)"
		tail -25 "$serial"
	fi
}

registry=$("$MAKE_CMD" --no-print-directory -s check-list) || { echo "check: make check-list failed"; exit 1; }

# The host tests: pure code compiled natively under ASan and UBSan, no QEMU.
host_targets=$("$MAKE_CMD" --no-print-directory -s check-host-list) || { echo "check: make check-host-list failed"; exit 1; }

for target in $host_targets; do
	selected "$target" || selected host || continue
	echo "check: $target"
	started=$SECONDS
	if "$MAKE_CMD" --no-print-directory "$target" >"$SCRATCH/$target.log" 2>&1 </dev/null; then
		record "ok    $target  ($((SECONDS - started))s)"
	else
		record "FAIL  $target  (log: $SCRATCH/$target.log)"
		tail -30 "$SCRATCH/$target.log"
	fi
done

while IFS=$'\t' read -r id timeout pass cflags capture verify frameworks; do
	[ -n "$id" ] || continue
	selected "$id" || continue
	echo "check: $id"
	run_kernel_test "$id" "$timeout" "$pass" "$cflags" "$capture" "$verify" "$frameworks"
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
