#!/bin/bash
#
# Host tests of the audio code, behind `make test-audio-host`.
#
# Everything here is the kernel's own source compiled natively with
# AddressSanitizer and UndefinedBehaviorSanitizer, so a bad access or an
# overflow in the pure parts of the driver stops the run instead of passing:
#
#   sound core   feature negotiation (a device without VERSION_1 is refused),
#                the stream state machine (every state against every request),
#                names, sample widths and the parameter negotiation
#
# usage: test_host.sh <scratch dir>
# Plain bash 3.2 (what macOS ships).

set -u

SCRATCH=${1:?usage: test_host.sh <scratch dir>}
HERE=$(dirname "$0")
ROOT=$HERE/../..
CC=${AUDIO_HOST_CC:-cc}
CFLAGS="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -I$ROOT"

mkdir -p "$SCRATCH"

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

build_and_run test_sound_core "$HERE/host/test_sound_core.c" "$ROOT/drivers/virtio/virtio_sound_core.c"

echo
echo "audio host tests: $failures failure(s)"
[ "$failures" -eq 0 ]
