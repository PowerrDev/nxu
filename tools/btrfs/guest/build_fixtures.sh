#!/bin/bash
# Guest-side fixture builder (runs inside the Alpine lab, see make_fixtures.sh).
#
# Every image is made with the real mkfs.btrfs, populated through the real
# Linux Btrfs driver (mount -o loop), described by a MANIFEST generated from
# the mounted filesystem, checked with `btrfs check`, and compressed with zstd.
# Everything lands in /out, which is streamed to the host as a tar archive.
#
# NXU_ONLY (space separated group names) restricts what is built.

set -u
. /root/guest/lib.sh

setup_modules || { say "modloop setup failed"; exit 1; }
install_tools || { say "apk add failed"; exit 1; }
apk add --no-progress attr e2fsprogs-extra > /dev/null 2>&1 || true
modprobe loop
modprobe btrfs

WORK=/work
OUT=/out
GUESTDIR=/root/guest
mkdir -p $WORK $OUT /mnt

fail() { say "FATAL: $*"; exit 1; }

want() {
	[ -z "${NXU_ONLY:-}" ] && return 0
	for g in $NXU_ONLY; do [ "$g" = "$1" ] && return 0; done
	return 1
}

uuid_for() {
	# A stable fsid per fixture name, so reruns produce the same fsid.
	python3 -c "import uuid,sys; print(uuid.uuid5(uuid.NAMESPACE_DNS, 'nxu-btrfs-fixture-' + sys.argv[1]))" "$1"
}

# new_fs NAME SIZE_MB [mkfs args...]: create and format $WORK/NAME.img.
new_fs() {
	local name=$1 size=$2
	shift 2
	rm -f "$WORK/$name.img"
	truncate -s ${size}M "$WORK/$name.img"
	mkfs.btrfs -q -f -L "$name" -U "$(uuid_for "$name")" "$@" "$WORK/$name.img" > "$WORK/mkfs.log" 2>&1 || { cat "$WORK/mkfs.log"; fail "mkfs $name"; }
}

# mount_fs NAME [mount options] [subvolid]: mount at /mnt/NAME.
mount_fs() {
	local name=$1 opts=${2:-}
	mkdir -p /mnt/$name
	mount -o "loop${opts:+,$opts}" "$WORK/$name.img" /mnt/$name || fail "mount $name ($opts)"
}

# manifest NAME MOUNTPOINT OUTNAME [extra manifest.py args]
manifest() {
	local mp=$1 dest=$2
	shift 2
	python3 $GUESTDIR/manifest.py "$mp" "$@" > "$OUT/$dest" || fail "manifest $dest"
}

# finish NAME: check, record the superblock dump, compress the image.
finish() {
	local name=$1
	sync
	umount /mnt/$name || fail "umount $name"
	btrfs check --readonly "$WORK/$name.img" > "$WORK/check.log" 2>&1 || { cat "$WORK/check.log"; fail "btrfs check $name"; }
	# The root of the tree is printed first: "node ... level N ..." or "leaf ...".
	first=$(btrfs inspect-internal dump-tree -t 5 "$WORK/$name.img" 2>/dev/null | grep -m1 -E '^(node|leaf) ')
	case "$first" in
	node*) lvl=$(echo "$first" | sed 's/.* level \([0-9]*\).*/\1/') ;;
	*) lvl=0 ;;
	esac
	{
		btrfs inspect-internal dump-super -f "$WORK/$name.img"
		echo "fs_tree_level: $lvl"
	} > "$OUT/$name.info"
	zstd -19 -q --long=27 -f "$WORK/$name.img" -o "$OUT/$name.img.zst" || fail "zstd $name"
	rm -f "$WORK/$name.img"
	say "built $name ($(wc -c < "$OUT/$name.img.zst") bytes compressed)"
}

# simple NAME SIZE RECIPE [mkfs args...]: the common single-view fixture.
simple() {
	local name=$1 size=$2 recipe=$3
	shift 3
	new_fs "$name" "$size" "$@"
	mount_fs "$name" noatime
	python3 $GUESTDIR/pop.py "$recipe" /mnt/$name || fail "populate $name"
	sync
	manifest /mnt/$name "$name.manifest"
	finish "$name"
}

# ---- (a) minimal ------------------------------------------------------------
if want basic; then
	simple empty 128 empty
	simple minimal 128 minimal
