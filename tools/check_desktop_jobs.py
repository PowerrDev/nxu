#!/usr/bin/env python3
"""Validate production GUI job activation in dynamic, legacy and safe modes."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix='nxu-desktop-jobs-') as scratch:
    binary = Path(scratch) / 'jobs'
    sources = [
        'tools/fixtures/gui/registration/jobs.c',
        'frameworks/CoreFoundation.framework/lib/service/service_config.c',
        'frameworks/CoreFoundation.framework/lib/plist/plist.c',
        'frameworks/CoreFoundation.framework/sbin/bootd/job.c',
        'frameworks/lib/string.c',
    ]
    subprocess.run([
        'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffunction-sections', '-fdata-sections',
        '-I' + str(ROOT), '-I' + str(ROOT / 'frameworks/include'),
        *[str(ROOT / source) for source in sources],
        *(['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']),
        '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary), str(ROOT)], check=True)
print('PASS desktop jobs: mode gating, safe-mode GUI, disable flags and opt-in demo')
