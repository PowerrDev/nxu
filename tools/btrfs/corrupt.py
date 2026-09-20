#!/usr/bin/env python3
"""Make a damaged copy of a good Btrfs image, for the negative tests.

usage: corrupt.py KIND SRC DST INFO

  INFO is the output of `btrfs_host info SRC` (it supplies the tree locations
  and the chunk mapping, so the damage lands on blocks that really matter).

Kinds (the damage is applied to DST, a copy of SRC):

  super-magic-all         zero the magic of every superblock copy
  super-csum-all          flip a bit in every superblock copy (bad checksum)
  super-primary           damage only the primary copy (a mirror is intact)
  incompat-bit:N          set incompat feature bit N in every copy (checksums fixed)
  csum-type:N             set the checksum type in every copy (checksums fixed)
  log-root                point log_root at a block in every copy (checksums fixed)
  nodesize:N              set a nonsense nodesize in every copy (checksums fixed)
  num-devices:N           claim N devices in every copy (checksums fixed)
  dev-total-bytes         claim a device twice as large (checksums fixed)
  sys-array-garbage       overwrite the start of sys_chunk_array (checksums fixed)
  root-block:all|first    flip a byte in the root tree's root block (all copies / one)
  chunk-block:all|first   the same for the chunk tree's root block
  fs-block:all|first      the same for the mounted subvolume's root block
  truncate:N              cut the image to N bytes
  data-extent:PATH:WHICH[:N]
                          flip a byte in the data of PATH's first regular extent
                          (WHICH: first = first copy only, second = second copy
                          only, all = every copy). N picks the Nth regular extent
                          (default 0). The info file needs `btrfs_host extents`
                          output for PATH appended (test_host.sh does that).
  nodatasum-off:PATH      clear the NODATASUM flag of PATH's inode (tree block
                          checksums fixed): a file that must have checksums but
                          has no csum items
"""

import os
import shutil
import struct
import sys

POLY = 0x82F63B78
TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (POLY if _c & 1 else 0)
    TABLE.append(_c)


def crc32c(data):
    c = 0xFFFFFFFF
    for b in data:
        c = TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


SB_OFFSETS = [0x10000, 64 << 20, 256 << 30]
SB_SIZE = 4096


def read_info(path):
    info = {"chunk_list": []}
    for line in open(path):
        line = line.strip()
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        if k == "chunk":
            f = v.split(",")
            info["chunk_list"].append((int(f[0]), int(f[1]), [int(x) for x in f[4:]]))
        elif k == "extent":
            info.setdefault("extents", []).append(v.split(","))
        else:
            info[k] = v
    return info


def physical(info, logical):
    for start, length, stripes in info["chunk_list"]:
        if start <= logical < start + length:
            return [s + (logical - start) for s in stripes]
    raise SystemExit("corrupt.py: logical %d is not mapped" % logical)


def sb_copies(f, size):
    for off in SB_OFFSETS:
        if off + SB_SIZE <= size:
            yield off


def edit_super(f, size, edit, fix=True):
    for off in sb_copies(f, size):
        f.seek(off)
        raw = bytearray(f.read(SB_SIZE))
        edit(raw)
        if fix:
            struct.pack_into("<I", raw, 0, crc32c(bytes(raw[32:])))
        f.seek(off)
        f.write(raw)


def flip_byte(f, offset, position=200):
    f.seek(offset + position)
    b = f.read(1)
    f.seek(offset + position)
    f.write(bytes([b[0] ^ 0x5A]))


def main():
    kind, src, dst, info_path = sys.argv[1:5]
    shutil.copyfile(src, dst)
    info = read_info(info_path)
    size = os.path.getsize(dst)
    name, _, arg = kind.partition(":")

    if name == "truncate":
        os.truncate(dst, int(arg))
        return

    with open(dst, "r+b") as f:
        if name == "super-magic-all":
            edit_super(f, size, lambda r: r.__setitem__(slice(64, 72), b"\0" * 8), fix=False)
        elif name == "super-csum-all":
            edit_super(f, size, lambda r: r.__setitem__(200, r[200] ^ 1), fix=False)
        elif name == "super-primary":
            f.seek(SB_OFFSETS[0] + 200)
            b = f.read(1)
            f.seek(SB_OFFSETS[0] + 200)
            f.write(bytes([b[0] ^ 1]))
        elif name == "incompat-bit":
            bit = int(arg)

            def edit(r):
                v = struct.unpack_from("<Q", r, 188)[0] | (1 << bit)
                struct.pack_into("<Q", r, 188, v)
            edit_super(f, size, edit)
        elif name == "csum-type":
            edit_super(f, size, lambda r: struct.pack_into("<H", r, 196, int(arg)))
        elif name == "log-root":
            edit_super(f, size, lambda r: struct.pack_into("<Q", r, 96, struct.unpack_from("<Q", r, 80)[0]))
        elif name == "nodesize":
            edit_super(f, size, lambda r: struct.pack_into("<I", r, 148, int(arg)))
        elif name == "num-devices":
            edit_super(f, size, lambda r: struct.pack_into("<Q", r, 136, int(arg)))
        elif name == "dev-total-bytes":
            def edit(r):
                v = struct.unpack_from("<Q", r, 201 + 8)[0]
                struct.pack_into("<Q", r, 201 + 8, v * 2)
            edit_super(f, size, edit)
        elif name == "sys-array-garbage":
            def edit(r):
                for i in range(811, 811 + 60):
                    r[i] = 0xEE
            edit_super(f, size, edit)
        elif name in ("root-block", "chunk-block", "fs-block"):
            key = {"root-block": "root", "chunk-block": "chunk_root", "fs-block": "mount_tree_bytenr"}[name]
            offsets = physical(info, int(info[key]))
            if arg == "first":
                offsets = offsets[:1]
            for off in offsets:
                flip_byte(f, off)
        elif name == "data-extent":
            parts = arg.split(":")
            path, which = parts[0], parts[1]
            nth = parts[2] if len(parts) > 2 else ""
            regular = [e for e in info.get("extents", []) if e[1] == "regular"]
            n = int(nth) if nth else 0
            if n >= len(regular):
                raise SystemExit("corrupt.py: %s has no regular extent %d" % (path, n))
            phys = [int(x) for x in regular[n][6:]]
            if which == "first":
                phys = phys[:1]
            elif which == "second":
                phys = phys[1:2]
            for off in phys:
                flip_byte(f, off, 100)
        elif name == "nodatasum-off":
            leaf, item = [int(x) for x in info["inode_leaf"].split(",")]
            nodesize = int(info["nodesize"])
            for off in physical(info, leaf):
                f.seek(off)
                block = bytearray(f.read(nodesize))
                flags = struct.unpack_from("<Q", block, item + 64)[0]
                struct.pack_into("<Q", block, item + 64, flags & ~1)
                struct.pack_into("<I", block, 0, crc32c(bytes(block[32:])))
                f.seek(off)
                f.write(block)
        else:
            raise SystemExit("corrupt.py: unknown kind " + kind)


if __name__ == "__main__":
    main()