fi

# ---- (b) directory tree -----------------------------------------------------
if want tree; then
	simple tree 128 tree
fi

# ---- (d) deep trees (4 KiB nodes: level >= 2, EXTREFs) ------------------------
if want deep; then
	simple deep 256 deep -n 4096 -s 4096
	lvl=$(sed -n 's/^fs_tree_level: //p' "$OUT/deep.info")
	say "deep: fs tree level $lvl"
	[ "${lvl:-0}" -ge 2 ] || fail "deep tree did not reach level 2 (level $lvl)"
fi

# ---- (c) subvolumes, snapshots, default subvolume ---------------------------
subvol_recipe() {
	local mp=$1
	echo "in the top level" > $mp/top.txt
	mkdir $mp/dir
	btrfs subvolume create $mp/sub1 > /dev/null
	echo "sub1 a v1" > $mp/sub1/a.txt
	mkdir $mp/sub1/d
	echo "sub1 deep" > $mp/sub1/d/x.txt
	ln -s a.txt $mp/sub1/link
	btrfs subvolume create $mp/sub1/nested > /dev/null
	echo "nested n" > $mp/sub1/nested/n.txt
	btrfs subvolume snapshot $mp/sub1 $mp/snap1 > /dev/null
	btrfs subvolume snapshot -r $mp/sub1 $mp/dir/snap-ro > /dev/null
	btrfs subvolume create $mp/dir/sub2 > /dev/null
	echo "sub2 only" > $mp/dir/sub2/s2.txt
	# Diverge the source from its snapshots.
	echo "sub1 a v2, longer than before" > $mp/sub1/a.txt
	echo "added later" > $mp/sub1/later.txt
	rm $mp/sub1/d/x.txt
	sync
}

subvol_views() {
	# One manifest per subvolume id, mounted with subvolid=N (id 5 is the top).
	local name=$1
	local mp=/mnt/$1
	local ids id
	ids=$(btrfs subvolume list $mp | sed -n 's/^ID \([0-9]*\) .*/\1/p')
	btrfs subvolume list $mp | sed 's/^/subvolume: /' > "$OUT/$name.subvols"
	umount $mp
	for id in 5 $ids; do
		mount -o loop,ro,subvolid=$id "$WORK/$name.img" $mp || fail "mount subvolid=$id"
		manifest $mp "$name.subvol$id.manifest"
		umount $mp
	done
	mount -o loop,noatime "$WORK/$name.img" $mp || fail "remount $name"
}

if want subvols; then
	new_fs subvols 128
	mount_fs subvols noatime
	subvol_recipe /mnt/subvols
	manifest /mnt/subvols subvols.manifest
	subvol_views subvols
	finish subvols

	new_fs subvols-default 128
	mount_fs subvols-default noatime
	subvol_recipe /mnt/subvols-default
	sid=$(btrfs subvolume list /mnt/subvols-default | sed -n 's/^ID \([0-9]*\) .* path sub1$/\1/p')
	[ -n "$sid" ] || fail "cannot find sub1"
	btrfs subvolume set-default "$sid" /mnt/subvols-default || fail "set-default"
	sync
	echo "default_subvolid: $sid" > "$OUT/subvols-default.default"
	umount /mnt/subvols-default
	mount -o loop,ro "$WORK/subvols-default.img" /mnt/subvols-default || fail "mount default"
	manifest /mnt/subvols-default subvols-default.manifest
	umount /mnt/subvols-default
	mount -o loop,noatime "$WORK/subvols-default.img" /mnt/subvols-default
	subvol_views subvols-default
	finish subvols-default
fi

