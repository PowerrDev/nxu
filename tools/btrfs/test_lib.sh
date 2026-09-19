# Shared by tools/btrfs/test_i386.sh and tools/btrfs/test_arm64.sh (sourced; bash 3.2).
#
# The caller sets FIX (fixtures dir), SCRATCH, HOST (the btrfs_host tool),
# HERE (tools/btrfs) and defines arch_boot(), which boots the kernel with
# the images listed in $BT_IMAGES (space separated paths, in device order,
# after the root disk) and the boot argument btrfs-test=$BT_SPEC, and leaves
# the console output in $OUTPUT and the exit status in $STATUS.
#
# A group is a list of "token=image" pairs. token is a btrfs_selftest spec
# (see vfs/btrfs/btrfs_selftest.h); image is a fixture name, or
# "@KIND@NAME" for a damaged copy of fixture NAME made by corrupt.py.

failures=0
count=0
OUTPUT=
STATUS=0

pass() { count=$((count + 1)); printf 'ok    %s\n' "$1"; }
fail() {
	count=$((count + 1))
	failures=$((failures + 1))
	printf 'FAIL  %s\n' "$1"
	[ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/      | /' | tail -${3:-25}
}

mkdir -p "$SCRATCH/img" "$SCRATCH/bad"

fixture_image() {
	local name=$1
	if [ ! -f "$SCRATCH/img/$name.img" ] || [ "$FIX/$name.img.zst" -nt "$SCRATCH/img/$name.img" ]; then
		zstd -d -q --sparse -f "$FIX/$name.img.zst" -o "$SCRATCH/img/$name.img" || { echo "cannot decompress $name" >&2; exit 2; }
	fi
	printf '%s' "$SCRATCH/img/$name.img"
}

damaged_image() {
	local kind=$1 name=$2
	local out=$SCRATCH/bad/$name.$(printf '%s' "$kind" | tr ':/' '__').img
	"$HOST" info "$(fixture_image "$name")" > "$SCRATCH/bad/$name.info" 2>&1
	python3 "$HERE/corrupt.py" "$kind" "$(fixture_image "$name")" "$out" "$SCRATCH/bad/$name.info" || { echo "corrupt.py failed: $kind" >&2; exit 2; }
	printf '%s' "$out"
}

# resolve_image IMAGE -> path (a scratch copy: the guest may not change it)
resolve_image() {
	local image=$1 src copy
	case "$image" in
	@*)
		local rest=${image#@}
		local kind=${rest%%@*} name=${rest#*@}
		src=$(damaged_image "$kind" "$name")
		;;
	*)
		src=$(fixture_image "$image")
		;;
	esac
	copy=$SCRATCH/run.$BT_INDEX.img
	cp "$src" "$copy"
	printf '%s' "$copy"
}

# run_group NAME PAIR...
run_group() {
	local name=$1
	shift
	BT_SPEC=
	BT_IMAGES=
	BT_INDEX=1
	local hashes=
	local pair token image path

	for pair in "$@"; do
		token=${pair%%=*}
		image=${pair#*=}
		path=$(resolve_image "$image")
		BT_SPEC=${BT_SPEC:+$BT_SPEC,}$token
		BT_IMAGES="$BT_IMAGES $path"
		hashes="$hashes $(shasum -a 256 "$path" | cut -d' ' -f1)"
		BT_INDEX=$((BT_INDEX + 1))
	done

	arch_boot
	local rc=$STATUS

	local total=$#
	local passes bad
	passes=$(printf '%s\n' "$OUTPUT" | grep -c 'btrfs_selftest: PASS ')
	bad=$(printf '%s\n' "$OUTPUT" | grep -c 'btrfs_selftest: FAIL\|Unhandled\|PANIC\|panic')

	if printf '%s\n' "$OUTPUT" | grep -q 'btrfs_selftest: ALL PASSED' && [ "$passes" -eq "$total" ] && [ "$bad" -eq 0 ]; then
		pass "$name: $total fixture(s) [$BT_SPEC]"
	else
		fail "$name: expected $total PASS lines, saw $passes, $bad failure lines (status $rc) [$BT_SPEC]" "$OUTPUT" 40
	fi

	# The kernel never wrote to a fixture disk (the mounts are read-only).
	local index=0 changed=""
	for path in $BT_IMAGES; do
		index=$((index + 1))
		local before after
		before=$(printf '%s\n' $hashes | sed -n "${index}p")
		after=$(shasum -a 256 "$path" | cut -d' ' -f1)
		[ "$before" = "$after" ] || changed="$changed $index"
	done
	if [ -z "$changed" ]; then pass "$name: no fixture disk was modified"; else fail "$name: fixture disk(s) modified by the guest:$changed"; fi

	rm -f "$SCRATCH"/run.*.img
}

# The kernel's virtio-blk driver takes at most four devices: the ext4 root disk
# plus three fixtures per boot.
run_all_groups() {
	echo "== positive fixtures: tree walk against Linux's listing, refusal of every write =="
	run_group "positive-1" "n4k+verify=n4k" "tree+verify=tree" "subvols=subvols"
	run_group "positive-2" "subvols-default=subvols-default" "subvols-default@5=subvols-default" "subvols@258=subvols"
	run_group "positive-3" "comp-zlib=comp-zlib" "nodatasum=nodatasum" "no-holes-off=no-holes-off"
	run_group "positive-4" "n64k=n64k" "minimal=minimal" "n4k=n4k"

	echo "== unsupported and damaged images: clean refusal, kernel keeps running (a good mount follows) =="
	run_group "refusal-1" "!unsupported-csum=csum-xxhash" "!unsupported-profile=raid1" "n4k=n4k"
	run_group "refusal-2" "!unsupported-csum=csum-sha256" "!unsupported-profile=raid5" "minimal=minimal"
	run_group "refusal-3" "!bad-magic=@super-magic-all@minimal" "!csum=@super-csum-all@minimal" "n4k=n4k"
	run_group "refusal-4" "!truncated=@truncate:41943040@minimal" "!corrupt=@sys-array-garbage@minimal" "n4k=n4k"
	run_group "refusal-5" "!log-tree=@log-root@minimal" "!unsupported-feature=@incompat-bit:14@minimal" "minimal=minimal"
	run_group "refusal-6" "!csum=@root-block:all@minimal" "!corrupt=@nodesize:12345@minimal" "n4k=n4k"
	run_group "refusal-7" "!unsupported-csum=csum-blake2" "!unsupported-profile=single2dev" "!bad-magic=@truncate:1024@minimal"
}
