#!/bin/bash
#
# Build the Btrfs test fixtures with the REAL Linux Btrfs stack.
#
# This Mac has no mkfs.btrfs and no container runtime, so the fixtures are made
# inside an official Alpine Linux guest under QEMU:
#
#   1. download the official Alpine x86_64 netboot kernel, initramfs and modloop
#      (dl-cdn.alpinelinux.org) into a cache directory OUTSIDE the repository,
#   2. boot them headless with QEMU user networking; the initramfs pulls a tiny
#      apkovl overlay (tools/btrfs/guest/*) from a throw-away HTTP server on the
#      host, which runs the guest script at boot,
#   3. the guest installs btrfs-progs (and python3, zstd, ...) with apk, builds
#      and populates every image with the real mkfs.btrfs and the real Linux
#      Btrfs driver, writes a MANIFEST for each from the mounted filesystem,
#      zstd-compresses the images and streams everything to the host as a tar
#      archive on a second virtio disk,
#   4. the host unpacks the archive into tools/btrfs/fixtures/.
#
# Usage: tools/btrfs/make_fixtures.sh [--smoke] [--only NAME]...
#
#   --smoke        run tools/btrfs/guest/smoke.sh instead (print versions and
#                  mkfs limits; nothing is written to the fixtures directory)
#   --only NAME    build only the named fixture group (repeatable)
#
# Environment:
#   NXU_BTRFS_LAB  cache directory (default $HOME/.cache/nxu-btrfs-lab)
#   NXU_LAB_TIMEOUT seconds before the guest is killed (default 1500)
#   NXU_LAB_ACCEL  qemu accelerator options (default "-accel hvf -accel tcg")
#
# Bash 3.2 compatible. Uses only the host's curl, tar, python3 and qemu.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

ALPINE_BRANCH=v3.24
ALPINE_RELEASE=3.24.2
ALPINE_KERNEL=6.18.52-0-virt
NETBOOT=https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/releases/x86_64/netboot

LAB=${NXU_BTRFS_LAB:-$HOME/.cache/nxu-btrfs-lab}
TIMEOUT=${NXU_LAB_TIMEOUT:-1500}
ACCEL=${NXU_LAB_ACCEL:-"-accel hvf -accel tcg"}
GUEST_SCRIPT=build_fixtures.sh
ONLY=

while [ $# -gt 0 ]; do
	case "$1" in
	--smoke) GUEST_SCRIPT=smoke.sh ;;
	--only) shift; ONLY="$ONLY $1" ;;
	*) echo "make_fixtures: unknown argument $1" >&2; exit 2 ;;
	esac
	shift
done

for tool in curl tar python3 qemu-system-x86_64 perl; do
	command -v "$tool" > /dev/null 2>&1 || { echo "make_fixtures: $tool not found" >&2; exit 2; }
done

mkdir -p "$LAB"

# ---- 1. download (cached, verified against the checksums recorded here) -----

fetch() {
	local name=$1 sha=$2

	if [ ! -f "$LAB/$name" ]; then
		echo "make_fixtures: downloading $name"
		curl -fL --retry 3 -o "$LAB/$name.part" "$NETBOOT/$name"
		mv "$LAB/$name.part" "$LAB/$name"
	fi

	local got
	got=$(shasum -a 256 "$LAB/$name" | cut -d' ' -f1)

	if [ "$got" != "$sha" ]; then
		echo "make_fixtures: $name has sha256 $got, expected $sha" >&2
		echo "make_fixtures: Alpine republished it? update the pins in this script" >&2
		exit 1
	fi
}

