#!/usr/bin/env python3
"""Populate a mounted Btrfs (or a staging directory) with a named recipe.

Runs inside the Alpine guest on the mount of a filesystem the real Linux Btrfs
driver serves, so the on-disk shapes (inline and regular extents, partially
referenced extents, PREALLOC, holes, hard links, ...) are whatever Linux
produces, not what our own driver would expect.

usage: pop.py RECIPE ROOT [FLAGS_FILE]

Recipes: empty minimal tree deep small compressible compress_mix compress_random logbase logops
Data is deterministic and position dependent (a file read at the wrong offset
can never hash equal) yet highly compressible, so the images stay small.
"""

import os
import socket
import struct
import sys


def pat(seed, off, n):
    """n bytes of the file-position dependent pattern starting at offset off."""
    out = bytearray()
    pos = off
    end = off + n
    while pos < end:
        blk = pos // 256
        chunk = struct.pack("<QQQQ", blk, seed * 0x9E3779B97F4A7C15 & (2**64 - 1), blk ^ seed, 0xC0FFEE)
        chunk = (chunk * 8)[:256]
        lo = pos % 256
        take = min(256 - lo, end - pos)
        out += chunk[lo:lo + take]
        pos += take
    return bytes(out)


def put(path, data, mode=0o644):
    with open(path, "wb") as f:
        f.write(data)
    os.chmod(path, mode)


def pwrite_sync(path, off, data, create=False):
    flags = os.O_WRONLY | (os.O_CREAT if create else 0)
    fd = os.open(path, flags, 0o644)
    try:
        os.pwrite(fd, data, off)
        os.fsync(fd)
    finally:
        os.close(fd)


def name_hash_collisions(count_pairs):
    """Find pairs of names with the same Btrfs directory-entry hash.

    The DIR_ITEM key is crc32c(~1, name); two names with equal hashes end up
    packed into a single DIR_ITEM. Brute force over short names (birthday
    bound: about 2**16 names for a collision).
    """
    poly = 0x82F63B78
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (poly if c & 1 else 0)
        table.append(c)

    def h(name):
        c = 0xFFFFFFFE
        for b in name:
            c = table[(c ^ b) & 0xFF] ^ (c >> 8)
        return c

    seen = {}
    pairs = []
    i = 0
    while len(pairs) < count_pairs:
        name = ("c%x" % (i * 2654435761 % 0xFFFFFFFF)).encode()
        i += 1
        k = h(name)
        if k in seen and seen[k] != name:
            pairs.append((seen[k], name))
            del seen[k]
        else:
            seen[k] = name
    return pairs


def small(root):
    """The small population shared by the format-variant fixtures."""
    os.makedirs(root + "/dir/sub")
    put(root + "/hello.txt", b"hello btrfs\n")
    put(root + "/empty", b"")
    put(root + "/dir/sub/data.bin", pat(1, 0, 200 * 1024))
    put(root + "/dir/inline.txt", pat(2, 0, 700))
    put(root + "/dir/three-k.bin", pat(3, 0, 3000))
    os.link(root + "/hello.txt", root + "/dir/hello-link.txt")
    os.symlink("hello.txt", root + "/link-short")
    os.symlink("dir/sub/data.bin", root + "/dir/link-rel")
    # A hole in the middle, data at both ends.
    with open(root + "/sparse", "wb") as f:
        f.truncate(1024 * 1024)
    pwrite_sync(root + "/sparse", 0, pat(4, 0, 4096))
    pwrite_sync(root + "/sparse", 900 * 1024, pat(4, 900 * 1024, 8192))
    pwrite_sync(root + "/sparse", 1024 * 1024 - 100, pat(4, 1024 * 1024 - 100, 100))
    os.mkfifo(root + "/fifo")


def empty(root):
    pass


def minimal(root):
    put(root + "/small.txt", b"hello from a real btrfs\n")
    put(root + "/empty", b"")


