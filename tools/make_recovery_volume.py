#!/usr/bin/env python3
"""Build an ext4 recovery partition with a standalone triageOS and update storage.

With --factory-image, the partition also carries the factory payload that
"Erase All Content and Settings" reinstalls: every file and directory of that
system image under System/Recovery/Factory/Root, a FactoryManifest listing
each one (type, mode, owner, size, SHA-256, path), FactoryManifest.sha256, and
the image's own patchd at System/Recovery/patchd, which the kernel runs from
this volume to do the erase. See doc/erase-and-startup-disk.md.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from make_system_disk import ensure_offline

E2FS_PATHS = ("/usr/local/opt/e2fsprogs/sbin", "/opt/homebrew/opt/e2fsprogs/sbin")
FACTORY_RELATIVE_MAX = 200
# Request and status files of the running system are never part of a factory install.
FACTORY_EXCLUDED = {
    "lost+found",
    "var/db/patchd/erase-request",
    "var/db/patchd/erase-status",
    "var/db/patchd/startup-disk-request",
    "var/db/patchd/repair-all",
    "var/db/patchd/prepare-update",
    "var/db/patchd/install-update",
    "var/db/patchd/update-status",
    "var/db/patchd/update-active",
}


def e2fs_environment():
    environment = dict(os.environ)
    extra = [path for path in E2FS_PATHS if os.path.isdir(path)]
    environment["PATH"] = os.pathsep.join(extra + [environment.get("PATH", "")])
    return environment


def debugfs_listing(image, directories, environment):
    """Run `ls -p` for every directory in one debugfs session; map dir -> entries."""
    script = "".join('ls -p "/%s"\n' % directory for directory in directories)
    result = subprocess.run(
        ["debugfs", "-f", "-", str(image)],
        input=script.encode(),
        capture_output=True,
        check=True,
        env=environment,
    )
    listing = {}
    current = None
    for raw in result.stdout.decode("utf-8", "surrogateescape").splitlines():
        if raw.startswith("debugfs: ls -p "):
            current = raw[len('debugfs: ls -p "/') : -1]
            listing[current] = []
            continue
        if current is None or not raw.startswith("/"):
            continue
        fields = raw.split("/")
        # /inode/mode/uid/gid/name/size/ ; a name never contains '/'.
        if len(fields) != 8:
            raise ValueError("unexpected debugfs listing line: " + raw)
        _, inode, mode, uid, gid, name, size, _ = fields
        if name in (".", ".."):
            continue
        listing[current].append(
            (name, int(mode, 8), int(uid), int(gid), int(size) if size else 0)
        )
    missing = [directory for directory in directories if directory not in listing]
    if missing:
        raise ValueError("debugfs did not list: " + ", ".join(missing))
    return listing


def build_factory(image, root, scratch):
    """Extract image into root/System/Recovery/Factory and write its manifest."""
    environment = e2fs_environment()
    extract = scratch / "factory-extract"
    extract.mkdir()
    subprocess.run(
        ["debugfs", "-R", f'rdump / "{extract}"', str(image)],
        check=True,
        capture_output=True,
        env=environment,
    )
    factory = root / "System/Recovery/Factory"
    files_root = factory / "Root"
    files_root.mkdir(parents=True)
    entries = []
    pending = [""]
    while pending:
        listing = debugfs_listing(image, pending, environment)
        next_level = []
        for directory in pending:
            for name, mode, uid, gid, size in sorted(listing[directory]):
                relative = f"{directory}/{name}" if directory else name
                if relative in FACTORY_EXCLUDED:
                    continue
                if (
                    len(relative) > FACTORY_RELATIVE_MAX
                    or any(ord(c) < 32 or ord(c) > 126 for c in relative)
                ):
                    raise ValueError("unsupported factory path: " + repr(relative))
                kind = mode & 0o170000
                permissions = mode & 0o7777
                source = extract / relative
                target = files_root / relative
                if kind == 0o040000:
                    target.mkdir()
                    entries.append(f"d {permissions:04o} {uid} {gid} 0 - {relative}")
                    next_level.append(relative)
                elif kind == 0o100000:
                    data = source.read_bytes()
                    if len(data) != size:
                        raise ValueError(f"extracted size mismatch for {relative}")
                    target.write_bytes(data)
                    digest = hashlib.sha256(data).hexdigest()
                    entries.append(
                        f"f {permissions:04o} {uid} {gid} {size} {digest} {relative}"
                    )
                else:
                    raise ValueError(f"unsupported file type {kind:o} for {relative}")
        pending = next_level
    manifest = "NXUFactoryManifest 1\n" + "".join(e + "\n" for e in entries)
    manifest += f"end {len(entries)}\n"
    data = manifest.encode("ascii")
    (factory / "FactoryManifest").write_bytes(data)
    (factory / "FactoryManifest.sha256").write_text(hashlib.sha256(data).hexdigest() + "\n")
    eraser = files_root / "System/Library/CoreServices/patchd"
    if not eraser.is_file() or eraser.read_bytes()[:4] != b"\x7fELF":
        raise ValueError("factory image has no patchd to run the erase")
    shutil.copyfile(eraser, root / "System/Recovery/patchd")
    total = sum(int(e.split(" ")[4]) for e in entries)
    print(f"Factory payload: {len(entries)} entries, {total} bytes from {image}")


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
    parser.add_argument(
        "--factory-image",
        type=Path,
        help="system image to carry as the Erase All Content and Settings payload",
    )
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
        if args.factory_image:
            factory = root / "System/Recovery/Factory"
            if factory.exists():
                shutil.rmtree(factory)
            try:
                ensure_offline(args.factory_image)
                build_factory(args.factory_image, root, scratch)
            except (ValueError, subprocess.CalledProcessError) as error:
                parser.error(f"factory payload: {error}")
        # The startup disk choice always starts as the system volume.
        (root / "System/StartupDisk").write_text("mainOS\n")
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
