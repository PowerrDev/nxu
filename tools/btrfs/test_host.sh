#!/bin/bash
#
# Host tests of the Btrfs core, behind `make test-btrfs-host`.
#
# Everything runs the pure core (compiled natively with AddressSanitizer and
# UndefinedBehaviorSanitizer) over the real-Linux fixtures in
# tools/btrfs/fixtures, comparing with the ground truth Linux produced (data checksums are verified
# by default):
#
#   superblock   what `btrfs inspect-internal dump-super` said about each image
#   tree walk    every path, type, mode, owner, size, nlink, inode, rdev, mtime,
#                symlink target and the crc32c + sha256 of every file's contents,
#                partial/EOF/stepped reads, readdir cursors, lookups by name, hard
#                link counts, subvolume crossing, all with and without data checksums
#   subvolumes   ROOT_REF/BACKREF against `btrfs subvolume list`, the default
#                subvolume, every subvolume mounted by id
#   compression  zlib, LZO and ZSTD extents read byte-identically to Linux's view; the
#                decoders alone (test_codec.sh): real streams, limits, mutation sweeps
#   refusals     RAID/multi-device
#   damage       named corruptions of good images: each must give a clean status
#   sweep        seeded random corruption: only errors, or the good image's results
#
# usage: test_host.sh <btrfs_host binary> <scratch dir>
# Environment: BTRFS_SWEEP_ITERS (default 300), BTRFS_SWEEP_SEEDS (default 2)
# Plain bash 3.2 (no associative arrays).

set -u

