#!/usr/bin/env python3
"""Native policy/allocator checks using real implementation files and host mocks.

Does not emulate ARM64 syscall copyin, IRQ masking, or scheduling.
"""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'tools/fixtures/gui/host_policy'


def main():
    work = Path(tempfile.mkdtemp(prefix='nxu-gui-host-check-', dir='/private/tmp'))

    def write(name, contents):
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)

    write('kern/process/proc.h', (FIXTURES / 'kern/process/proc.h').read_text())
    write('kern/process/signal.h', (FIXTURES / 'kern/process/signal.h').read_text())
    write('kern/lock.h', (FIXTURES / 'kern/lock.h').read_text())
    write('libk/string.h', (FIXTURES / 'libk/string.h').read_text())
    write('check.c', (FIXTURES / 'check.c').read_text())
    command = [
        'clang',
        '-std=c11',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-fsanitize=address,undefined',
        '-I' + str(work),
        '-I' + str(ROOT),
        '-I' + str(ROOT / 'frameworks/include'),
        str(work / 'check.c'),
        str(ROOT / 'kern/console/display_owner.c'),
        str(ROOT / 'frameworks/lib/allocator.c'),
        '-o',
        str(work / 'check'),
    ]
    subprocess.run(command, check=True)
    subprocess.run([str(work / 'check')], check=True)


if __name__ == '__main__':
    main()
