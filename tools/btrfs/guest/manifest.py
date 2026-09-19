#!/usr/bin/env python3
"""Write the ground-truth MANIFEST of a mounted (or staged) directory tree.

Runs inside the Alpine guest, against a filesystem the real Linux Btrfs driver
mounted, so the manifest is Linux's view of the image, not ours.

usage: manifest.py ROOT [--flags FILE] [--no-ino] [--no-times]

One tab separated record per path, sorted by path bytes:

    path type mode uid gid size nlink ino rdev mtime crc32c sha256 target flag

  path    "/" for ROOT itself, else "/a/b" (raw bytes, UTF-8 as created)
  type    f regular, d directory, l symlink, c char dev, b block dev,
          p fifo, s socket
  mode    st_mode in octal (type bits included)
  size    st_size (for a directory: what Btrfs reports, the sum of the name
          lengths times two)
  ino     st_ino ("-" with --no-ino)
  rdev    "major:minor" for devices, else "-"
  mtime   st_mtime seconds ("-" with --no-times)
  crc32c  8 hex digits, the standard CRC-32C (init ~0, final xor ~0) of the
          contents of a regular file, "-" otherwise
  sha256  hex digest of a regular file, "-" otherwise
  target  the symlink target, "-" otherwise
  flag    "-" or a marker from --flags (a file of "path<TAB>flag" lines),
          e.g. "compressed" for files the driver must refuse to read

The first line is a comment naming the format.
"""

import hashlib
import os
import stat
import sys

POLY = 0x82F63B78
TABLE = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ (POLY if c & 1 else 0)
    TABLE.append(c)


def crc32c(data):
    c = 0xFFFFFFFF
    t = TABLE
    for b in data:
        c = t[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def main():
    args = sys.argv[1:]
    root = args.pop(0)
    flags = {}
    no_ino = False
    no_times = False
    while args:
        a = args.pop(0)
        if a == "--flags":
            with open(args.pop(0), "rb") as f:
                for line in f.read().split(b"\n"):
                    if line:
                        p, fl = line.split(b"\t")
                        flags[p] = fl
        elif a == "--no-ino":
            no_ino = True
        elif a == "--no-times":
            no_times = True
        else:
            sys.exit("manifest.py: bad argument " + a)

    rootb = os.fsencode(root)
    records = []

    def record(path_b, rel):
        st = os.lstat(path_b)
        m = st.st_mode
        crc = b"-"
        sha = b"-"
        target = b"-"
        rdev = b"-"
        if stat.S_ISREG(m):
            t = b"f"
            with open(path_b, "rb") as fh:
                data = fh.read()
            crc = b"%08x" % crc32c(data)
            sha = hashlib.sha256(data).hexdigest().encode()
        elif stat.S_ISDIR(m):
            t = b"d"
        elif stat.S_ISLNK(m):
            t = b"l"
            target = os.readlink(path_b)
        elif stat.S_ISCHR(m):
            t = b"c"
            rdev = b"%d:%d" % (os.major(st.st_rdev), os.minor(st.st_rdev))
        elif stat.S_ISBLK(m):
            t = b"b"
            rdev = b"%d:%d" % (os.major(st.st_rdev), os.minor(st.st_rdev))
        elif stat.S_ISFIFO(m):
            t = b"p"
        elif stat.S_ISSOCK(m):
            t = b"s"
        else:
            sys.exit("manifest.py: unknown file type at " + repr(path_b))
        fields = [
            rel, t, b"%o" % m, b"%d" % st.st_uid, b"%d" % st.st_gid,
            b"%d" % st.st_size, b"%d" % st.st_nlink,
            b"-" if no_ino else b"%d" % st.st_ino, rdev,
            b"-" if no_times else b"%d" % st.st_mtime, crc, sha, target,
            flags.get(rel, b"-"),
        ]
        for f in fields:
            if b"\t" in f or b"\n" in f:
                sys.exit("manifest.py: tab/newline in " + repr(f))
        records.append(fields)

    def walk(path_b, rel):
        record(path_b, rel)
        if stat.S_ISDIR(os.lstat(path_b).st_mode):
            for name in os.listdir(path_b):
                walk(os.path.join(path_b, name), (rel if rel != b"/" else b"") + b"/" + name)

    walk(rootb, b"/")
    records.sort(key=lambda r: r[0])
    out = sys.stdout.buffer
    out.write(b"# nxu-btrfs-manifest 1\n")
    for r in records:
        out.write(b"\t".join(r) + b"\n")


if __name__ == "__main__":
    main()
