#!/usr/bin/env python3
"""Build a raw ext4 recovery filesystem containing NXU's triageOS and kernel."""

import argparse
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=["arm64", "i386"], required=True)
    parser.add_argument("--build-root", type=Path, default=Path("BUILD"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=32)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; choose a new path")
    if not 16 <= args.size_mib <= 1024:
        parser.error("size must be between 16 and 1024 MiB")
    if args.build_root.is_absolute():
        try:
            args.build_root = args.build_root.resolve().relative_to(ROOT)
        except ValueError:
            if " " in str(args.build_root):
                parser.error("external build roots must not contain spaces")
    i386 = args.arch == "i386"
    user = args.build_root / ("i386/user" if i386 else "userland")
    kernel = args.build_root / ("i386/kernel.elf" if i386 else "default/kernel.bin")
    subprocess.run(
        [
            "make",
            "-j8",
            "BUILD_ROOT=" + str(args.build_root),
            "i386" if i386 else "all",
            str(user / "triageOS"),
        ],
        cwd=ROOT,
        check=True,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nxu-recovery-image-") as temporary:
        scratch = Path(temporary)
        root = scratch / "root"
        recovery = root / "System/Recovery"
        recovery.mkdir(parents=True)
        shutil.copyfile(ROOT / user / "triageOS", recovery / "triageOS")
        kernels = root / "System/Library/Kernels"
        kernels.mkdir(parents=True)
        shutil.copyfile(ROOT / kernel, kernels / kernel.name)
        (recovery / "RecoveryManifest.plist").write_bytes(
            plistlib.dumps(
                {
                    "FormatVersion": 1,
                    "Architecture": args.arch,
                    "RecoveryProgram": "/System/Recovery/triageOS",
                    "Kernel": "/System/Library/Kernels/" + kernel.name,
                }
            )
        )
        image = scratch / "recovery.img"
        with image.open("wb") as stream:
            stream.truncate(args.size_mib * 1024 * 1024)
        subprocess.run(
            [
                "mkfs.ext4",
                "-q",
                "-F",
                "-b",
                "4096",
                "-I",
                "256",
                "-O",
                "has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit",
                "-E",
                "lazy_itable_init=0,lazy_journal_init=0",
                "-d",
                str(root),
                str(image),
            ],
            check=True,
        )
        shutil.copyfile(image, args.output)
    print(
        f"Created {args.output}; boot selection and memory-backed mounting are separate steps"
    )


if __name__ == "__main__":
    main()
