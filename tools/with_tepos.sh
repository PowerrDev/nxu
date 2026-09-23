#!/usr/bin/env bash
#
# tools/with_tepos.sh <NXU qemu command...> -- run NXU with tepOS alongside.
#
# Builds tepOS (TrustedEnclaveProcessor repository, `make image`), boots it in
# a headless QEMU, joins its mailbox serial port to NXU's with that
# repository's tools/mailbox_link.py, then runs the given NXU QEMU command with
# a second serial port for the mailbox appended. NXU keeps the terminal;
# tepOS's console goes to BUILD/tepos-run/console.log. When NXU's QEMU exits
# (or is interrupted), tepOS and the relay are stopped.
#
# Without a tepOS checkout, or if it does not build, NXU boots alone and its
# requests to tepOS fail closed.
#
#   TEP_DIR=<path>   the TrustedEnclaveProcessor checkout (default: next to
#                    this repository's main checkout)
#
# Used by `make run` (TEST_desktop_TEP) and by `make test`/`make run-console`
# with TEP=1; see doc/drivers/tep-mailbox.md.

set -u

[ $# -gt 0 ] || { echo "usage: tools/with_tepos.sh <qemu command...>"; exit 2; }

MAKE_CMD=${MAKE:-make}

common_dir=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null) || common_dir="$PWD/.git"
TEP_DIR=${TEP_DIR:-$(cd "$(dirname "$common_dir")/.." && pwd)/TrustedEnclaveProcessor}

if [ ! -f "$TEP_DIR/tools/mailbox_link.py" ]; then
	echo "with_tepos: no tepOS checkout at $TEP_DIR (set TEP_DIR); booting NXU without tepOS"
	exec "$@"
fi

LOGS=BUILD/tepos-run
mkdir -p "$LOGS"

echo "with_tepos: building tepOS in $TEP_DIR"
if ! "$MAKE_CMD" -C "$TEP_DIR" image >"$LOGS/build.log" 2>&1 </dev/null; then
	tail -15 "$LOGS/build.log"
	echo "with_tepos: tepOS did not build (log: $LOGS/build.log); booting NXU without tepOS"
	exec "$@"
fi

# Short socket paths: Unix socket paths are limited to about 100 bytes.
SOCKS=$(mktemp -d /tmp/nxu-tep.XXXXXX) || exit 1
tep_pid=
link_pid=
nxu_pid=

cleanup() {
	for pid in "$nxu_pid" "$tep_pid" "$link_pid"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null; done
	wait 2>/dev/null
	rm -rf "$SOCKS"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

qemu-system-aarch64 -machine virt,secure=off,virtualization=off,gic-version=2 -cpu cortex-a53 -m 1024 \
	-nographic -serial file:"$LOGS/console.log" -serial unix:"$SOCKS/tepos.sock",server=on,wait=off \
	-monitor none -kernel "$TEP_DIR/build/boot/sel4-image.elf" >"$LOGS/qemu.log" 2>&1 </dev/null &
tep_pid=$!

python3 "$TEP_DIR/tools/mailbox_link.py" --nxu "$SOCKS/nxu.sock" --tepos "$SOCKS/tepos.sock" 2>"$LOGS/link.log" &
link_pid=$!

echo "with_tepos: tepOS running; its console: $LOGS/console.log"

# Not exec, and not in the foreground: bash runs a trap only once a foreground
# child has exited, so a signal to this script would otherwise leave every
# machine running. NXU's QEMU keeps the terminal as its stdin (0<&0: without
# job control bash would give a background command /dev/null).
"$@" -chardev socket,id=tep,path="$SOCKS/nxu.sock",server=on,wait=off -serial chardev:tep 0<&0 &
nxu_pid=$!
wait "$nxu_pid"
status=$?
nxu_pid=
exit "$status"
