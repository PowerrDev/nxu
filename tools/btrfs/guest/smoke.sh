#!/bin/sh
# Guest-side smoke test: install the tools and print what the lab has.
set -u
. /root/guest/lib.sh
setup_modules || echo "guest: modloop setup failed"
echo "guest: uname $(uname -r)"
cat /etc/alpine-release
apk add --no-progress btrfs-progs python3 zstd bash util-linux e2fsprogs 2>&1 | tail -3
echo "guest: $(mkfs.btrfs --version)"
echo "guest: $(python3 --version)"
modprobe loop && echo "guest: loop ok"
modprobe btrfs && echo "guest: btrfs ok"
ls -la /dev/vd* 2>&1 | sed 's/^/guest: /'
cd /tmp
for size in 16 32 64 128; do
	for prof in "-m dup -d single" "-m single -d single" "-M -m single -d single"; do
		truncate -s ${size}M t.img
		if mkfs.btrfs -q -f $prof t.img > /tmp/mk.log 2>&1; then
			echo "guest: mkfs $prof size ${size}M ok"
		else
			echo "guest: mkfs $prof size ${size}M fail: $(tail -1 /tmp/mk.log)"
		fi
	done
done
for csum in xxhash sha256 blake2; do
	truncate -s 128M t.img
	mkfs.btrfs -q -f --csum $csum t.img > /tmp/mk.log 2>&1 && echo "guest: mkfs csum $csum ok" || echo "guest: mkfs csum $csum fail"
done
for f in extent-tree-v2 raid-stripe-tree squota block-group-tree; do
	truncate -s 256M t.img
	mkfs.btrfs -q -f -O $f t.img > /tmp/mk.log 2>&1 && echo "guest: mkfs feature $f ok" || echo "guest: mkfs feature $f fail: $(tail -1 /tmp/mk.log)"
done
mkfs.btrfs -q -f t.img >/dev/null 2>&1
mount -o loop t.img /mnt && echo "guest: mount ok" && df /mnt | sed 's/^/guest: /'
umount /mnt
for m in xxhash sha256 blake2; do
	truncate -s 128M t.img
	mkfs.btrfs -q -f --csum $m t.img > /dev/null 2>&1
	mount -o loop t.img /mnt 2>&1 | sed 's/^/guest: /'
	mountpoint -q /mnt && { echo "guest: csum $m mount ok"; umount /mnt; }
done
exit 0
