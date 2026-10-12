#!/usr/bin/env python3
"""Copyright (c) 2026 NXU Project. All rights reserved.

Resize the Sonoma wallpaper and encode it as a compact QOI kernel asset.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def qoi_encode(image: Image.Image) -> bytes:
    image = image.convert("RGB")
    width, height = image.size
    output = bytearray(b"qoif")
    output.extend(width.to_bytes(4, "big"))
    output.extend(height.to_bytes(4, "big"))
    output.extend((3, 0))
    index = [(0, 0, 0, 0)] * 64
    previous = (0, 0, 0, 255)
    run = 0

    for red, green, blue in image.get_flattened_data():
        pixel = (red, green, blue, 255)

        if pixel == previous:
            run += 1
            if run == 62:
                output.append(0xC0 | (run - 1))
                run = 0
            continue

        if run:
            output.append(0xC0 | (run - 1))
            run = 0

        slot = (red * 3 + green * 5 + blue * 7 + 255 * 11) % 64
        if index[slot] == pixel:
            output.append(slot)
        else:
            index[slot] = pixel
            old_red, old_green, old_blue, _ = previous
            dr = red - old_red
            dg = green - old_green
            db = blue - old_blue
            dr_dg = dr - dg
            db_dg = db - dg

            if -2 <= dr <= 1 and -2 <= dg <= 1 and -2 <= db <= 1:
                output.append(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2))
            elif -32 <= dg <= 31 and -8 <= dr_dg <= 7 and -8 <= db_dg <= 7:
                output.extend((0x80 | (dg + 32), ((dr_dg + 8) << 4) | (db_dg + 8)))
            else:
                output.extend((0xFE, red, green, blue))

        previous = pixel

    if run:
        output.append(0xC0 | (run - 1))
    output.extend((0, 0, 0, 0, 0, 0, 0, 1))
    return bytes(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    with Image.open(args.source) as source:
        source.thumbnail((1920, 1080), Image.Resampling.LANCZOS)
        data = qoi_encode(source)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)


if __name__ == "__main__":
    main()
