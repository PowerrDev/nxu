#!/usr/bin/env bash
#
# tools/with_tepos.sh <NXU qemu command...> -- run NXU with tepOS alongside.
#
# Builds tepOS (TrustedEnclaveProcessor repository, `make image`), boots it in
# a headless QEMU, joins its mailbox serial port to NXU's with that
# repository's tools/mailbox_link.py, then runs the given NXU QEMU command with
# a second serial port for the mailbox appended (COM2 on i386). When NXU's
# QEMU exits (or this is interrupted), tepOS and the relay are stopped.
#
# From a terminal, tools/console_switch.py shows the consoles: press s for
# NXU's, t for tepOS's, Ctrl-C to quit. Both are also logged in full to
# BUILD/tepos-run/nxu-console.log and console.log. Otherwise (a script, a
# pipe, TEP_CONSOLE=stdio) NXU keeps stdio and tepOS's console only goes to
# BUILD/tepos-run/console.log.
#
# Without a tepOS checkout, or if it does not build, NXU boots alone and its
# requests to tepOS fail closed.
#
#   TEP_DIR=<path>   the TrustedEnclaveProcessor checkout (default: next to
#                    this repository's main checkout)
#
# Used by `make run` (TEST_desktop_TEP), `make run-i386`, and by
# `make test`/`make run-console` with TEP=1; see doc/drivers/tep-mailbox.md.

set -u

[ $# -gt 0 ] || { echo "usage: tools/with_tepos.sh <qemu command...>"; exit 2; }

MAKE_CMD=${MAKE:-make}

common_dir=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null) || common_dir="$PWD/.git"
TEP_DIR=${TEP_DIR:-$(cd "$(dirname "$common_dir")/.." && pwd)/TrustedEnclaveProcessor}

if [ ! -f "$TEP_DIR/tools/mailbox_link.py" ]; then
	echo "with_tepos: no tepOS checkout at $TEP_DIR (set TEP_DIR); booting NXU without tepOS"
	exec "$@"
fi

# Absolute: tepOS's QEMU runs in $TEP_DIR, where its device paths are relative.
LOGS="$PWD/BUILD/tepos-run"
mkdir -p "$LOGS"

echo "with_tepos: building tepOS in $TEP_DIR"
if ! "$MAKE_CMD" -C "$TEP_DIR" image >"$LOGS/build.log" 2>&1 </dev/null; then
	tail -15 "$LOGS/build.log"
	echo "with_tepos: tepOS did not build (log: $LOGS/build.log); booting NXU without tepOS"
	exec "$@"
fi

# tepOS's own devices (virtio-rng, its key store disk and sealing key, ...), as
# its Makefile defines them, with paths relative to $TEP_DIR. The key store is
# tepOS's real, persistent one (build/tepos-keystore.img there).
TEP_DEVICES=$("$MAKE_CMD" -s -C "$TEP_DIR" qemu-devices 2>/dev/null)

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

# The console switcher needs a terminal and a "-serial stdio" to take over.
SWITCH=0
NXU_ARGS=()
if [ -t 0 ] && [ -t 1 ] && [ "${TEP_CONSOLE:-switch}" != stdio ]; then
	while [ $# -gt 0 ]; do
		if [ "$1" = "-serial" ] && [ $# -gt 1 ] && [ "$2" = "stdio" ]; then
			NXU_ARGS+=(-chardev "socket,id=con,path=$SOCKS/nxu-console.sock,server=on,wait=on" -serial chardev:con)
			SWITCH=1
			shift 2
			continue
		fi
		NXU_ARGS+=("$1")
		shift
	done
else
	NXU_ARGS=("$@")
fi

# With the switcher both consoles wait for it to connect, so nothing printed at boot is lost.
if [ "$SWITCH" = 1 ]; then
	TEP_CONSOLE_ARG="unix:$SOCKS/tepos-console.sock,server=on,wait=on"
else
	TEP_CONSOLE_ARG="file:$LOGS/console.log"
fi

(cd "$TEP_DIR" && exec qemu-system-aarch64 -machine virt,secure=off,virtualization=off,gic-version=2 -cpu cortex-a53 -m 1024 \
	-nographic -serial "$TEP_CONSOLE_ARG" -serial unix:"$SOCKS/tepos.sock",server=on,wait=off \
	-monitor none $TEP_DEVICES -kernel build/boot/sel4-image.elf) >"$LOGS/qemu.log" 2>&1 </dev/null &
tep_pid=$!

python3 "$TEP_DIR/tools/mailbox_link.py" --nxu "$SOCKS/nxu.sock" --tepos "$SOCKS/tepos.sock" 2>"$LOGS/link.log" &
link_pid=$!

MAILBOX_ARGS=(-chardev "socket,id=tep,path=$SOCKS/nxu.sock,server=on,wait=off" -serial chardev:tep)

if [ "$SWITCH" = 1 ]; then
	"${NXU_ARGS[@]}" "${MAILBOX_ARGS[@]}" </dev/null >"$LOGS/nxu-qemu.log" 2>&1 &
	nxu_pid=$!
	# In the foreground: it owns the terminal. When it returns, NXU is gone or the user quit.
	python3 tools/console_switch.py --nxu "$SOCKS/nxu-console.sock" --tepos "$SOCKS/tepos-console.sock" \
		--nxu-log "$LOGS/nxu-console.log" --tepos-log "$LOGS/console.log"
	exit $?
fi

echo "with_tepos: tepOS running; its console: $LOGS/console.log"

# Not exec, and not in the foreground: bash runs a trap only once a foreground
# child has exited, so a signal to this script would otherwise leave every
# machine running. NXU's QEMU keeps the terminal as its stdin (0<&0: without
# job control bash would give a background command /dev/null).
"${NXU_ARGS[@]}" "${MAILBOX_ARGS[@]}" 0<&0 &
nxu_pid=$!
wait "$nxu_pid"
status=$?
nxu_pid=
exit "$status"