HOST=${1:?usage: test_host.sh <btrfs_host> <scratch dir>}
SCRATCH=${2:?usage: test_host.sh <btrfs_host> <scratch dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
FIX=$HERE/fixtures
ITERS=${BTRFS_SWEEP_ITERS:-300}
SEEDS=${BTRFS_SWEEP_SEEDS:-2}

mkdir -p "$SCRATCH/img" "$SCRATCH/bad"
IMG=$SCRATCH/img
BAD=$SCRATCH/bad

command -v zstd > /dev/null 2>&1 || { echo "test_host: zstd not found"; exit 2; }
command -v python3 > /dev/null 2>&1 || { echo "test_host: python3 not found"; exit 2; }

failures=0
count=0

pass() { count=$((count + 1)); printf 'ok    %s\n' "$1"; }
fail() { count=$((count + 1)); failures=$((failures + 1)); printf 'FAIL  %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/      | /' | tail -12; }

# image NAME: decompress a fixture (once) and print its path.
image() {
	local name=$1
	if [ ! -f "$IMG/$name.img" ] || [ "$FIX/$name.img.zst" -nt "$IMG/$name.img" ]; then
		zstd -d -q --sparse -f "$FIX/$name.img.zst" -o "$IMG/$name.img" || { echo "test_host: cannot decompress $name" >&2; exit 2; }
	fi
	printf '%s' "$IMG/$name.img"
}

# field FILE KEY: value of "KEY=value" (btrfs_host info) or "KEY   value" (dump-super).
kv() { grep -E "^$2=" "$1" | head -1 | cut -d= -f2-; }
sb() { grep -E "^$2[[:space:]]" "$1" | head -1 | awk '{print $2}'; }

# ---- superblock and features against dump-super ----------------------------------

check_info() {
	local name=$1 out
	out=$("$HOST" info "$(image "$name")" 2>&1)
	local info=$FIX/$name.info bad=""

	for pair in generation:generation root:root chunk_root:chunk_root total_bytes:total_bytes bytes_used:bytes_used sectorsize:sectorsize nodesize:nodesize root_level:root_level chunk_root_level:chunk_root_level; do
		local ours=${pair%%:*} theirs=${pair##*:}
		local a b
		a=$(printf '%s\n' "$out" | kv /dev/stdin "$ours")
		b=$(sb "$info" "$theirs")
		[ "$a" = "$b" ] || bad="$bad $ours(ours=$a linux=$b)"
	done

	local label_ours label_linux
	label_ours=$(printf '%s\n' "$out" | kv /dev/stdin label)
	label_linux=$(grep -E '^label' "$info" | head -1 | awk '{print $2}')
	[ "$label_ours" = "$label_linux" ] || bad="$bad label(ours=$label_ours linux=$label_linux)"

	if [ -z "$bad" ]; then pass "super $name"; else fail "super $name: differs from dump-super:$bad" "$out"; fi
}

# ---- full tree walk against the manifest -------------------------------------------

check_tree() {
	local name=$1 manifest=$2
	shift 2
	local out
	out=$("$HOST" check "$(image "$name")" "$FIX/$manifest" "$@" 2>&1)
	local rc=$?

	if [ $rc -eq 0 ]; then
		pass "walk $name ($manifest $*)"
	else
		fail "walk $name ($manifest $*)" "$out"
	fi
}

# ---- one image, one expected open status --------------------------------------------

expect_open() {
	local label=$1 image=$2 expected=$3
	shift 3
	local out
	out=$("$HOST" open "$image" "$@" 2>&1)

	if printf '%s\n' "$out" | grep -qF "open=$expected"; then
		pass "$label -> $expected"
	else
		fail "$label: expected open=$expected" "$out"
	fi
}

expect_walk_fails() {
	local label=$1 image=$2 pattern=$3
	local out
	out=$("$HOST" walk "$image" 2>&1)
	local rc=$?

	if [ $rc -ne 0 ] && printf '%s\n' "$out" | grep -qF "$pattern" && ! printf '%s\n' "$out" | grep -q 'AddressSanitizer\|runtime error'; then
		pass "$label -> $pattern"
	else
		fail "$label: expected a clean failure containing \"$pattern\" (rc $rc)" "$out"
	fi
}

# corrupt KIND SRC-NAME -> path of the damaged copy
corrupt() {
	local kind=$1 name=$2
	local out=$BAD/$name.${kind//[:\/]/_}.img
	"$HOST" info "$(image "$name")" > "$SCRATCH/$name.info.txt" 2>&1
	case "$kind" in
	data-extent:*|nodatasum-off:*)
		# The kind names a file: add where its extents and inode item are.
		local file=${kind#*:}
		"$HOST" extents "$(image "$name")" "${file%%:*}" >> "$SCRATCH/$name.info.txt" 2>&1
		;;
	esac
	python3 "$HERE/corrupt.py" "$kind" "$(image "$name")" "$out" "$SCRATCH/$name.info.txt" || { echo "test_host: corrupt.py failed for $kind" >&2; exit 2; }
	printf '%s' "$out"
}

echo "== superblocks =="
for name in empty minimal tree deep n4k n64k s16k meta-single data-dup mixed no-holes-off no-skinny space-cache-v1 block-group-tree squota nodatasum subvols subvols-default comp-zlib comp-lzo comp-zstd csum-xxhash csum-sha256 csum-blake2 mix-zlib1 mix-zlib9 mix-lzo mix-zstd1 mix-zstd3 mix-zstd15 mix-random; do
	check_info "$name"
done

echo "== full tree walks, compared with Linux's manifests =="
for name in empty minimal tree deep n4k n64k s16k meta-single data-dup mixed no-holes-off no-skinny space-cache-v1 block-group-tree squota nodatasum subvols subvols-default; do
	check_tree "$name" "$name.manifest"
	check_tree "$name" "$name.manifest" --noverify
done

echo "== every subvolume mounted by id =="
for name in subvols subvols-default; do
	for m in "$FIX"/$name.subvol*.manifest; do
		id=${m##*subvol}
		id=${id%.manifest}
		check_tree "$name" "$(basename "$m")" --subvol "$id"
	done
done

echo "== subvolume relationships and the default subvolume =="
for name in subvols subvols-default; do
	out=$("$HOST" subvols "$(image "$name")" 2>&1)
	bad=""
	# `btrfs subvolume list` lines: "ID 256 gen 11 top level 5 path sub1"
	while IFS= read -r line; do
		id=$(printf '%s' "$line" | sed -n 's/^subvolume: ID \([0-9]*\) .*/\1/p')
		parent=$(printf '%s' "$line" | sed -n 's/.*top level \([0-9]*\) path.*/\1/p')
		path=$(printf '%s' "$line" | sed -n 's/.* path //p')
		leaf=${path##*/}
		printf '%s\n' "$out" | grep -qF "subvol id=$id parent=$parent name=$leaf " || bad="$bad id$id"
	done < "$FIX/$name.subvols"
	printf '%s\n' "$out" | grep -qF "subvol id=5 " || bad="$bad id5"
	nlist=$(wc -l < "$FIX/$name.subvols" | tr -d ' ')
	nours=$(printf '%s\n' "$out" | grep -c '^subvol ')
	[ "$nours" -eq $((nlist + 1)) ] || bad="$bad count(ours=$nours linux=$nlist+1)"
	if [ -z "$bad" ]; then pass "subvolume list $name"; else fail "subvolume list $name:$bad" "$out"; fi
done

want=$(sed -n 's/^default_subvolid: //p' "$FIX/subvols-default.default")
got=$("$HOST" info "$(image subvols-default)" | kv /dev/stdin default_subvol)
[ "$want" = "$got" ] && pass "default subvolume is $want (subvols-default)" || fail "default subvolume: driver $got, Linux $want"
got=$("$HOST" info "$(image subvols)" | kv /dev/stdin default_subvol)
[ "$got" = 5 ] && pass "default subvolume is 5 (subvols)" || fail "default subvolume of subvols is $got"

lvl=$("$HOST" info "$(image deep)" | kv /dev/stdin mount_tree_level)
want=$(sed -n 's/^fs_tree_level: //p' "$FIX/deep.info")
[ "$lvl" = "$want" ] && [ "$lvl" -ge 2 ] && pass "deep: fs tree level $lvl" || fail "deep: tree level driver $lvl, Linux $want"

# read_field OUTPUT KEY: the value of "KEY=value" on a `btrfs_host read` line.
rf() {
	if [ "$2" = read ]; then printf '%s\n' "$1" | sed -n 's/^read=\(.*\) done=.*/\1/p' | head -1
	else printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; fi
}

echo "== data checksums: on by default, bad extents are I/O errors, DUP heals, NODATASUM is exempt =="
FILE=/dir/sub/data.bin
want=$("$HOST" read "$(image meta-single)" $FILE)
wcrc=$(rf "$want" crc)
wdone=$(rf "$want" done)
[ "$(rf "$want" read)" = ok ] && [ "$wdone" -gt 200000 ] && pass "read $FILE of a good image (crc $wcrc, $wdone bytes)" || fail "cannot read $FILE of the good image" "$want"

# Cost of the check: verifying a 3 MiB single-extent file reads the data in 64 KiB pieces (48)
# and costs only a few csum-tree blocks on top (the cache serves the rest), not a read per sector.
plain=$("$HOST" read "$(image tree)" /data/one-extent --noverify)
checked=$("$HOST" read "$(image tree)" /data/one-extent)
extra=$(( $(rf "$checked" device_reads) - $(rf "$plain" device_reads) ))
extra_blocks=$(( $(rf "$checked" cache_misses) - $(rf "$plain" cache_misses) ))
if [ "$(rf "$checked" crc)" = "$(rf "$plain" crc)" ] && [ "$(rf "$checked" checked)" -ge 768 ] && [ "$extra" -le 52 ] && [ "$extra_blocks" -le 3 ]; then pass "checksum cost: 3 MiB verified ($(rf "$checked" checked) sectors) took $extra extra device reads and $extra_blocks extra tree blocks"; else fail "verification cost out of bounds (extra device reads $extra, tree blocks $extra_blocks)" "$plain
$checked"; fi

# Single data, first sector of the extent flipped: the read fails, nothing is served.
bad=$(corrupt "data-extent:$FILE:first" meta-single)
out=$("$HOST" read "$bad" $FILE); rc=$?
if [ $rc -eq 1 ] && [ "$(rf "$out" read)" = "checksum mismatch" ] && [ "$(rf "$out" done)" = 0 ] && [ "$(rf "$out" csum_failures)" -ge 1 ]; then pass "single data, damaged extent: read fails with a checksum mismatch, 0 bytes served"; else fail "damaged single extent must fail the read (rc $rc)" "$out"; fi

# ... a read of another part of the same extent still works: verification is per sector.
out=$("$HOST" read "$bad" $FILE --offset 65536 --length 65536)
ref=$("$HOST" read "$(image meta-single)" $FILE --offset 65536 --length 65536)
if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" crc)" = "$(rf "$ref" crc)" ]; then pass "single data: sectors of the same extent that are intact are still served"; else fail "an intact sector next to a damaged one must read fine" "$out"; fi
out=$("$HOST" read "$bad" $FILE --offset 4000 --length 200); rc=$?
if [ $rc -eq 1 ] && [ "$(rf "$out" read)" = "checksum mismatch" ]; then pass "single data: a read that touches the damaged sector fails"; else fail "a read touching the damaged sector must fail" "$out"; fi

# The opt-out returns the (damaged) bytes: that is exactly what noverify means.
out=$("$HOST" read "$bad" $FILE --noverify)
if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" crc)" != "$wcrc" ] && [ "$(rf "$out" csum_failures)" = 0 ]; then pass "noverify: the damaged bytes are served unchecked"; else fail "noverify must skip the data check" "$out"; fi

# DUP data: one bad copy heals from the other; both bad fail.
want=$("$HOST" read "$(image data-dup)" $FILE)
out=$("$HOST" read "$(corrupt "data-extent:$FILE:first" data-dup)" $FILE)
if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" crc)" = "$(rf "$want" crc)" ] && [ "$(rf "$out" csum_failures)" -ge 1 ] && [ "$(rf "$out" mirror_fallbacks)" -ge 1 ]; then pass "DUP data, first copy damaged: healed from the second copy (mirror fallback counted)"; else fail "DUP data must heal" "$out"; fi
out=$("$HOST" read "$(corrupt "data-extent:$FILE:second" data-dup)" $FILE)
if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" crc)" = "$(rf "$want" crc)" ] && [ "$(rf "$out" csum_failures)" = 0 ]; then pass "DUP data, second copy damaged: the first copy serves, no failure seen"; else fail "DUP data with a damaged second copy" "$out"; fi
out=$("$HOST" read "$(corrupt "data-extent:$FILE:all" data-dup)" $FILE); rc=$?
if [ $rc -eq 1 ] && [ "$(rf "$out" read)" = "checksum mismatch" ] && [ "$(rf "$out" done)" = 0 ]; then pass "DUP data, both copies damaged: the read fails"; else fail "DUP data with both copies damaged must fail (rc $rc)" "$out"; fi

# NODATASUM files: nothing protects them, nothing is checked, they still read.
want=$("$HOST" read "$(image nodatasum)" $FILE)
out=$("$HOST" read "$(corrupt "data-extent:$FILE:all" nodatasum)" $FILE)
if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" done)" = "$(rf "$want" done)" ] && [ "$(rf "$out" crc)" != "$(rf "$want" crc)" ] && [ "$(rf "$out" csum_failures)" = 0 ]; then pass "NODATASUM file: damaged data is served without a check"; else fail "NODATASUM data must be read as it is" "$out"; fi

