#!/usr/bin/env python3
"""Generate Recovery.framework bitmap font tables from local TTF assets.

The generator intentionally runs on the host. triageOS consumes only the
resulting grayscale glyph tables and has no runtime dependency on FreeType,
UIService.framework, or the original font files.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ASCII_FIRST = 32
ASCII_LAST = 126
SANS_SIZES = (13, 16, 22)
MONO_SIZES = (14, 17, 20)


def variation(font: ImageFont.FreeTypeFont, name: str) -> None:
    try:
        names = font.get_variation_names()
    except (AttributeError, OSError):
        return

    normalized = {
        item.decode("utf-8", errors="ignore") if isinstance(item, bytes) else str(item): item
        for item in names
    }
    if name not in normalized:
        return

    font.set_variation_by_name(normalized[name])


def raster_face(path: Path, size: int, variation_name: str | None = None):
    font = ImageFont.truetype(str(path), size=size)
    if variation_name is not None:
        variation(font, variation_name)

    ascent, descent = font.getmetrics()
    line_height = max(1, ascent + descent)
    pixels: list[int] = []
    glyphs = []

    for code in range(ASCII_FIRST, ASCII_LAST + 1):
        character = chr(code)
        left, top, right, bottom = font.getbbox(character, anchor="ls")
        width = max(0, right - left)
        height = max(0, bottom - top)
        offset = len(pixels)

        if width != 0 and height != 0:
            image = Image.new("L", (width, height), 0)
            draw = ImageDraw.Draw(image)
            draw.text((-left, -top), character, font=font, fill=255, anchor="ls")
            pixels.extend(image.tobytes())

        advance = max(0, int(round(font.getlength(character))))
        x_offset = left
        y_offset = ascent + top

        if not -128 <= x_offset <= 127 or not -128 <= y_offset <= 127:
            raise ValueError(f"glyph offset out of range for U+{code:04X}")
        if width > 255 or height > 255 or advance > 255 or line_height > 255:
            raise ValueError(f"glyph metric out of range for U+{code:04X}")

        glyphs.append((offset, width, height, x_offset, y_offset, advance))

    return pixels, glyphs, line_height


def emit_bytes(name: str, values: list[int]) -> str:
    lines = [f"static const uint8_t {name}[{len(values)}] = {{"]
    for start in range(0, len(values), 24):
        chunk = values[start:start + 24]
        lines.append("\t" + ", ".join(f"{value}U" for value in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def emit_glyphs(name: str, glyphs) -> str:
    lines = [f"static const startup_options_ui_font_glyph_t {name}[95] = {{"]
    for offset, width, height, x_offset, y_offset, advance in glyphs:
        lines.append(
            f"\t{{ {offset}U, {width}U, {height}U, {x_offset}, {y_offset}, {advance}U }},"
        )
    lines.append("};")
    return "\n".join(lines)


def write_sans(path: Path, output: Path) -> None:
    regular = [raster_face(path, size, "Regular") for size in SANS_SIZES]
    semibold = [raster_face(path, size, "SemiBold") for size in SANS_SIZES]

    parts = [
        '#include "font_sans.h"',
        "",
        "#include <stdbool.h>",
        "#include <stdint.h>",
        "",
        "/* Generated from a local recovery font asset. Do not edit by hand. */",
    ]

    for prefix, faces in (("regular", regular), ("semibold", semibold)):
        for index, (pixels, glyphs, _line_height) in enumerate(faces):
            parts.extend([
                "",
                emit_bytes(f"g_{prefix}_{index}_pixels", pixels),
                emit_glyphs(f"g_{prefix}_{index}_glyphs", glyphs),
            ])

    parts.extend([
        "",
        "static const startup_options_ui_font_face_t g_regular_faces[3] = {",
    ])
    for index, (_pixels, _glyphs, line_height) in enumerate(regular):
        parts.append(f"\t{{ g_regular_{index}_pixels, g_regular_{index}_glyphs, {line_height}U }},")
    parts.extend([
        "};",
        "",
        "static const startup_options_ui_font_face_t g_semibold_faces[3] = {",
    ])
    for index, (_pixels, _glyphs, line_height) in enumerate(semibold):
        parts.append(f"\t{{ g_semibold_{index}_pixels, g_semibold_{index}_glyphs, {line_height}U }},")
    parts.extend([
        "};",
        "",
        "static uint32_t face_index(uint32_t scale)",
        "{",
        "\tif (scale <= 1U) return 0U;",
        "\tif (scale == 2U) return 1U;",
        "\treturn 2U;",
        "}",
        "",
        "const startup_options_ui_font_face_t *startup_options_ui_font_face(uint32_t scale)",
        "{",
        "\treturn &g_regular_faces[face_index(scale)];",
        "}",
        "",
        "const startup_options_ui_font_face_t *startup_options_ui_font_face_for_weight(uint32_t scale, uint32_t weight)",
        "{",
        "\tuint32_t index = face_index(scale);",
        "\treturn weight >= 550U ? &g_semibold_faces[index] : &g_regular_faces[index];",
        "}",
        "",
        "bool startup_options_ui_font_has_weights(void)",
        "{",
        "\treturn true;",
        "}",
        "",
        "const startup_options_ui_font_glyph_t *startup_options_ui_font_lookup(const startup_options_ui_font_face_t *face, char character)",
        "{",
        "\tif (face == 0) return 0;",
        "\tuint32_t code = (uint8_t)character;",
        "\tif (code < 32U || code > 126U) code = (uint32_t)'?';",
        "\treturn &face->glyphs[code - 32U];",
        "}",
        "",
        "const uint8_t *startup_options_ui_font_pixels(const startup_options_ui_font_face_t *face) { return face == 0 ? 0 : face->pixels; }",
        "uint32_t startup_options_ui_font_line_height(const startup_options_ui_font_face_t *face) { return face == 0 ? 0U : face->line_height; }",
        "",
    ])

    output.write_text("\n".join(parts))


def write_mono(path: Path, output: Path) -> None:
    faces = [raster_face(path, size) for size in MONO_SIZES]
    parts = [
        '#include "font_mono.h"',
        "",
        "#include <stdint.h>",
        "",
        "/* Generated from a local recovery font asset. Do not edit by hand. */",
    ]

    for index, (pixels, glyphs, _line_height) in enumerate(faces):
        parts.extend([
            "",
            emit_bytes(f"g_mono_{index}_pixels", pixels),
            emit_glyphs(f"g_mono_{index}_glyphs", glyphs),
        ])

    parts.extend([
        "",
        "static const startup_options_ui_font_face_t g_mono_faces[3] = {",
    ])
    for index, (_pixels, _glyphs, line_height) in enumerate(faces):
        parts.append(f"\t{{ g_mono_{index}_pixels, g_mono_{index}_glyphs, {line_height}U }},")
    parts.extend([
        "};",
        "",
        "const startup_options_ui_font_face_t *startup_options_ui_mono_font_face(uint32_t scale)",
        "{",
        "\tif (scale <= 1U) return &g_mono_faces[0];",
        "\tif (scale == 2U) return &g_mono_faces[1];",
        "\treturn &g_mono_faces[2];",
        "}",
        "",
    ])
    output.write_text("\n".join(parts))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sans", type=Path, required=True)
    parser.add_argument("--mono", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_sans(args.sans, args.out_dir / "font_sans.c")
    write_mono(args.mono, args.out_dir / "font_mono.c")


if __name__ == "__main__":
    main()
