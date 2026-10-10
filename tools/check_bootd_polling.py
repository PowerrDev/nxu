#!/usr/bin/env python3
"""Count real bootd manager operations with mocked syscalls, not emulated CPU time."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix='nxu-bootd-poll-', dir='/private/tmp') as scratch:
    binary = Path(scratch) / 'check'
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
        '-I' + str(ROOT / 'frameworks/include'), '-I' + str(ROOT),
        str(ROOT / 'tools/fixtures/gui/registration/manager_poll.c'),
        str(ROOT / 'frameworks/CoreFoundation.framework/sbin/bootd/manager.c'),
        str(ROOT / 'frameworks/lib/string.c'),
        *(['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']),
        '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
