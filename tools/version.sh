#!/bin/bash
#
# Derive the NXU version and build strings from the number of commits, so they
# move forward with the history instead of being edited by hand and forgotten.
#
#   N       commits reachable from HEAD (git rev-list --count HEAD)
#   gen     N / 100 + 1         commits 1-99 are generation 1, 100-199 are 2, ...
#   r       N % 100             position inside the generation
#   version 0.<gen>.<r>-dev
#   build   NXU-<gen><letter><digit>A
#           letter = 'A' + r / 10, digit = r % 10 (so r = 87 is "I7"); the
#           trailing A marks a development build
#
# 187 commits therefore give 0.2.87-dev and NXU-2I7A.
#
#   tools/version.sh           print both
#   tools/version.sh --write   rewrite NXU_VERSION and NXU_BUILD in
#                              kern/logging/version.h (`make version`)
#
# Plain bash 3.2 (what macOS ships).

set -eu

cd "$(dirname "$0")/.."

count=$(git rev-list --count HEAD)
generation=$((count / 100 + 1))
position=$((count % 100))
letter=$(printf "\\$(printf '%03o' $((65 + position / 10)))")
digit=$((position % 10))

version="0.${generation}.${position}-dev"
build="NXU-${generation}${letter}${digit}A"

if [ "${1:-}" = "--write" ]; then
	header=kern/logging/version.h
	perl -pi -e "s/^#define NXU_VERSION .*/#define NXU_VERSION \"$version\"/; s/^#define NXU_BUILD .*/#define NXU_BUILD \"$build\"/" "$header"
	echo "version: $header now says $version ($build), from $count commits"
else
	echo "$version $build ($count commits)"
fi
