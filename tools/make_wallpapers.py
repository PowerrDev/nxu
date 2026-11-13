#!/usr/bin/env python3
"""Copyright (c) 2026 NXU Project. All rights reserved.

Prepare desktop wallpapers for the disk image.

The desktop and System Settings decode wallpapers at run time with the small
PNG decoder in UIService.framework (ui-bundle: 8-bit, non-interlaced, at most
4096 pixels a side, no JPEG or GIF). Whatever is dropped into a source folder
(JPEG, PNG, GIF -- the first frame of an animated one -- or anything else
Pillow reads) is therefore re-encoded here, on the host:

    <output>/<Name>.png               the wallpaper, RGB, at most --max-size
                                      pixels on its longer side
    <output>/.thumbnails/<Name>.png   a THUMB_WIDTH x THUMB_HEIGHT cover crop
                                      for System Settings' tiles and preview

<Name> is the source file's name without its extension. Files Pillow cannot
read are skipped with a warning rather than failing the build.

    make_wallpapers.py --output DIR SOURCE [SOURCE...]

SOURCE is an image file or a folder of them.
"""
import argparse
from pathlib import Path
import sys

from PIL import Image, ImageOps

THUMB_WIDTH = 320
THUMB_HEIGHT = 200


def sources(paths):
    for path in paths:
        if path.is_dir():
            yield from sorted(p for p in path.iterdir() if p.is_file() and not p.name.startswith('.'))
        elif path.is_file():
            yield path


def load(path):
    with Image.open(path) as image:
        image.seek(0)
        image = ImageOps.exif_transpose(image)
        if image.mode in ('RGBA', 'LA', 'P'):
            image = image.convert('RGBA')
            flat = Image.new('RGB', image.size, (0, 0, 0))
            flat.paste(image, mask=image.getchannel('A'))
            return flat
        return image.convert('RGB')


def write_png(image, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Pillow writes 8-bit, non-interlaced PNG unless asked otherwise: exactly
    # what the runtime decoder takes.
    image.save(destination, 'PNG', optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--max-size', type=int, default=1920)
    parser.add_argument('sources', type=Path, nargs='+')
    args = parser.parse_args()

    written = 0
    for path in sources(args.sources):
        try:
            image = load(path)
        except (OSError, ValueError) as error:
            print(f'make_wallpapers: skipping {path.name}: {error}', file=sys.stderr)
            continue

        name = path.stem
        wallpaper = image.copy()
        wallpaper.thumbnail((args.max_size, args.max_size), Image.Resampling.LANCZOS)
        write_png(wallpaper, args.output / f'{name}.png')

        thumbnail = ImageOps.fit(image, (THUMB_WIDTH, THUMB_HEIGHT), Image.Resampling.LANCZOS)
        write_png(thumbnail, args.output / '.thumbnails' / f'{name}.png')
        written += 1

    if not written:
        print('make_wallpapers: no wallpapers found', file=sys.stderr)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
