#!/usr/bin/env python3
"""Build an ext4 recovery partition with a standalone triageOS and update storage."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from make_system_disk import ensure_offline


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ramdisk-image", type=Path, required=True)
    parser.add_argument("--seed-image", type=Path)
    parser.add_argument(
        "--recovery-program",
        type=Path,
        help="use the current standalone triageOS binary",
    )
    parser.add_argument("--package", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-mib", type=int, default=512)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; recovery volumes are never replaced")
    if args.size_mib < 64 or args.size_mib > 4096:
        parser.error("recovery size must be between 64 and 4096 MiB")
    if args.seed_image:
        try:
            ensure_offline(args.seed_image)
        except ValueError as error:
            parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="nxu-recovery-volume-", dir=args.output.parent
    ) as temporary:
        scratch = Path(temporary)
        root = scratch / "root"
        root.mkdir()
        if args.seed_image:
            subprocess.run(
                ["debugfs", "-R", f'rdump / "{root}"', str(args.seed_image)], check=True
            )
            # Do not transplant an active transaction onto a different system snapshot.
            if (root / "InstallPending").exists() or (root / "InstallReady").exists():
                parser.error(
                    "seed recovery volume has a pending update; finish or roll it back first"
                )
        programs = root / "System/Recovery"
        programs.mkdir(parents=True, exist_ok=True)
        program = programs / "triageOS"
        if args.recovery_program:
            shutil.copyfile(args.recovery_program, program)
        else:
            subprocess.run(
                [
                    "debugfs",
                    "-R",
                    f'dump /System/Recovery/triageOS "{program}"',
                    str(args.ramdisk_image),
                ],
                check=True,
            )
        if not program.is_file() or program.stat().st_size < 52:
            parser.error("recovery ramdisk does not contain triageOS")
        with program.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                parser.error("recovery ramdisk does not contain a valid triageOS ELF")
        shutil.copyfile(args.ramdisk_image, programs / "RecoveryRamdisk.img")
        packages = root / "mainOS"
        packages.mkdir(exist_ok=True)
        for package in args.package:
            if package.suffix != ".nsw":
                parser.error("update packages must have the .nsw extension")
            target = packages / package.name
            if target.exists():
                parser.error("duplicate package filename: " + package.name)
            shutil.copyfile(package, target)
        (root / "RecoveryVolumeVersion").write_text("1\n")
        image = scratch / "volume.img"
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
                "-L",
                "NXU Recovery",
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
        with image.open("rb") as stream:
            os.fsync(stream.fileno())
        os.link(image, args.output)
    print(f"Created recovery partition image {args.output}")


if __name__ == "__main__":
    main()
