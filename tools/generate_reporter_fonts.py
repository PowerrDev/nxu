#!/usr/bin/env python3
"""Rasterize the staged Inter fonts into antialiased reporter glyphs."""
import argparse
from pathlib import Path
from generate_recovery_fonts import raster_face, emit_bytes


parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parent / 'DiskRoot/System/Library/Fonts'
parts = [
    '/* Generated from sevOS Inter assets. */',
    '#include <stdint.h>',
    'typedef struct { uint32_t offset; uint8_t width, height; int8_t x, y; uint8_t advance; } reporter_glyph_t;',
    'typedef struct { const uint8_t *pixels; const reporter_glyph_t *glyphs; uint8_t height; } reporter_font_t;',
]
for name, filename, size in [('regular', 'Inter-Regular.ttf', 13), ('semibold', 'Inter-SemiBold.ttf', 14)]:
    pixels, glyphs, height = raster_face(root / filename, size)
    parts.append(emit_bytes(name + '_pixels', pixels))
    parts.append('static const reporter_glyph_t ' + name + '_glyphs[95] = {')
    parts.extend(('    { %d, %d, %d, %d, %d, %d },' % glyph for glyph in glyphs))
    parts.append('};')
    parts.append(
        'static const reporter_font_t ' + name + ' = { ' + name + '_pixels, ' + name + '_glyphs, ' + str(height) + ' };',
    )
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text('\n'.join(parts) + '\n')