# A file that must have checksums but whose csum items are gone (flag cleared on a NODATASUM
# fixture): refused, not served unverified.
out=$("$HOST" read "$(corrupt "nodatasum-off:$FILE" nodatasum)" $FILE); rc=$?
if [ $rc -eq 1 ] && [ "$(rf "$out" read)" = "checksum mismatch" ] && [ "$(rf "$out" csum_missing)" -ge 1 ]; then pass "missing checksum items: the read is refused (never served unverified)"; else fail "a missing checksum must fail the read (rc $rc)" "$out"; fi
out=$("$HOST" read "$(corrupt "nodatasum-off:$FILE" nodatasum)" $FILE --noverify)
if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" crc)" = "$(rf "$want" crc)" ]; then pass "missing checksum items with noverify: served"; else fail "noverify with missing checksums" "$out"; fi

# Prealloc, holes and sparse files have no checksums to miss: the tree fixture (PREALLOC
# files, sparse files, a nodatacow file) walks clean above with verification on.

# Compressed extents are verified too: the csum covers the bytes on disk, so damage is
# caught before any decoder sees them; noverify hands the damaged bytes to the decoder,
# which may only decode or refuse cleanly.
for name in mix-zlib9 mix-lzo mix-zstd3; do
	want=$("$HOST" read "$(image $name)" /e128k)
	out=$("$HOST" read "$(corrupt "data-extent:/e128k:all" $name)" /e128k); rc=$?
	if [ "$(rf "$want" read)" = ok ] && [ $rc -eq 1 ] && [ "$(rf "$out" read)" = "checksum mismatch" ] && [ "$(rf "$out" done)" = 0 ]; then pass "$name: a damaged compressed extent fails the read with a checksum mismatch"; else fail "$name: damaged compressed extent (rc $rc)" "$out"; fi
	out=$("$HOST" read "$(corrupt "data-extent:/e128k:all" $name)" /e128k --noverify)
	case "$(rf "$out" read)" in ok|"corrupt metadata") pass "$name: noverify hands the damaged extent to the decoder: $(rf "$out" read)" ;; *) fail "$name: noverify on a damaged compressed extent" "$out" ;; esac
