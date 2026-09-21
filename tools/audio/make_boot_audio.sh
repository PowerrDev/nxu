#!/bin/bash
#
# Behind `make boot-audio`: decode the boot chime to a WAV the kernel can play.
#
# The kernel has no MP3 decoder (and writing one is out of scope), so the
# chime ships twice, side by side in the system volume:
#
#   Boot_Audio.mp3   the original, MPEG-1 Layer III, 44.1 kHz stereo 128 kbps
#   Boot_Audio.wav   16-bit little-endian stereo PCM decoded from it, which is
#                    what the kernel reads (kern/boot/boot_chime.c)
#
# macOS's afconvert does the decoding; run this whenever the MP3 changes and
# commit the WAV. The result is checked with `file` and with the kernel's own
# WAV reader (tools/audio/host/wavinfo.c) so a file the boot could not play is
# caught here.
#
# usage: make_boot_audio.sh [directory holding Boot_Audio.mp3]
# Plain bash 3.2 (what macOS ships).

set -eu

HERE=$(dirname "$0")
ROOT=$HERE/../..
DIR=${1:-$ROOT/tools/DiskRoot/System/Library/Resources/Audio}
MP3=$DIR/Boot_Audio.mp3
WAV=$DIR/Boot_Audio.wav

command -v afconvert > /dev/null 2>&1 || { echo "make_boot_audio: afconvert not found (this step needs macOS)"; exit 1; }
[ -f "$MP3" ] || { echo "make_boot_audio: $MP3 not found"; exit 1; }

echo "make_boot_audio: $(file -b "$MP3")"

# LEI16: little-endian signed 16-bit integers; the rate and channel count of the source are kept.
afconvert -f WAVE -d LEI16 "$MP3" "$WAV"

echo "make_boot_audio: $(file -b "$WAV")"

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/wavinfo.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT

${AUDIO_HOST_CC:-cc} -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror -I"$ROOT" \
	"$HERE/host/wavinfo.c" "$ROOT/libk/wav.c" -o "$SCRATCH/wavinfo"

"$SCRATCH/wavinfo" "$WAV"
