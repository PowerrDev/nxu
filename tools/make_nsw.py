#!/usr/bin/env python3
"""Create an NSW ISO/Joliet package with raw ext4 system and recovery images."""

import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True)
    parser.add_argument("--arch", choices=["arm64", "i386"], required=True)
    parser.add_argument("--system-image", type=Path, required=True)
    parser.add_argument("--recovery-ramdisk", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9_.-]{0,39}", args.build):
        parser.error("build must be a safe identifier of at most 40 ASCII characters")
    if args.build.lower() == "recoveryramdisk":
        parser.error("build identifier collides with the recovery payload filename")
    if args.output.exists():
        parser.error("output already exists; choose a new path")

    for path in (args.system_image, args.recovery_ramdisk):
        if not path.is_file() or path.stat().st_size < 2048:
            parser.error(f"not an ext4 image: {path}")
        with path.open("rb") as stream:
            stream.seek(1080)
            if stream.read(2) != b"\x53\xef":
                parser.error(f"not an ext4 image: {path}")
        if path.stat().st_size > 0xFFFFFFFF:
            parser.error("NSW version 1 does not support images over 4 GiB")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nxu-nsw-") as temporary:
        scratch = Path(temporary)
        root = scratch / "root"
        contents = root / ".nsw"
        contents.mkdir(parents=True)
        manifest = {"FormatVersion": 1, "Build": args.build, "Architecture": args.arch}
        for key, source, name in (
            ("SystemImage", args.system_image, args.build + ".img"),
            ("RecoveryRamdisk", args.recovery_ramdisk, "RecoveryRamdisk.img"),
        ):
            target = contents / name
            shutil.copyfile(source, target)
            with target.open("rb") as stream:
                manifest[key + "SHA256"] = hashlib.file_digest(
                    stream, "sha256"
                ).hexdigest()
            manifest[key] = name
            manifest[key + "Format"] = "ext4"
        (contents / "BuildManifest.plist").write_bytes(
            plistlib.dumps(manifest, sort_keys=False)
        )
        image = scratch / "update.iso"
        if shutil.which("hdiutil"):
            command = [
                "hdiutil",
                "makehybrid",
                "-iso",
                "-joliet",
                "-o",
                str(image),
                str(root),
            ]
        elif shutil.which("xorriso"):
            command = ["xorriso", "-as", "mkisofs", "-J", "-o", str(image), str(root)]
        else:
            parser.error("install xorriso, or run on macOS with hdiutil")
        subprocess.run(command, check=True)
        if image.stat().st_size > 0xFFFFFFFF:
            parser.error("NSW version 1 package exceeds the 4 GiB limit")
        shutil.copyfile(image, args.output)
    print(f"Created {args.output}")


if __name__ == "__main__":
    main()
