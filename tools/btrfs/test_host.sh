#!/bin/bash
#
# Host tests of the Btrfs core, behind `make test-btrfs-host`.
#
# Everything runs the pure core (compiled natively with AddressSanitizer and
# UndefinedBehaviorSanitizer) over the real-Linux fixtures in
# tools/btrfs/fixtures, comparing with the ground truth Linux produced:
#
#   superblock   what `btrfs inspect-internal dump-super` said about each image
#   tree walk    every path, type, mode, owner, size, nlink, inode, rdev, mtime,
#                symlink target and the crc32c + sha256 of every file's contents,
#                partial/EOF/stepped reads, readdir cursors, lookups by name, hard
#                link counts, subvolume crossing, all with and without data checksums
#   subvolumes   ROOT_REF/BACKREF against `btrfs subvolume list`, the default
#                subvolume, every subvolume mounted by id
#   refusals     compressed files, unsupported checksums, RAID/multi-device
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
	local out=$BAD/$name.${kind//[:]/_}.img
	"$HOST" info "$(image "$name")" > "$SCRATCH/$name.info.txt" 2>&1
	python3 "$HERE/corrupt.py" "$kind" "$(image "$name")" "$out" "$SCRATCH/$name.info.txt" || { echo "test_host: corrupt.py failed for $kind" >&2; exit 2; }
	printf '%s' "$out"
}

echo "== superblocks =="
for name in empty minimal tree deep n4k n64k s16k meta-single data-dup mixed no-holes-off no-skinny space-cache-v1 block-group-tree squota nodatasum subvols subvols-default comp-zlib comp-lzo comp-zstd; do
	check_info "$name"
done

echo "== full tree walks, compared with Linux's manifests =="
for name in empty minimal tree deep n4k n64k s16k meta-single data-dup mixed no-holes-off no-skinny space-cache-v1 block-group-tree squota nodatasum subvols subvols-default; do
	check_tree "$name" "$name.manifest"
	check_tree "$name" "$name.manifest" --verify-data
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

echo "== compressed extents are refused, everything else is served =="
for name in comp-zlib comp-lzo comp-zstd; do
	check_tree "$name" "$name.manifest"
	check_tree "$name" "$name.manifest" --verify-data
done

echo "== unsupported images are refused cleanly =="
expect_open "csum-xxhash" "$(image csum-xxhash)" "unsupported checksum type"
expect_open "csum-sha256" "$(image csum-sha256)" "unsupported checksum type"
expect_open "csum-blake2" "$(image csum-blake2)" "unsupported checksum type"
expect_open "raid1 (device 0 of 2)" "$(image raid1)" "unsupported RAID or multi-device profile"
expect_open "raid0 (device 0 of 2)" "$(image raid0)" "unsupported RAID or multi-device profile"
expect_open "raid5 (device 0 of 3)" "$(image raid5)" "unsupported RAID or multi-device profile"
expect_open "single profile over 2 devices" "$(image single2dev)" "unsupported RAID or multi-device profile"
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
expect_open "checksum type 1 in a crc32c image" "$(corrupt csum-type:1 minimal)" "unsupported checksum type"
expect_open "checksum type 9" "$(corrupt csum-type:9 minimal)" "corrupt metadata"
expect_open "unreplayed log tree" "$(corrupt log-root minimal)" "unreplayed log tree"
expect_open "unreplayed log tree, told to ignore it" "$(corrupt log-root minimal)" "ok" --ignore-log
expect_open "nonsense nodesize" "$(corrupt nodesize:12345 minimal)" "corrupt metadata"
expect_open "nodesize larger than allowed" "$(corrupt nodesize:131072 minimal)" "corrupt metadata"
expect_open "claims two devices" "$(corrupt num-devices:2 minimal)" "unsupported RAID or multi-device profile"
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
expect_open "subvolume root block damaged (every copy)" "$(corrupt fs-block:all minimal)" "checksum mismatch"
expect_open "root tree damaged in the only copy (single metadata)" "$(corrupt root-block:first meta-single)" "checksum mismatch"
expect_walk_fails "walk with the tree root damaged" "$(corrupt fs-block:all tree)" "checksum mismatch"

# DUP metadata heals: only the first copy of each block is bad, the walk must still be right.
out=$("$HOST" walk "$(corrupt root-block:first tree)" 2>&1)
if printf '%s\n' "$out" | grep -q 'walk=ok' && printf '%s\n' "$out" | grep -qE 'mirror_fallbacks=[1-9]'; then pass "DUP heals a damaged root tree block (mirror fallback used)"; else fail "DUP did not heal" "$out"; fi
out=$("$HOST" walk "$(corrupt fs-block:first tree)" 2>&1)
if printf '%s\n' "$out" | grep -q 'walk=ok' && printf '%s\n' "$out" | grep -qE 'mirror_fallbacks=[1-9]'; then pass "DUP heals a damaged subvolume root block (mirror fallback used)"; else fail "DUP did not heal the subvolume root" "$out"; fi

echo "== corruption sweeps (seeded, ${ITERS} iterations x ${SEEDS} seeds) =="
for name in empty minimal n4k meta-single data-dup mixed no-holes-off subvols nodatasum comp-zlib; do
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

printf '\n%d check(s), %d failure(s)\n' "$count" "$failures"
[ "$failures" -eq 0 ]
