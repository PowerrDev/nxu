#!/usr/bin/env python3
"""Build a GPT/FAT32 NXU USB boot image using only Python's standard library."""

import argparse
import struct
import uuid
import zlib
from pathlib import Path

SECTOR = 512
START = 2048
SECTORS = 131072


def build_image(loader, kernel, bootargs):
    fat_sectors = 1
    while True:
        clusters = SECTORS - 32 - 2 * fat_sectors
        required = ((clusters + 2) * 4 + 511) // 512
        if required <= fat_sectors:
            break
        fat_sectors = required
    assert clusters >= 65525
    volume = bytearray(SECTORS * SECTOR)
    boot = bytearray(SECTOR)
    boot[:11] = b"\xeb\x58\x90NXUBOOT "
    struct.pack_into("<HBHBHHBHHHII", boot, 11, 512, 1, 32, 2, 0, 0, 0xF8, 0, 63, 255, START, SECTORS)
    struct.pack_into("<IHHIHH", boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into("<I", boot, 67, 0x4E585542)
    boot[71:82] = b"NXU BOOT   "
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xaa"
    volume[:512] = boot
    volume[6 * 512 : 7 * 512] = boot
    fsinfo = bytearray(512)
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<III", fsinfo, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)
    volume[512:1024] = fsinfo
    volume[7 * 512 : 8 * 512] = fsinfo
    fat = bytearray(fat_sectors * 512)
    struct.pack_into("<II", fat, 0, 0x0FFFFFF8, 0xFFFFFFFF)
    next_cluster = 2
    data_start = (32 + fat_sectors * 2) * 512

    def allocate(data):
        nonlocal next_cluster
        count = max(1, (len(data) + 511) // 512)
        first = next_cluster
        assert first + count <= clusters + 2, "Boot payload exceeds the 64 MiB ESP"
        for i in range(count):
            cluster = first + i
            struct.pack_into("<I", fat, cluster * 4, cluster + 1 if i + 1 < count else 0x0FFFFFFF)
        offset = data_start + (first - 2) * 512
        volume[offset : offset + len(data)] = data
        next_cluster += count
        return first

    root = allocate(b"")
    efi = allocate(b"")
    boot_dir = allocate(b"")
    nxu = allocate(b"")

    def entry(name, cluster, size=0, directory=False):
        result = bytearray(32)
        result[:11] = name
        result[11] = 0x10 if directory else 0x20
        struct.pack_into("<H", result, 20, cluster >> 16)
        struct.pack_into("<HI", result, 26, cluster & 0xFFFF, size)
        return result

    def directory(cluster, parent, entries):
        content = entry(b".          ", cluster, directory=True) + entry(b"..         ", parent, directory=True)
        content += b"".join(entries)
        assert len(content) < 512
        offset = data_start + (cluster - 2) * 512
        volume[offset : offset + len(content)] = content

    loader_cluster = allocate(loader)
    kernel_cluster = allocate(kernel)
    args = bootargs.encode() + b"\n"
    args_cluster = allocate(args)
    root_data = entry(b"EFI        ", efi, directory=True) + entry(b"NXU        ", nxu, directory=True)
    volume[data_start : data_start + len(root_data)] = root_data
    directory(efi, 0, [entry(b"BOOT       ", boot_dir, directory=True)])
    directory(boot_dir, efi, [entry(b"BOOTX64 EFI", loader_cluster, len(loader))])
    directory(
        nxu, 0, [entry(b"KERNEL  ELF", kernel_cluster, len(kernel)), entry(b"BOOTARGSTXT", args_cluster, len(args))]
    )
    for copy_index in range(2):
        offset = (32 + copy_index * fat_sectors) * 512
        volume[offset : offset + len(fat)] = fat
    # Publish accurate allocation hints in both FSInfo copies.
    struct.pack_into("<II", fsinfo, 488, clusters - (next_cluster - 2), next_cluster)
    volume[512:1024] = fsinfo
    volume[7 * 512 : 8 * 512] = fsinfo
    total = START + SECTORS + 33
    image = bytearray(total * 512)
    image[START * 512 : (START + SECTORS) * 512] = volume
    image[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, total - 1)
    image[510:512] = b"\x55\xaa"
    disk_guid = uuid.uuid4().bytes_le
    entries = bytearray(128 * 128)
    entries[:16] = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b").bytes_le
    entries[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", entries, 32, START, START + SECTORS - 1, 0)
    name = "NXU Boot".encode("utf-16le")
    entries[56 : 56 + len(name)] = name

    def header(current, backup, table):
        block = bytearray(512)
        struct.pack_into(
            "<8sIIIIQQQQ16sQIII",
            block,
            0,
            b"EFI PART",
            0x10000,
            92,
            0,
            0,
            current,
            backup,
            34,
            total - 34,
            disk_guid,
            table,
            128,
            128,
            zlib.crc32(entries),
        )
        struct.pack_into("<I", block, 16, zlib.crc32(block[:92]))
        return block

    image[512:1024] = header(1, total - 1, 2)
    image[1024 : 1024 + len(entries)] = entries
    image[(total - 33) * 512 : (total - 1) * 512] = entries
    image[(total - 1) * 512 :] = header(total - 1, 1, total - 33)
    return image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["loader", "kernel", "output"]:
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--bootargs", default="test=desktop ui.userland=1 virtio-irq=1")
    args = parser.parse_args()
    image = build_image(args.loader.read_bytes(), args.kernel.read_bytes(), args.bootargs)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"NxuBoot: GPT/FAT32 boot image: {args.output} ({len(image)//1024//1024} MiB)")


if __name__ == "__main__":
    main()
