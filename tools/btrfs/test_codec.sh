#!/bin/bash
#
# Host tests of the compression decoders, run by tools/btrfs/test_host.sh (or on
# its own): zlib, LZO and ZSTD against streams made by real compressors, the
# limits (output cap, sector padding, truncation), and mutation sweeps under
# AddressSanitizer + UBSan.
#
# usage: test_codec.sh <btrfs_host binary> <scratch dir>
# Environment: BTRFS_CODEC_ITERS (default 600 per stream and seed)

set -u

HOST=${1:?usage: test_codec.sh <btrfs_host> <scratch dir>}
SCRATCH=${2:?usage: test_codec.sh <btrfs_host> <scratch dir>}
HERE=$(cd "$(dirname "$0")" && pwd)
ITERS=${BTRFS_CODEC_ITERS:-600}
C=$SCRATCH/codec

command -v zstd > /dev/null 2>&1 || { echo "test_codec: zstd not found"; exit 2; }
command -v python3 > /dev/null 2>&1 || { echo "test_codec: python3 not found"; exit 2; }

failures=0
count=0

pass() { count=$((count + 1)); printf 'ok    %s\n' "$1"; }
fail() { count=$((count + 1)); failures=$((failures + 1)); printf 'FAIL  %s\n' "$1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/      | /' | tail -8; }

rm -rf "$C"
python3 "$HERE/codec_corpus.py" "$C" || { echo "test_codec: corpus generation failed"; exit 2; }

# Btrfs LZO extents from the same plaintexts, two sector sizes (the framing depends on it).
for plain in "$C"/*.bin; do
	base=${plain%.bin}
	"$HOST" codec lzo "$plain" "$base.lzo.s4k" --sector 4096 --seed 1 || fail "lzo generator failed on $plain"
	"$HOST" codec lzo "$plain" "$base.lzo.s4kb" --sector 4096 --seed 7 || fail "lzo generator failed on $plain"
	"$HOST" codec lzo "$plain" "$base.lzo.s16k" --sector 16384 --seed 3 || fail "lzo generator failed on $plain"
done

# ---- known answers ----------------------------------------------------------------

out=$("$HOST" codec xxh64 2>&1)
if [ "$out" = "xxh64=ok" ]; then pass "xxh64 known-answer vectors"; else fail "xxh64 vectors" "$out"; fi

# ---- decode: every stream must give back exactly its plaintext -------------------------

echo "== decoders against real compressors' streams =="
for kind in zlib zstd lzo; do
	total=0 bad=0 badlist=""
	for stream in "$C"/*."$kind".*; do
		name=$(basename "$stream")
		plain=$C/${name%%.*}.bin
		sector=4096
		case "$name" in *.s16k) sector=16384 ;; esac
		out=$("$HOST" codec decode "$kind" "$stream" "$plain" --sector $sector 2>&1)
		total=$((total + 1))
		case "$out" in *"decode=ok match=yes leaked=0"*) ;; *) bad=$((bad + 1)); badlist="$badlist $name($out)" ;; esac
	done
	if [ "$bad" -eq 0 ] && [ "$total" -gt 0 ]; then pass "$kind: $total streams decode byte-identically"; else fail "$kind: $bad of $total streams did not decode" "$badlist"; fi
done

# ---- limits ---------------------------------------------------------------------------------

echo "== output cap, padding and truncation =="
for kind in zlib zstd lzo; do
	case $kind in lzo) stream=$C/odd.lzo.s4k ;; *) stream=$C/odd.$kind.l9 ;; esac
	plain=$C/odd.bin
	size=$(wc -c < "$plain" | tr -d ' ')

	# The decoder is told the extent is smaller than what the stream decodes to: the bomb limit.
	out=$("$HOST" codec decode $kind "$stream" "$plain" --outlen $((size - 1)) 2>&1)
	case "$out" in *"decode=corrupt metadata"*) pass "$kind: a stream that decodes past ram_bytes is refused" ;; *) fail "$kind: output cap not enforced" "$out" ;; esac

	# The extent is larger (sector padding): the tail reads as zeros.
	out=$("$HOST" codec decode $kind "$stream" "$plain" --pad 500 2>&1)
	case "$out" in *"decode=ok match=yes"*) pass "$kind: a stream shorter than ram_bytes is zero filled" ;; *) fail "$kind: padding" "$out" ;; esac

	# Cut the stream at several points: never OK.
	bad=""
	len=$(wc -c < "$stream" | tr -d ' ')
	cuts="0 1 2 3 5 $((len / 4)) $((len / 2))"
	# An LZO extent ends in sector padding: cutting only that is legitimate.
	[ "$kind" = lzo ] || cuts="$cuts $((len - 5)) $((len - 1))"
	for cut in $cuts; do
		if [ "$cut" -eq 0 ]; then : > "$C/cut.bin"; else head -c "$cut" "$stream" > "$C/cut.bin"; fi
		out=$("$HOST" codec decode $kind "$C/cut.bin" "$plain" 2>&1)
		case "$out" in *"decode=ok match=yes"*) bad="$bad $cut" ;; esac
	done
	if [ -z "$bad" ]; then pass "$kind: truncated streams never decode"; else fail "$kind: a truncated stream decoded (cut at$bad)"; fi
done

# A hostile stream that is tiny but claims a huge result: 200 bytes of RLE blocks would
# produce far more than the extent holds.
python3 - "$C" <<'EOF'
import sys, struct, zlib
c = sys.argv[1]
# zlib: 1 MiB of zeros compresses to ~1 KiB.
open(c + "/bomb.zlib", "wb").write(zlib.compress(b"\0" * (1 << 20), 9))
# zstd: RLE blocks, each block header says 128 KiB of one byte (3-byte header + 1 byte payload).
frame = bytearray(b"\x28\xb5\x2f\xfd")
frame += bytes([0x00, 0x58])           # no single segment, window descriptor: 128 KiB window
for i in range(64):
    last = 1 if i == 63 else 0
    hdr = (131072 << 3) | (1 << 1) | last
    frame += struct.pack("<I", hdr)[:3] + b"\x41"
open(c + "/bomb.zstd", "wb").write(bytes(frame))
open(c + "/bomb.bin", "wb").write(b"")
EOF
for kind in zlib zstd; do
	out=$("$HOST" codec decode $kind "$C/bomb.$kind" "$C/bomb.bin" --outlen 131072 2>&1)
	case "$out" in *"decode=corrupt metadata"*) pass "$kind: a decompression bomb into a 128 KiB extent is refused" ;; *) fail "$kind: bomb not refused" "$out" ;; esac
done

# ---- mutation sweeps --------------------------------------------------------------------------

echo "== mutation sweeps: only errors or the original bytes, no crash, hang, leak or out-of-bounds access =="
# stream (variant) and whether damage that still parses is caught by a checksum in the format.
fuzz_one() {
	local kind=$1 name=$2 variant=$3 stream extra=""
	stream=$C/$name.$kind.$variant
	case $variant in nocheck|stored|fixed|s4kb) ;; esac
	if [ "$kind" = zlib ] || { [ "$kind" = zstd ] && [ "$variant" != nocheck ] && [ "$variant" != btrfs ]; }; then extra="--integrity"; fi
	local out rc
	out=$("$HOST" codec fuzz $kind "$stream" "$C/$name.bin" --seed 1 --iters "$ITERS" $extra 2>&1)
	rc=$?
	if [ $rc -eq 0 ]; then pass "fuzz $kind $name.$variant: $(printf '%s' "$out" | sed 's/fuzz=[a-z]* //')"; else fail "fuzz $kind $name.$variant (rc $rc)" "$out"; fi
}

for name in prose mixed random tiny; do
	for variant in l6 fixed stored; do fuzz_one zlib $name $variant; done
	for variant in l3 nocheck l19; do fuzz_one zstd $name $variant; done
	fuzz_one lzo $name s4kb
done

printf '\n%d codec check(s), %d failure(s)\n' "$count" "$failures"
[ "$failures" -eq 0 ]