done

echo "== compressed extents (zlib levels, LZO, ZSTD levels, boundaries, sparse, partial references, incompressible) read as Linux read them =="
for name in comp-zlib comp-lzo comp-zstd mix-zlib1 mix-zlib9 mix-lzo mix-zstd1 mix-zstd3 mix-zstd15 mix-random; do
	check_tree "$name" "$name.manifest"
	check_tree "$name" "$name.manifest" --noverify
done

echo "== log tree: fsynced changes not yet committed are replayed in memory =="
check_tree logtree logtree.manifest
check_tree logtree logtree.manifest --noverify
check_tree logtree logtree.base.manifest --ignore-log

echo "== every checksum type: metadata and data verified, compared with Linux's view =="
for name in csum-xxhash csum-sha256 csum-blake2; do
	check_tree "$name" "$name.manifest"
	check_tree "$name" "$name.manifest" --noverify
done

# ---- multi-device filesystems -------------------------------------------------------

# mdimg NAME I: the image of device I of a multi-device fixture (NAME.I.img.zst).
mdimg() { image "$1.$2"; }

# mddevs NAME N [SKIP...]: "--dev IMG1 --dev IMG2 ..." for devices 1..N-1 (the first is the image argument).
mddevs() {
	local name=$1 n=$2 i out=""
	for ((i = 1; i < n; i++)); do out="$out --dev $(mdimg "$name" $i)"; done
	printf '%s' "$out"
}

