#!/bin/bash
#
# Host tests of the VFS pure code, behind `make test-vfs-host`.
#
# Path resolution (vfs/pathname.c) compiled natively with the sanitizers:
# relative and absolute paths, ".", "..", slashes, the root, overflow.
#
# usage: test_host.sh <scratch dir>
# Plain bash 3.2 (what macOS ships).

set -u

SCRATCH=${1:?usage: test_host.sh <scratch dir>}
HERE=$(dirname "$0")
ROOT=$HERE/../..
CC=${VFS_HOST_CC:-cc}
CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -I$ROOT"

mkdir -p "$SCRATCH"

# Some hosts ship an AddressSanitizer runtime that cannot start a process at
# all (seen on macOS 26 with Apple clang 17, x86_64: every ASan binary dies in
# __pthread_init before main). Probe once; without a working one, say so and
# keep UndefinedBehaviorSanitizer and bounds checking rather than report a
# failure that is not the code's.
printf 'int main(void) { return 0; }\n' >"$SCRATCH/asan_probe.c"
if ! $CC -fsanitize=address "$SCRATCH/asan_probe.c" -o "$SCRATCH/asan_probe" 2>/dev/null || ! ("$SCRATCH/asan_probe") 2>/dev/null; then
	echo "note  AddressSanitizer does not run on this host; using UBSan and bounds checks only"
	CFLAGS="-std=gnu11 -O1 -g -fsanitize=undefined,bounds -fno-sanitize-recover=all -fstack-protector-all -Wall -Wextra -Werror -I$ROOT"
fi

failures=0

# build_and_run <name> <sources...>
build_and_run() {
	local name=$1
	shift

	if ! $CC $CFLAGS "$@" -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"; then
		echo "FAIL  $name: build failed"
		sed 's/^/      | /' "$SCRATCH/$name.build.log"
		failures=$((failures + 1))
		return
	fi

	if "$SCRATCH/$name"; then
		echo "ok    $name"
	else
		echo "FAIL  $name"
		failures=$((failures + 1))
	fi
}

build_and_run test_pathname "$HERE/host/test_pathname.c" "$ROOT/vfs/pathname.c"

echo
echo "vfs host tests: $failures failure(s)"
[ "$failures" -eq 0 ]