def tree(root):
    # Nested directories with a file at each level.
    p = root + "/a"
    depth = ["a", "b", "c", "d", "e", "f", "g"]
    p = root
    for i, d in enumerate(depth):
        p += "/" + d
        os.mkdir(p)
        put(p + "/level%d.txt" % i, ("level %d\n" % i).encode())

    # A big directory: many DIR_ITEM/DIR_INDEX items spanning several leaves.
    os.mkdir(root + "/big")
    for i in range(2500):
        put(root + "/big/f%04d" % i, b"n%d\n" % i)

    # Colliding directory-entry hashes: several names in one DIR_ITEM.
    os.mkdir(root + "/collide")
    for pair in name_hash_collisions(2):
        for nm in pair:
            put(root + "/collide/" + nm.decode(), b"collide " + nm + b"\n")

    # Names.
    os.mkdir(root + "/names")
    put(root + "/names/" + "x" * 255, b"255 bytes\n")
    put(root + "/names/" + "é" * 127 + "x", b"255 bytes of utf-8\n")
    put(root + "/names/héllo wörld", b"accents and a space\n")
    put(root + "/names/日本語のファイル.txt", b"cjk\n")
    put(root + "/names/\U0001F600.txt", b"emoji\n")
    put(root + "/names/.hidden", b"dot\n")
    os.mkdir(root + "/names/dir with space")
    put(root + "/names/dir with space/in", b"in\n")
    put(root + "/names/a", b"short a\n")
    put(root + "/names/b", b"short b\n")

    # Links.
    os.mkdir(root + "/links")
    os.mkdir(root + "/links/sub")
    put(root + "/links/orig", pat(5, 0, 5000))
    os.link(root + "/links/orig", root + "/links/hard1")
    os.link(root + "/links/orig", root + "/links/hard2")
    os.link(root + "/links/orig", root + "/links/sub/hard3")
    os.mkdir(root + "/sl")
    os.symlink("../links/orig", root + "/sl/short")
    os.symlink("/absolute/dangling/target", root + "/sl/dangling")
    os.symlink("../links", root + "/sl/to-dir")
    long_target = "/".join("segment%03d" % i for i in range(250))  # 2749 bytes
    os.symlink(long_target, root + "/sl/long")
    os.symlink("l" * 4000, root + "/sl/longer")

    # Special files.
    os.mkdir(root + "/dev")
    os.mkfifo(root + "/dev/fifo")
    os.mknod(root + "/dev/null", 0o20666, os.makedev(1, 3))
    os.mknod(root + "/dev/loop7", 0o60660, os.makedev(7, 7))
    s = socket.socket(socket.AF_UNIX)
    s.bind(root + "/dev/sock")
    s.close()

    # Permissions and owners.
    os.mkdir(root + "/perm")
    put(root + "/perm/suid", b"x\n", 0o4755)
    put(root + "/perm/private", b"x\n", 0o600)
    os.chown(root + "/perm/private", 1000, 1001)
    os.mkdir(root + "/perm/sticky")
    os.chmod(root + "/perm/sticky", 0o1777)
    os.utime(root + "/perm/private", (1000000000, 1234567890))
    os.setxattr(root + "/perm/private", "user.nxu", b"an xattr that must be skipped")
    os.setxattr(root + "/perm", "user.dir", b"xattr on a directory")

    # File data.
    os.mkdir(root + "/data")
    d = root + "/data"
    put(d + "/tiny", b"hello\n")
    put(d + "/inline2000", pat(6, 0, 2000))
    put(d + "/regular3000", pat(7, 0, 3000))   # a whole sector holding 3000 bytes
    put(d + "/one-extent", pat(8, 0, 3 * 1024 * 1024))
    put(d + "/empty", b"")

    # Sparse: implicit gaps between extents.
    with open(d + "/sparse", "wb") as f:
        f.truncate(10 * 1024 * 1024)
    for off, n in ((0, 4096), (1024 * 1024 + 7, 100), (5 * 1024 * 1024 + 123, 8192), (10 * 1024 * 1024 - 10, 10)):
        pwrite_sync(d + "/sparse", off, pat(9, off, n))

    # Many extents, written out of physical order: one file offset per fsync.
    chunk = 64 * 1024
    nchunks = 48
    order = [(i * 17) % nchunks for i in range(nchunks)]
    with open(d + "/many-extents", "wb") as f:
        f.truncate(chunk * nchunks)
    for i in order:
        pwrite_sync(d + "/many-extents", i * chunk, pat(10, i * chunk, chunk))

    # Overwritten in place: the new extents cover parts of the old, bigger one,
    # which leaves extent items whose offset/num_bytes select a slice of a
    # larger disk extent.
    pwrite_sync(d + "/overwrite", 0, pat(11, 0, 1024 * 1024), create=True)
    over = bytearray(pat(11, 0, 1024 * 1024))
    for off, n, seed in ((300000, 100000, 12), (350000, 5000, 13), (0, 100, 14), (1024 * 1024 - 5000, 5000, 15)):
        blob = pat(seed, off, n)
        over[off:off + n] = blob
        pwrite_sync(d + "/overwrite", off, blob)
    os.system("fallocate -p -o 700000 -l 20000 " + d + "/overwrite")  # punch a hole in it

    # PREALLOC.
    os.system("fallocate -l 1M " + d + "/prealloc")
    os.system("fallocate -l 1M " + d + "/prealloc-part")
    pwrite_sync(d + "/prealloc-part", 256 * 1024, pat(16, 256 * 1024, 4096))
    put(d + "/prealloc-beyond-eof", pat(17, 0, 5000))
    os.system("fallocate -n -o 8192 -l 65536 " + d + "/prealloc-beyond-eof")

    # nodatacow file (NODATASUM inode flag, plain extents).
    open(d + "/nodatacow", "wb").close()
    os.system("chattr +C " + d + "/nodatacow")
    pwrite_sync(d + "/nodatacow", 0, pat(18, 0, 100000))

    # A file that grew past its inline size, and a truncated one.
    put(d + "/grown", pat(19, 0, 500))
    pwrite_sync(d + "/grown", 500, pat(19, 500, 200000))
    put(d + "/truncated", pat(20, 0, 100000))
    os.truncate(d + "/truncated", 33333)