fetch vmlinuz-virt 40f620bc8c93d952e57dd8dfc0f94fca1759d192a4fc4a260705d50ca378559c
fetch initramfs-virt c990c63e4602aa84b92d7df54fd180cb0e56590d61b71221ba6d60e913d26357
fetch modloop-virt 1e7a3eea707d2ecb3d0502f97fb4dc4dcbac1fe06075e24ae19bfdf4343bb1cf

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nxu-btrfs-lab.XXXXXX")
HTTP_PID=
cleanup() {
	[ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2> /dev/null || true
	rm -rf "$WORK"
}
trap cleanup EXIT

# ---- 2. the apkovl overlay ------------------------------------------------

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')

OVL=$WORK/ovl
mkdir -p "$OVL/etc/local.d" "$OVL/etc/runlevels/default" "$OVL/etc/apk" "$OVL/root/guest" "$WORK/www"

ln -s /etc/init.d/local "$OVL/etc/runlevels/default/local"
cp "$HERE"/guest/* "$OVL/root/guest/"
chmod 755 "$OVL"/root/guest/*.sh

cat > "$OVL/etc/local.d/nxu-lab.start" <<EOF
#!/bin/sh
# Runs once the network is up. All output goes to the serial console.
exec > /dev/console 2>&1
echo "nxu-lab: guest script starting ($GUEST_SCRIPT)"
NXU_HTTP="http://10.0.2.2:$PORT" NXU_ONLY="$ONLY" sh /root/guest/$GUEST_SCRIPT
echo "nxu-lab: guest script exit status \$?"
poweroff
EOF
chmod 755 "$OVL/etc/local.d/nxu-lab.start"

cat > "$OVL/etc/apk/repositories" <<EOF
http://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/main
http://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/community
EOF

# Without a uid/gid override bsdtar would store the invoking user's ids.
tar --uid 0 --gid 0 --uname root --gname root -C "$OVL" -czf "$WORK/www/nxu.apkovl.tar.gz" .
cp "$LAB/modloop-virt" "$WORK/www/modloop-virt"

# ---- 3. boot the guest ----------------------------------------------------

(cd "$WORK/www" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 > "$WORK/http.log" 2>&1) &
HTTP_PID=$!
sleep 1

# The output disk: the guest writes a tar stream straight onto it.
OUT=$WORK/out.raw
: > "$OUT"
python3 -c "import os; f=open('$OUT','wb'); f.truncate(96*1024*1024); f.close()"

APPEND="console=ttyS0 ip=dhcp alpine_repo=http://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/main modloop=http://10.0.2.2:$PORT/modloop-virt apkovl=http://10.0.2.2:$PORT/nxu.apkovl.tar.gz"

echo "make_fixtures: booting Alpine $ALPINE_RELEASE ($ALPINE_KERNEL) headless, guest script $GUEST_SCRIPT"

set +e
perl "$REPO/tools/qemu_watchdog.pl" "$TIMEOUT" qemu-system-x86_64 \
	-machine q35 $ACCEL -cpu max -smp 2 -m 1536 \
	-kernel "$LAB/vmlinuz-virt" -initrd "$LAB/initramfs-virt" -append "$APPEND" \
	-display none -serial stdio -monitor none -no-reboot \
	-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	-drive "if=none,format=raw,file=$OUT,id=out" -device virtio-blk-pci,drive=out \
	< /dev/null 2>&1 | tee "$WORK/console.log" | sed -e 's/\r$//' | grep -a -E '^(nxu-lab|guest:|ERROR|error|mkfs|btrfs|Kernel panic)'
QSTATUS=${PIPESTATUS[0]}
set -e

cp "$WORK/console.log" "$LAB/last-console.log"

if [ "$QSTATUS" -ne 0 ]; then
	echo "make_fixtures: qemu exited with status $QSTATUS (console in $LAB/last-console.log)" >&2
	exit 1
fi

if ! grep -aq "nxu-lab: guest script exit status 0" "$WORK/console.log"; then
	echo "make_fixtures: the guest script failed (console in $LAB/last-console.log)" >&2
	exit 1
fi

if [ "$GUEST_SCRIPT" = smoke.sh ]; then
	echo "make_fixtures: smoke run finished"
	exit 0
fi

# ---- 4. unpack ------------------------------------------------------------

mkdir -p "$WORK/unpack"
tar -xf "$OUT" -C "$WORK/unpack"

DEST=$HERE/fixtures
mkdir -p "$DEST"
cp "$WORK"/unpack/* "$DEST"/
echo "make_fixtures: wrote $(ls "$WORK/unpack" | wc -l | tr -d ' ') files to $DEST"
