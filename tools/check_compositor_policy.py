#!/usr/bin/env python3
"""Check bootd's compositor registration policy with reused and stale identities."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix='nxu-compositor-policy-') as scratch:
    binary = Path(scratch) / 'check'
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I' + str(ROOT / 'frameworks/include'), '-I' + str(ROOT),
        str(ROOT / 'tools/fixtures/gui/registration/check.c'),
        str(ROOT / 'frameworks/CoreFoundation.framework/sbin/bootd/registry.c'),
        str(ROOT / 'frameworks/lib/string.c'), '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
    windows = Path(scratch) / 'windows'
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
        '-I' + str(ROOT / 'frameworks/include'), '-I' + str(ROOT),
        '-I' + str(ROOT / 'BUILD/userland/gui-pic/sources/WindowServer.framework/include'),
        str(ROOT / 'tools/fixtures/gui/registration/windows.c'),
        *(['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']),
        '-o', str(windows),
    ], check=True)
    subprocess.run([str(windows)], check=True)
print('PASS compositor registration: exact identity, PID reuse, queued exit, window ownership and restart revocation')