# ---- (e) format variants ---------------------------------------------------
if want variants; then
	simple n4k 128 small -n 4096 -s 4096
	simple n64k 128 small -n 65536
	simple meta-single 128 small -m single -d single
	simple data-dup 256 small -m dup -d dup
	simple mixed 64 small -M -n 4096 -s 4096 -m single -d single
	simple no-holes-off 128 small -O ^no-holes
	simple no-skinny 128 small -O ^skinny-metadata
	simple space-cache-v1 128 small -O ^free-space-tree
	simple block-group-tree 128 small -O block-group-tree
	simple squota 128 small -O squota

	# nodatasum: no checksum items for the data.
	new_fs nodatasum 128
	mount_fs nodatasum noatime,nodatasum
	python3 $GUESTDIR/pop.py small /mnt/nodatasum
	sync
	manifest /mnt/nodatasum nodatasum.manifest
	finish nodatasum

	# Sector sizes the running kernel cannot mount (x86 has 4 KiB pages):
	# populated with mkfs.btrfs --rootdir instead. Inode numbers are assigned by
	# mkfs there, so the manifest leaves them out.
	rm -rf $WORK/stage
	mkdir -p $WORK/stage
	python3 $GUESTDIR/pop.py small $WORK/stage
	rm -f $WORK/s16k.img
	truncate -s 128M $WORK/s16k.img
	mkfs.btrfs -q -f -L s16k -U "$(uuid_for s16k)" -s 16384 -n 16384 --rootdir $WORK/stage $WORK/s16k.img > $WORK/mkfs.log 2>&1 || { cat $WORK/mkfs.log; fail "mkfs s16k"; }
	python3 $GUESTDIR/manifest.py $WORK/stage --no-ino --staging > $OUT/s16k.manifest
	mkdir -p /mnt/s16k
	# `finish` expects a mounted fs; do the tail of it by hand.
	btrfs check --readonly $WORK/s16k.img > $WORK/check.log 2>&1 || { cat $WORK/check.log; fail "btrfs check s16k"; }
	{ btrfs inspect-internal dump-super -f $WORK/s16k.img; echo "fs_tree_level: 0"; } > $OUT/s16k.info
	zstd -19 -q --long=27 -f $WORK/s16k.img -o $OUT/s16k.img.zst
	rm -f $WORK/s16k.img
	say "built s16k"
fi

# ---- (f) compressed extents (must be refused cleanly) -----------------------
if want compressed; then
	for algo in zlib lzo zstd; do
		name=comp-$algo
		new_fs $name 128
		mount_fs $name noatime,compress-force=$algo
		python3 $GUESTDIR/pop.py compressible /mnt/$name
		sync
		# Files that really are compressed on disk.
		: > $WORK/flags
		for f in comp_text comp_inline comp_big dir/comp_nested; do
			printf '/%s\tcompressed\n' "$f" >> $WORK/flags
		done
		manifest /mnt/$name "$name.manifest" --flags $WORK/flags
		btrfs inspect-internal dump-tree -t 5 "$WORK/$name.img" 2>/dev/null | grep -c "compression $(case $algo in zlib) echo 1;; lzo) echo 2;; zstd) echo 3;; esac)" > "$WORK/ncomp" || true
		say "$name: $(cat $WORK/ncomp) compressed extents"
		finish $name
	done
fi

# ---- (g) refusal fixtures ---------------------------------------------------
if want refusal; then
	simple csum-xxhash 128 small --csum xxhash
	simple csum-sha256 128 small --csum sha256
	simple csum-blake2 128 small --csum blake2

	# RAID profiles across several devices: the first device is the fixture.
	for spec in "raid1:raid1:2" "raid0:raid0:2" "raid5:raid5:3" "single2dev:single:2"; do
		name=${spec%%:*}
		rest=${spec#*:}
		prof=${rest%%:*}
		ndev=${rest##*:}
		devs=""
		i=0
		while [ $i -lt $ndev ]; do
			rm -f $WORK/$name.$i.img
			truncate -s 128M $WORK/$name.$i.img
			devs="$devs $WORK/$name.$i.img"
			i=$((i + 1))
		done
		mkfs.btrfs -q -f -L $name -U "$(uuid_for $name)" -d $prof -m $prof $devs > $WORK/mkfs.log 2>&1 || { cat $WORK/mkfs.log; fail "mkfs $name"; }
		{ btrfs inspect-internal dump-super -f $WORK/$name.0.img; echo "fs_tree_level: 0"; } > $OUT/$name.info
		zstd -19 -q --long=27 -f $WORK/$name.0.img -o $OUT/$name.img.zst
		rm -f $WORK/$name.*.img
		say "built $name"
	done
fi

# ---- hand everything to the host --------------------------------------------
say "archiving $(ls $OUT | wc -l) files, $(du -sk $OUT | cut -f1) KiB"
cd $OUT || fail "cd $OUT"
tar cf /dev/vda * || fail "tar to /dev/vda"
sync
say "done"
exit 0
