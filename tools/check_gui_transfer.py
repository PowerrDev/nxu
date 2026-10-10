#!/usr/bin/env python3
"""Check pixel transfer lifetimes and operation counts using the production adapter."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix='nxu-gui-transfer-', dir='/private/tmp') as scratch:
    binary = Path(scratch) / 'check'
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections',
        '-I' + str(ROOT / 'frameworks/include'), '-I' + str(ROOT),
        '-I' + str(ROOT / 'BUILD/userland/gui-pic/sources/WindowServer.framework/include'),
        str(ROOT / 'tools/fixtures/gui/registration/transfer.c'),
        str(ROOT / 'frameworks/lib/string.c'),
        *(['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']),
        '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
