#!/usr/bin/env bash
#
# tools/sign_boot_image.sh <image> <name> <manifest> -- a tepOS boot manifest
# for a staged image.
#
# Signs <image> as <name> with the host's boot-signing key, using the
# TrustedEnclaveProcessor checkout's tools/boot_sign (tepOS
# boot/include/tep/boot_manifest.h). The key lives in that checkout,
# build/boot-signing.key, and is made on first use together with the public
# key tepOS is built with (build/boot_pubkey.h), so the two always match.
# The key never enters this repository or the disk; only the manifest does.
#
# The manifest's version is the commit count (as in tools/version.sh), so it
# only moves forward along the history; tepOS reports an older bootd as a
# rollback. NXU_BOOT_VERSION overrides it.
#
# Without the tepOS checkout the manifest is removed and NXU reports bootd as
# unchecked; the disk build does not fail.
#
#   TEP_DIR=<path>   the TrustedEnclaveProcessor checkout (default: next to
#                    this repository's main checkout)

set -u

if [ $# -ne 3 ]; then
	echo "usage: $0 <image> <name> <manifest>" >&2
	exit 2
fi
image=$1
name=$2
manifest=$3

cd "$(dirname "$0")/.." || exit 1

common_dir=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
if [ -z "${TEP_DIR:-}" ] && [ -n "$common_dir" ]; then
	TEP_DIR=$(cd "$(dirname "$common_dir")/.." && pwd)/TrustedEnclaveProcessor
fi

rm -f "$manifest"

if [ -z "${TEP_DIR:-}" ] || [ ! -f "$TEP_DIR/tools/boot_sign.c" ]; then
	echo "sign_boot_image: no tepOS checkout (TEP_DIR); $name stays unsigned"
	exit 0
fi

# boot_sign and, the first time, the key pair. Without NXU's MAKEFLAGS: its
# command-line variables (EXTRA_CFLAGS, CONFIG, ...) are not tepOS's.
if ! env -u MAKEFLAGS -u MFLAGS -u MAKELEVEL "${MAKE:-make}" -s -C "$TEP_DIR" build/boot_pubkey.h >/dev/null 2>&1 </dev/null; then
	echo "sign_boot_image: could not build tepOS's boot_sign; $name stays unsigned"
	exit 0
fi

version=${NXU_BOOT_VERSION:-$(git rev-list --count HEAD 2>/dev/null || echo 1)}

if ! "$TEP_DIR/build/boot_sign" sign "$TEP_DIR/build/boot-signing.key" "$image" "$name" "$version" "$manifest" >/dev/null; then
	rm -f "$manifest"
	echo "sign_boot_image: signing $name failed; it stays unsigned"
	exit 0
fi

echo "sign_boot_image: $name signed for tepOS, version $version"