# devid_of IMAGE: the device id in its superblock.
devid_of() { python3 -c "import struct,sys; f=open(sys.argv[1],'rb'); f.seek(0x10000+201); print(struct.unpack('<Q', f.read(8))[0])" "$1"; }

MDNAMES="md-raid1:2 md-raid0:2 md-raid10:4 md-raid1c3:3 md-raid1c4:4 md-raid5:3 md-raid6:4 md-single:2"

echo "== multi-device filesystems: every profile walks byte-identically to what Linux read =="
for spec in $MDNAMES; do
	name=${spec%%:*} n=${spec##*:}
	devs=$(mddevs "$name" "$n")
	# shellcheck disable=SC2086
	out=$("$HOST" check "$(mdimg "$name" 0)" "$FIX/$name.manifest" $devs 2>&1); rc=$?
	if [ $rc -eq 0 ]; then pass "walk $name ($n devices: $(printf '%s' "$out" | tail -1 | sed 's/^PASS [^:]*: //'))"; else fail "walk $name ($n devices)" "$out"; fi
	# shellcheck disable=SC2086
	out=$("$HOST" check "$(mdimg "$name" 0)" "$FIX/$name.manifest" $devs --noverify 2>&1); rc=$?
	if [ $rc -eq 0 ]; then pass "walk $name without data verification"; else fail "walk $name --noverify" "$out"; fi

	# The devices in reverse order: the driver must not care which one is first.
	last=$((n - 1)) rev=""
	for ((i = last - 1; i >= 0; i--)); do rev="$rev --dev $(mdimg "$name" $i)"; done
	# shellcheck disable=SC2086
	out=$("$HOST" check "$(mdimg "$name" $last)" "$FIX/$name.manifest" $rev 2>&1); rc=$?
	if [ $rc -eq 0 ]; then pass "walk $name with the devices in reverse order"; else fail "walk $name, reversed" "$out"; fi
done

echo "== multi-device: missing, foreign and duplicate devices are refused =="
for spec in $MDNAMES; do
	name=${spec%%:*} n=${spec##*:}
	# The first device alone, and every device but the last.
	expect_open "$name: device 0 alone" "$(mdimg "$name" 0)" "a device of the filesystem is missing"
	if [ "$n" -gt 2 ]; then
		# shellcheck disable=SC2046
		expect_open "$name: $((n - 1)) of $n devices" "$(mdimg "$name" 0)" "a device of the filesystem is missing" $(mddevs "$name" $((n - 1)))
	fi
done
expect_open "a device of another filesystem" "$(mdimg md-raid1 0)" "invalid argument" --dev "$(mdimg md-raid0 1)"
expect_open "the same device twice" "$(mdimg md-raid1 0)" "invalid argument" --dev "$(mdimg md-raid1 0)"
expect_open "too many devices" "$(mdimg md-raid1 0)" "invalid argument" --dev "$(mdimg md-raid1 1)" --dev "$(mdimg md-raid0 0)"

echo "== multi-device: a mirror that fails its checksum falls back to another copy =="
FILE=/dir/sub/data.bin
# damaged NAME N WHICH...: a copy of device images with the data extent's copy on device id WHICH... damaged.
# Prints the "--dev" argument list, with the damaged images in place of the good ones.
damage_md() {
	local name=$1 n=$2 target=$3 i out="" dev primary
	shift 3
	# shellcheck disable=SC2046
	"$HOST" info "$(mdimg "$name" 0)" $(mddevs "$name" "$n") > "$SCRATCH/$name.md.info" 2>&1
	# shellcheck disable=SC2046
	"$HOST" extents "$(mdimg "$name" 0)" "$FILE" $(mddevs "$name" "$n") >> "$SCRATCH/$name.md.info" 2>&1
	for ((i = 0; i < n; i++)); do
		dev=$(mdimg "$name" $i)
		id=$(devid_of "$dev")
		case " $* " in
		*" $id "*)
			python3 "$HERE/corrupt.py" "data-extent:$FILE:dev=$id" "$dev" "$BAD/$name.$i.$target.img" "$SCRATCH/$name.md.info" || { echo "test_host: corrupt.py failed" >&2; exit 2; }
			dev=$BAD/$name.$i.$target.img
			;;
		esac
		if [ $i -eq 0 ]; then primary=$dev; else out="$out --dev $dev"; fi
	done
	printf '%s%s' "$primary" "$out"
}

md_first_copy_devices() {
	# the device ids of the copies of the file's first extent, in copy order
	local name=$1 n=$2
	# shellcheck disable=SC2046
	"$HOST" extents "$(mdimg "$name" 0)" "$FILE" $(mddevs "$name" "$n") | sed -n 's/^extentdev=[0-9]*,//p' | head -1 | tr ',' ' '
}

md_read() { # the damaged set given as one string: primary then --dev arguments
	# shellcheck disable=SC2086
	set -- $1
	local primary=$1
	shift
	"$HOST" read "$primary" $FILE "$@"
}

for spec in md-raid1:2 md-raid1c3:3 md-raid10:4 md-raid0:2; do
	name=${spec%%:*} n=${spec##*:}
	# shellcheck disable=SC2046
	want=$("$HOST" read "$(mdimg "$name" 0)" $FILE $(mddevs "$name" "$n"))
	copies=$(md_first_copy_devices "$name" "$n")
	ncopies=$(printf '%s\n' $copies | wc -l | tr -d ' ')
	first=$(printf '%s\n' $copies | head -1)
	if [ "$(rf "$want" read)" != ok ]; then fail "$name: cannot read $FILE from the good set" "$want"; continue; fi

	# One copy (the first one tried) damaged.
	set_=$(damage_md "$name" "$n" one $first)
	out=$(md_read "$set_"); rc=$?
	if [ "$ncopies" -ge 2 ]; then
		if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" crc)" = "$(rf "$want" crc)" ] && [ "$(rf "$out" csum_failures)" -ge 1 ] && [ "$(rf "$out" mirror_fallbacks)" -ge 1 ]; then pass "$name: first copy damaged: served from another device (fallback counted)"; else fail "$name: damaged first copy must heal" "$out"; fi
	else
		if [ $rc -eq 1 ] && [ "$(rf "$out" read)" = "checksum mismatch" ] && [ "$(rf "$out" done)" = 0 ]; then pass "$name: the only copy damaged: the read fails, nothing served"; else fail "$name: damaged only copy must fail" "$out"; fi
	fi

	# All copies but one damaged (a mirrored profile still has the last one).
	if [ "$ncopies" -ge 3 ]; then
		set_=$(damage_md "$name" "$n" most $(printf '%s\n' $copies | head -$((ncopies - 1)) | tr '\n' ' '))
		out=$(md_read "$set_")
		if [ "$(rf "$out" read)" = ok ] && [ "$(rf "$out" crc)" = "$(rf "$want" crc)" ] && [ "$(rf "$out" mirror_fallbacks)" -ge 2 ]; then pass "$name: $((ncopies - 1)) of $ncopies copies damaged: the last one serves"; else fail "$name: all but one copy damaged must heal" "$out"; fi
	fi

	# Every copy damaged.
	if [ "$ncopies" -ge 2 ]; then
		set_=$(damage_md "$name" "$n" all $copies)
		out=$(md_read "$set_"); rc=$?
		if [ $rc -eq 1 ] && [ "$(rf "$out" read)" = "checksum mismatch" ] && [ "$(rf "$out" done)" = 0 ]; then pass "$name: every copy damaged: the read fails"; else fail "$name: all copies damaged must fail (rc $rc)" "$out"; fi
	fi
done

echo "== unsupported images are refused cleanly =="
expect_open "raid1 (device 0 of 2)" "$(image raid1)" "a device of the filesystem is missing"
expect_open "raid0 (device 0 of 2)" "$(image raid0)" "a device of the filesystem is missing"
expect_open "raid5 (device 0 of 3)" "$(image raid5)" "a device of the filesystem is missing"
expect_open "single profile over 2 devices" "$(image single2dev)" "a device of the filesystem is missing"
expect_open "subvolume id that does not exist" "$(image subvols)" "not found" --subvol 9999
expect_open "mount a non-subvolume tree" "$(image subvols)" "not found" --subvol 7

echo "== damaged superblocks =="
expect_open "magic zeroed in every copy" "$(corrupt super-magic-all minimal)" "not a Btrfs filesystem"
expect_open "checksum broken in every copy" "$(corrupt super-csum-all minimal)" "checksum mismatch"
expect_open "primary damaged, mirror intact" "$(corrupt super-primary minimal)" "ok"
out=$("$HOST" info "$(corrupt super-primary minimal)" | kv /dev/stdin super_copies_valid)
[ "$out" = 1 ] && pass "primary damaged: mount used the one valid mirror" || fail "primary damaged: super_copies_valid=$out"
expect_open "unknown incompat bit 40" "$(corrupt incompat-bit:40 minimal)" "unsupported feature"
expect_open "extent-tree-v2 (incompat bit 13)" "$(corrupt incompat-bit:13 minimal)" "unsupported feature"
expect_open "raid-stripe-tree (incompat bit 14)" "$(corrupt incompat-bit:14 minimal)" "unsupported feature"
expect_open "zoned (incompat bit 12)" "$(corrupt incompat-bit:12 minimal)" "unsupported feature"
expect_open "checksum type 1 (xxhash) claimed by a crc32c image" "$(corrupt csum-type:1 minimal)" "checksum mismatch"
expect_open "checksum type 9" "$(corrupt csum-type:9 minimal)" "corrupt metadata"
expect_open "log_root that is not a log tree" "$(corrupt log-root minimal)" "corrupt metadata"
expect_open "log_root that is not a log tree, told to ignore the log" "$(corrupt log-root minimal)" "ok" --ignore-log
expect_open "real log tree" "$(image logtree)" "ok"
expect_open "real log tree, told to ignore it" "$(image logtree)" "ok" --ignore-log
expect_open "log root block damaged" "$(corrupt log-block:root:all logtree)" "checksum mismatch"
expect_open "log tree block damaged" "$(corrupt log-block:tree:all logtree)" "checksum mismatch"
expect_open "log root block damaged, told to ignore the log" "$(corrupt log-block:root:all logtree)" "ok" --ignore-log
expect_open "nonsense nodesize" "$(corrupt nodesize:12345 minimal)" "corrupt metadata"
expect_open "nodesize larger than allowed" "$(corrupt nodesize:131072 minimal)" "corrupt metadata"
expect_open "claims two devices" "$(corrupt num-devices:2 minimal)" "a device of the filesystem is missing"
expect_open "device claimed larger than the image" "$(corrupt dev-total-bytes minimal)" "device smaller than the filesystem"
expect_open "garbage sys_chunk_array" "$(corrupt sys-array-garbage minimal)" "corrupt metadata"

echo "== truncated devices =="
expect_open "cut to 40 MiB" "$(corrupt truncate:41943040 minimal)" "device smaller than the filesystem"
expect_open "cut to 64 KiB + 100 bytes" "$(corrupt truncate:65636 minimal)" "not a Btrfs filesystem"
expect_open "cut to 1000 bytes" "$(corrupt truncate:1000 minimal)" "not a Btrfs filesystem"
expect_open "empty device" "$(corrupt truncate:0 minimal)" "not a Btrfs filesystem"

echo "== damaged tree blocks =="
expect_open "root tree block damaged (every copy)" "$(corrupt root-block:all minimal)" "checksum mismatch"
expect_open "chunk tree block damaged (every copy)" "$(corrupt chunk-block:all minimal)" "checksum mismatch"
expect_walk_fails "subvolume root block damaged (every copy)" "$(corrupt fs-block:all minimal)" "checksum mismatch"
expect_open "root tree damaged in the only copy (single metadata)" "$(corrupt root-block:first meta-single)" "checksum mismatch"
expect_walk_fails "walk with the tree root damaged" "$(corrupt fs-block:all tree)" "checksum mismatch"

# DUP metadata heals: only the first copy of each block is bad, the walk must still be right.
out=$("$HOST" walk "$(corrupt root-block:first tree)" 2>&1)
if printf '%s\n' "$out" | grep -q 'walk=ok' && printf '%s\n' "$out" | grep -qE 'mirror_fallbacks=[1-9]'; then pass "DUP heals a damaged root tree block (mirror fallback used)"; else fail "DUP did not heal" "$out"; fi
out=$("$HOST" walk "$(corrupt fs-block:first tree)" 2>&1)
if printf '%s\n' "$out" | grep -q 'walk=ok' && printf '%s\n' "$out" | grep -qE 'mirror_fallbacks=[1-9]'; then pass "DUP heals a damaged subvolume root block (mirror fallback used)"; else fail "DUP did not heal the subvolume root" "$out"; fi

echo "== corruption sweeps (seeded, ${ITERS} iterations x ${SEEDS} seeds) =="
for name in empty minimal n4k meta-single data-dup mixed no-holes-off subvols nodatasum comp-zlib mix-zstd3 mix-lzo csum-blake2 csum-xxhash logtree; do
	seed=1
	while [ $seed -le "$SEEDS" ]; do
		out=$("$HOST" sweep "$(image "$name")" --seed $seed --iters "$ITERS" 2>&1)
		rc=$?
		if [ $rc -eq 0 ]; then pass "sweep $name seed $seed: $(printf '%s' "$out" | sed 's/.*; mount refused/mount refused/')"; else fail "sweep $name seed $seed (rc $rc)" "$out"; fi
		seed=$((seed + 1))
	done
done

# The big ones get fewer iterations: a full walk of a 10 000 file image is slow under ASan.
for name in tree deep; do
	out=$("$HOST" sweep "$(image "$name")" --seed 7 --iters $((ITERS / 6 + 10)) 2>&1)
	rc=$?
	if [ $rc -eq 0 ]; then pass "sweep $name: $(printf '%s' "$out" | sed 's/.*; mount refused/mount refused/')"; else fail "sweep $name (rc $rc)" "$out"; fi
done

# The decoders on their own: real compressors' streams, limits, mutation sweeps.
echo "== compression decoders (tools/btrfs/test_codec.sh) =="
out=$(BTRFS_CODEC_ITERS=${BTRFS_CODEC_ITERS:-400} "$HERE/test_codec.sh" "$HOST" "$SCRATCH" 2>&1)
rc=$?
printf '%s\n' "$out" | grep -v -e '^$' -e 'codec check(s)'
codec_checks=$(printf '%s\n' "$out" | sed -n 's/^\([0-9]*\) codec check(s), \([0-9]*\) failure(s)$/\1/p')
codec_failures=$(printf '%s\n' "$out" | sed -n 's/^\([0-9]*\) codec check(s), \([0-9]*\) failure(s)$/\2/p')
count=$((count + ${codec_checks:-1}))
failures=$((failures + ${codec_failures:-$rc}))

printf '\n%d check(s), %d failure(s)\n' "$count" "$failures"
[ "$failures" -eq 0 ]
