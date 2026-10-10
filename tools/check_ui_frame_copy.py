#!/usr/bin/env python3
"""Check packed frame copying and validation without bypassing user-copy calls."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix='nxu-ui-frame-copy-', dir='/private/tmp') as scratch:
    binary = Path(scratch) / 'check'
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
        '-I' + str(ROOT / 'frameworks/include'), '-I' + str(ROOT),
        str(ROOT / 'tools/fixtures/gui/registration/frame_copy.c'),
        *(['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']),
        '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
