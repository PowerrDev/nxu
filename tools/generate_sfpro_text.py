#!/usr/bin/env python3
"""Instantiate SF Pro *Text* Regular and Semibold from the macOS system font.

The CoreGraphics text renderer (UIService.framework's `ui-text` crate) draws
whatever static outline it is handed; it does not pick an optical size per point
size. macOS, by contrast, renders small UI text with the *Text* optical variant
of SF Pro and emphasised text at Semibold (Apple's semibold is wght 590, not
600). Feeding the renderer the *Display* variant at Regular/Medium is the reason
sevOS chrome reads lighter and thinner than macOS.

This tool pins the `opsz` axis of the system variable font (`SFNS.ttf`) to its
Text extreme (17) and the `wght` axis to 400 / 590, flattening the result to a
plain static TrueType file the no_std renderer can parse with ttf-parser. The
output is subset to exactly the codepoints the shipped SF Pro Display faces
already cover, so there is no glyph-coverage regression and the embedded size
stays comparable.

The macOS system font is Apple-licensed; this script only derives from a copy
already installed on the build machine and never downloads anything. Run it on a
macOS host to refresh the two committed `.ttf` faces when needed:

    .venv-fonts/bin/python tools/generate_sfpro_text.py
"""

from __future__ import annotations

import argparse
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / "tools/DiskRoot/System/Library/Fonts"

# The Text optical extreme, default width, default grade. Weight is per face.
BASE_AXES = {"opsz": 17.0, "wdth": 100.0, "GRAD": 400.0}

# (output name, family, subfamily, weight-axis value, OS/2 usWeightClass)
FACES = [
    ("SF-Pro-Text-Regular.ttf", "SF Pro Text", "Regular", 400.0, 400),
    ("SF-Pro-Text-Semibold.ttf", "SF Pro Text Semibold", "Regular", 590.0, 600),
]


def covered_codepoints(*otf_names: str) -> set[int]:
    """Union of Unicode codepoints the given shipped faces already cover."""
    points: set[int] = set()
    for name in otf_names:
        font = TTFont(FONTS / name)
        for table in font["cmap"].tables:
            if table.isUnicode():
                points |= set(table.cmap.keys())
        font.close()
    return points


def set_names(font: TTFont, family: str, subfamily: str) -> None:
    name = font["name"]
    full = family if subfamily == "Regular" else f"{family} {subfamily}"
    postscript = full.replace(" ", "")
    for platform, encoding, language in ((3, 1, 0x409), (1, 0, 0)):
        name.setName(family, 1, platform, encoding, language)
        name.setName(subfamily, 2, platform, encoding, language)
        name.setName(full, 4, platform, encoding, language)
        name.setName(postscript, 6, platform, encoding, language)
        # Typographic family/subfamily, so all weights group under one family.
        name.setName("SF Pro Text", 16, platform, encoding, language)
        name.setName(subfamily if subfamily != "Regular" else "Regular",
                     17, platform, encoding, language)


def build_face(source: Path, out_name: str, family: str, subfamily: str,
               weight: float, weight_class: int, keep: set[int]) -> None:
    font = TTFont(source, fontNumber=0, lazy=False)
    instantiateVariableFont(font, {**BASE_AXES, "wght": weight}, inplace=True)

    present = {cp for cp in keep if any(
        cp in table.cmap for table in font["cmap"].tables if table.isUnicode())}
    missing = len(keep) - len(present)

    subsetter = subset.Subsetter(options=subset.Options(
        name_IDs=["*"], name_legacy=True, name_languages=["*"],
        layout_features="*", glyph_names=False, recalc_bounds=True,
        recalc_timestamp=False, notdef_outline=True, drop_tables=["DSIG"]))
    subsetter.populate(unicodes=sorted(present))
    subsetter.subset(font)

    if "OS/2" in font:
        font["OS/2"].usWeightClass = weight_class
    set_names(font, family, subfamily)

    destination = FONTS / out_name
    font.save(destination)
    font.close()
    size_kb = destination.stat().st_size / 1024
    print(f"{out_name}: wght={weight:g} opsz=17 -> {len(present)} glyphs, "
          f"{size_kb:.0f} KiB ({missing} of {len(keep)} source codepoints absent)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path("/System/Library/Fonts/SFNS.ttf"),
                        help="macOS system SF Pro variable font to derive from")
    args = parser.parse_args()
    if not args.source.is_file():
        parser.error(f"system font not found: {args.source}")

    keep = covered_codepoints("SF-Pro-Display-Regular.otf",
                              "SF-Pro-Display-Medium.otf")
    print(f"targeting {len(keep)} codepoints from the shipped SF Pro Display faces")
    for out_name, family, subfamily, weight, weight_class in FACES:
        build_face(args.source, out_name, family, subfamily, weight, weight_class, keep)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