def deep(root):
    """Enough items for a tree of level >= 2 with 4 KiB nodes, plus EXTREFs."""
    for i in range(60):
        dd = root + "/d%02d" % i
        os.mkdir(dd)
        for j in range(100):
            put(dd + "/file%03d.txt" % j, ("file %d/%d " % (i, j)).encode() + b"x" * 30)
    os.mkdir(root + "/wide")
    for i in range(3000):
        put(root + "/wide/entry-with-a-longer-name-%05d" % i, b"w%d\n" % i)
    # Many hard links from one directory: the INODE_REF item outgrows a 4 KiB
    # leaf and the kernel switches to INODE_EXTREF items.
    os.mkdir(root + "/links")
    put(root + "/links/target", pat(21, 0, 9000))
    for i in range(400):
        os.link(root + "/links/target", root + "/links/hardlink-with-a-fairly-long-name-%04d" % i)


def compressible(root):
    """Compressible files: the caller mounts with compression forced."""
    os.mkdir(root + "/dir")
    text = (b"The quick brown fox jumps over the lazy dog. " * 4000)
    put(root + "/comp_text", text)
    put(root + "/comp_inline", (b"compress me " * 40))          # small enough to be inline
    put(root + "/comp_big", pat(22, 0, 1024 * 1024 + 12345))    # several 128 KiB compressed extents
    put(root + "/dir/comp_nested", pat(23, 0, 300000))
    os.symlink("comp_text", root + "/link")
    os.mkfifo(root + "/fifo")


def prng(seed, n):
    """n deterministic pseudo-random (incompressible) bytes."""
    out = bytearray()
    x = (seed * 0x9E3779B97F4A7C15 + 1) & (2**64 - 1)
    while len(out) < n:
        x ^= (x << 13) & (2**64 - 1)
        x ^= x >> 7
        x ^= (x << 17) & (2**64 - 1)
        out += struct.pack("<Q", x)
    return bytes(out[:n])


def compress_mix(root):
    """Compressed extents at the interesting sizes and shapes (compression is forced).

    Around the 128 KiB extent size (exactly one extent, one byte less, one byte
    more), sector sizes, an inline file, a sparse file with compressed extents
    between holes, a file whose middle was overwritten so that old compressed
    extents are only partly referenced, and one with an incompressible island.
    """
    os.mkdir(root + "/dir")
    for name, size in (("e128k", 131072), ("e128k_m1", 131071), ("e128k_p1", 131073), ("e256k", 262144), ("s4096", 4096), ("s4097", 4097), ("s8191", 8191), ("t1", 1), ("t200", 200)):
        put(root + "/" + name, pat(30 + size % 7, 0, size))
    put(root + "/text", b"The quick brown fox jumps over the lazy dog. " * 5000)
    put(root + "/dir/big", pat(31, 0, 600000))

    with open(root + "/sparse", "wb") as f:
        f.truncate(1048576)
    pwrite_sync(root + "/sparse", 300000, pat(32, 300000, 4096))
    pwrite_sync(root + "/sparse", 700000, pat(33, 700000, 100000))
    pwrite_sync(root + "/sparse", 1048000, pat(34, 1048000, 500))

    put(root + "/overwritten", pat(35, 0, 300000))
    os.sync()
    pwrite_sync(root + "/overwritten", 50000, pat(36, 50000, 10000))
    pwrite_sync(root + "/overwritten", 200001, pat(37, 200001, 3))
    pwrite_sync(root + "/overwritten", 131072, pat(38, 131072, 4096))

    island = pat(39, 0, 100000)
    put(root + "/island", island[:40000] + prng(5, 20000) + island[60000:])
    os.symlink("text", root + "/link")


