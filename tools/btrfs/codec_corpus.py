#!/usr/bin/env python3
"""Make the inputs of the decoder tests (tools/btrfs/test_host.sh, `btrfs_host codec`).

usage: codec_corpus.py OUTDIR

Writes, for a handful of plaintexts, streams made by real compressors:

  NAME.bin                  the plaintext
  NAME.zlib.<variant>       zlib streams (python's zlib: levels 1, 6, 9, stored, fixed
                            Huffman, run-length and Huffman-only strategies)
  NAME.zstd.<variant>       Zstandard frames (the zstd command: levels 1, 3, 9, 15, 19,
                            --ultra 22 with a checksum, a small window, no checksum)

The LZO streams are made by `btrfs_host codec lzo` (its own generator) from the same
plaintexts. Plaintexts are up to 128 KiB, the size of a Btrfs compressed extent.
"""

import os
import random
import subprocess
import sys
import zlib


def texts():
    rng = random.Random(1234)
    words = [b"btrfs", b"extent", b"the", b"quick", b"brown", b"fox", b"jumps", b"over", b"lazy", b"dog", b"kernel", b"sector", b"\n", b" ", b"0123456789", b"NXU"]
    prose = b" ".join(rng.choice(words) for _ in range(30000))[:131072]

    # Mixed: a text block, random noise, a long run, structured records, more text.
    records = b"".join(b"%08x:%s\n" % (i * 2654435761 % (1 << 32), b"name%04d" % (i % 97)) for i in range(4000))
    mixed = prose[:30000] + bytes(rng.randrange(256) for _ in range(20000)) + b"\0" * 25000 + records[:40000] + prose[:16072]

    return {
        "prose": prose,
        "mixed": mixed[:131072],
        "random": bytes(rng.randrange(256) for _ in range(60000)),
        "zeros": b"\0" * 131072,
        "run": (b"ab" * 3 + b"c") * 18000,
        "tiny": b"hello hello hello hello world",
        "one": b"x",
        "sector": prose[:4096],
        "odd": prose[:70001],
    }


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)

    for name, data in texts().items():
        write(os.path.join(out, name + ".bin"), data)

        for label, level, strategy in (("l1", 1, zlib.Z_DEFAULT_STRATEGY), ("l6", 6, zlib.Z_DEFAULT_STRATEGY), ("l9", 9, zlib.Z_DEFAULT_STRATEGY),
                                       ("stored", 0, zlib.Z_DEFAULT_STRATEGY), ("fixed", 9, zlib.Z_FIXED), ("rle", 6, zlib.Z_RLE), ("huff", 6, zlib.Z_HUFFMAN_ONLY)):
            c = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
            write(os.path.join(out, "%s.zlib.%s" % (name, label)), c.compress(data) + c.flush())

        for label, args in (("l1", ["-1"]), ("l3", ["-3"]), ("l9", ["-9"]), ("l15", ["-15"]), ("l19", ["-19"]), ("ultra22", ["--ultra", "-22"]),
                            ("nocheck", ["-3", "--no-check"]), ("w14", ["-19", "--zstd=wlog=14"]), ("btrfs", ["-3", "--no-check", "--zstd=wlog=17"])):
            dst = os.path.join(out, "%s.zstd.%s" % (name, label))
            subprocess.run(["zstd", "-q", "-f", "-c"] + args + [os.path.join(out, name + ".bin")], stdout=open(dst, "wb"), check=True)


if __name__ == "__main__":
    main()
