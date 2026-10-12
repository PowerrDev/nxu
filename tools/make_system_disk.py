#!/usr/bin/env python3
"""Combine ext4 system and recovery images into one named GPT SSD image."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import uuid
import zlib

SECTOR = 512
ALIGN = 2048
ENTRY_COUNT = 128
TABLE_SECTORS = 32
FILESYSTEM_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le


def align(value):
    return (value + ALIGN - 1) // ALIGN * ALIGN


def ensure_offline(path):
    if shutil.which("lsof"):
        result = subprocess.run(
            ["lsof", "-t", str(path.resolve())], capture_output=True
        )
        if result.stdout.strip():
            raise ValueError(
                f"image is open by another process; stop the VM first: {path}"
            )


def ext4(path):
    size = path.stat().st_size
    if size < 2048 or size % SECTOR:
        raise ValueError(f"not a sector-aligned filesystem image: {path}")
    with path.open("rb") as stream:
        stream.seek(1080)
        if stream.read(2) != b"\x53\xef":
            raise ValueError(f"not an ext4 filesystem: {path}")
    return size // SECTOR


def build(system, recovery, output):
    ensure_offline(system)
    ensure_offline(recovery)
    sizes = [ext4(system), ext4(recovery)]
    starts = [ALIGN, align(ALIGN + sizes[0])]
    sectors = align(starts[1] + sizes[1] + TABLE_SECTORS + 1)
    entries = bytearray(ENTRY_COUNT * 128)
    for index, name in enumerate(("NXU System", "NXU Recovery")):
        offset = index * 128
        entries[offset : offset + 16] = FILESYSTEM_GUID
        entries[offset + 16 : offset + 32] = uuid.uuid4().bytes_le
        struct.pack_into(
            "<QQQ",
            entries,
            offset + 32,
            starts[index],
            starts[index] + sizes[index] - 1,
            0,
        )
        encoded = name.encode("utf-16-le")
        entries[offset + 56 : offset + 56 + len(encoded)] = encoded
    disk_guid = uuid.uuid4().bytes_le

    def header(current, backup, table):
        data = bytearray(SECTOR)
        struct.pack_into(
            "<8sIIIIQQQQ16sQIII",
            data,
            0,
            b"EFI PART",
            0x10000,
            92,
            0,
            0,
            current,
            backup,
            34,
            sectors - 34,
            disk_guid,
            table,
            ENTRY_COUNT,
            128,
            zlib.crc32(entries),
        )
        struct.pack_into("<I", data, 16, zlib.crc32(data[:92]))
        return data

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        prefix=output.name + ".", dir=output.parent, delete=False
    ) as stream:
        temporary = Path(stream.name)
        try:
            stream.truncate(sectors * SECTOR)
            mbr = bytearray(SECTOR)
            mbr[450] = 0xEE
            struct.pack_into("<II", mbr, 454, 1, min(sectors - 1, 0xFFFFFFFF))
            mbr[510:512] = b"\x55\xaa"
            stream.write(mbr)
            stream.write(header(1, sectors - 1, 2))
            stream.write(entries)
            stream.seek((sectors - TABLE_SECTORS - 1) * SECTOR)
            stream.write(entries)
            stream.write(header(sectors - 1, 1, sectors - TABLE_SECTORS - 1))
            for source, start in zip((system, recovery), starts):
                stream.seek(start * SECTOR)
                with source.open("rb") as image:
                    shutil.copyfileobj(image, stream, 1024 * 1024)
            stream.flush()
            # Refuse races as well as accidental replacement of a live SSD.
            os.fsync(stream.fileno())
            os.link(temporary, output)
        finally:
            temporary.unlink(missing_ok=True)
    print(f"Created {output}: NXU System + NXU Recovery GPT partitions")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--system-image", type=Path, required=True)
    parser.add_argument("--recovery-image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("output already exists; existing SSDs are never replaced")
    try:
        build(args.system_image, args.recovery_image, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"make_system_disk: {error}\n")


if __name__ == "__main__":
    main()