def compress_random(root):
    """Incompressible data written with compression forced: Btrfs keeps it as plain extents."""
    put(root + "/random", prng(7, 200000))
    put(root + "/random_128k", prng(8, 131072))
    put(root + "/text", b"The quick brown fox jumps over the lazy dog. " * 3000)


def logbase(root):
    """The committed part of the log-tree fixture: files that logops then changes."""
    os.mkdir(root + "/dir")
    put(root + "/keep_append", pat(40, 0, 100000))
    put(root + "/keep_overwrite", pat(41, 0, 300000))
    put(root + "/keep_meta", pat(42, 0, 9000))
    put(root + "/keep_trunc", pat(43, 0, 200000))
    put(root + "/keep_link", pat(44, 0, 6000))
    put(root + "/keep_del", pat(45, 0, 7000))
    put(root + "/keep_ren", pat(46, 0, 8000))
    put(root + "/keep_hole", pat(47, 0, 200000))
    put(root + "/untouched", pat(48, 0, 30000))
    put(root + "/dir/untouched2", pat(49, 0, 3000))
    put(root + "/dir/keep_del2", pat(50, 0, 3000))


def fsync_path(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def logops(root):
    """Changes that are fsynced but never committed: they end up in the log tree."""
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)

    # New files, a new directory with a file in it.
    put(root + "/n_small", pat(51, 0, 5000))
    fsync_path(root + "/n_small")
    put(root + "/n_big", pat(52, 0, 300000))
    fsync_path(root + "/n_big")
    os.mkdir(root + "/nd")
    put(root + "/nd/f1", pat(53, 0, 20000))
    fsync_path(root + "/nd/f1")

    # Append; overwrite inside an existing extent; shrink.
    with open(root + "/keep_append", "ab") as f:
        f.write(pat(40, 100000, 50000))
        f.flush()
        os.fsync(f.fileno())
    pwrite_sync(root + "/keep_overwrite", 100000, pat(54, 100000, 20000))
    os.truncate(root + "/keep_trunc", 70000)
    fsync_path(root + "/keep_trunc")

    # Inode metadata only.
    os.chmod(root + "/keep_meta", 0o600)
    os.setxattr(root + "/keep_meta", "user.log", b"replayed")
    os.utime(root + "/keep_meta", (1700000000, 1700000123))
    fsync_path(root + "/keep_meta")

    # A hard link, a rename, an unlink (fsync of the directory), a new symlink.
    os.link(root + "/keep_link", root + "/nd/alias")
    fsync_path(root + "/keep_link")
    os.rename(root + "/keep_ren", root + "/renamed_ok")
    fsync_path(root + "/renamed_ok")
    os.unlink(root + "/keep_del")
    os.unlink(root + "/dir/keep_del2")
    fsync_path(root)
    fsync_path(root + "/dir")
    os.symlink("n_small", root + "/newlink")
    fsync_path(root)

    # Punch a hole (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE).
    fd = os.open(root + "/keep_hole", os.O_RDWR)
    libc.fallocate(fd, 0x03, ctypes.c_longlong(40960), ctypes.c_longlong(65536))
    os.fsync(fd)
    os.close(fd)


def plain(root):
    """Written after compression was switched off: plain extents in the same filesystem."""
    put(root + "/hello.txt", b"hello\n")
    put(root + "/plain_big", pat(24, 0, 300000))
    put(root + "/dir/plain_nested", pat(25, 0, 5000))
    with open(root + "/plain_sparse", "wb") as f:
        f.truncate(200000)
    pwrite_sync(root + "/plain_sparse", 100000, pat(26, 100000, 4096))


RECIPES = {"plain": plain, "empty": empty, "minimal": minimal, "tree": tree, "deep": deep, "small": small, "compressible": compressible, "logbase": logbase, "logops": logops, "compress_mix": compress_mix, "compress_random": compress_random}


def main():
    recipe, root = sys.argv[1], sys.argv[2]
    RECIPES[recipe](root)


if __name__ == "__main__":
    main()
