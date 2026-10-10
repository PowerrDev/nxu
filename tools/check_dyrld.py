#!/usr/bin/env python3
"""Prove ARM64 runtime linking and compatible framework updates on private disks."""
import hashlib
import argparse
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from tep_test_fixture import EnclaveFixture


ROOT = Path(__file__).resolve().parent.parent
MAKE_OPTIONS = []


def run(command, log, cwd=ROOT):
    if command[0] == 'make':
        command = [command[0], *MAKE_OPTIONS, *command[1:]]
    subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)


def job(path):
    return f'<plist version="1.0"><dict><key>Label</key><string>com.nxu.dyrtest</string><key>Program</key><string>{path}</string><key>AllowDisplay</key><true/><key>RunAtLoad</key><true/><key>KeepAlive</key><false/></dict></plist>'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--cached-gui',
        action='store_true',
        help='use existing sibling GUI archives without rebuilding them',
    )
    parser.add_argument('--user-cc', help='override userland C compiler')
    args = parser.parse_args()
    if args.cached_gui:
        MAKE_OPTIONS.extend(
            ['-o', 'uiservice-apps-build', '-o', 'windowserver-build', '-o', 'uiservice-build'],
        )
    if args.user_cc:
        MAKE_OPTIONS.append('USER_CC=' + args.user_cc)
    scratch = Path(tempfile.mkdtemp(prefix='nxu-dyrld-', dir='/private/tmp'))
    print(f'dyrld check artifacts: {scratch}', flush=True)
    diskroot = scratch / 'DiskRoot'
    shutil.copytree(ROOT / 'tools/DiskRoot', diskroot)
    libs = diskroot / 'System/Library/Frameworks'
    for name in ('Leaf', 'Math'):
        (libs / f'{name}.framework').mkdir(parents=True, exist_ok=True)
    fixtures = ROOT / 'tools/fixtures/dyrld'
    flags = [
        '--target=aarch64-none-elf',
        '-ffreestanding',
        '-fPIC',
        '-fno-stack-protector',
        '-fno-omit-frame-pointer',
        '-mno-omit-leaf-frame-pointer',
        '-O1',
        '-g',
        '-mgeneral-regs-only',
    ]
    with (scratch / 'build.log').open('w') as log:
        run(['make', '-j8', 'BUILD/userland/CoreFoundation.so'], log)
        for value in (7, 14):
            run(
                [
                    'clang',
                    *flags,
                    f'-DLEAF_VALUE={value}',
                    '-c',
                    str(fixtures / 'leaf.c'),
                    '-o',
                    str(scratch / f'leaf-{value}.o'),
                ],
                log,
            )
            run(
                [
                    'ld.lld',
                    '-shared',
                    '--hash-style=sysv',
                    '-z',
                    'max-page-size=4096',
                    '-z',
                    'norelro',
                    '--soname',
                    'Leaf.framework/Leaf.so',
                    str(scratch / f'leaf-{value}.o'),
                    '-o',
                    str(scratch / f'Leaf-{value}.so'),
                ],
                log,
            )
        shutil.copyfile(scratch / 'Leaf-7.so', libs / 'Leaf.framework/Leaf.so')
        run(['clang', *flags, '-c', str(fixtures / 'math.c'), '-o', str(scratch / 'math.o')], log)
        run(
            [
                'ld.lld',
                '-shared',
                '--hash-style=sysv',
                '-z',
                'max-page-size=4096',
                '-z',
                'norelro',
                '--soname',
                'Math.framework/Math.so',
                str(scratch / 'math.o'),
                str(libs / 'Leaf.framework/Leaf.so'),
                '-o',
                str(libs / 'Math.framework/Math.so'),
            ],
            log,
        )
        run(
            [
                'clang',
                *flags,
                '-I.',
                '-Iframeworks/include',
                '-c',
                str(fixtures / 'app.c'),
                '-o',
                str(scratch / 'app.o'),
            ],
            log,
        )
        run(['clang', *flags, '-c', 'frameworks/crt0.S', '-o', str(scratch / 'crt.o')], log)
        core = diskroot / 'System/Library/CoreServices'
        core.mkdir(parents=True, exist_ok=True)
        run(
            [
                'ld.lld',
                '-pie',
                '--hash-style=sysv',
                '-z',
                'max-page-size=4096',
                '-z',
                'norelro',
                '--dynamic-linker',
                '/System/Library/CoreServices/dyrld-runtime',
                str(scratch / 'crt.o'),
                str(scratch / 'app.o'),
                str(libs / 'Math.framework/Math.so'),
                str(ROOT / 'BUILD/userland/CoreFoundation.so'),
                str(ROOT / 'BUILD/userland/Runtime.so'),
                '-o',
                str(core / 'dyrtest'),
            ],
            log,
        )
        jobs = diskroot / 'System/Library/BootDaemons'
        jobs.mkdir(parents=True, exist_ok=True)
        (jobs / 'com.nxu.dyrtest.plist').write_text(job('/disk/System/Library/CoreServices/dyrtest'))
        disk = scratch / 'disk.img'
        run(
            [
                'make',
                '-j8',
                'CONFIG=dyrld-check',
                f'DISK={disk}',
                f'DISK_ROOT={diskroot}',
                f"USER_STAGE_STAMP={scratch / 'stage.stamp'}",
                f"DISK_FORMAT_STAMP={scratch / 'format.stamp'}",
                str(disk),
                'all',
            ],
            log,
        )
    # Manual crash job is unrelated to this regression.
    subprocess.run(
        ['debugfs', '-w', '-R', 'rm /System/Library/BootDaemons/com.nxu.crashtest.plist', str(disk)],
        capture_output=True,
        check=True,
    )
    original = hashlib.sha256((core / 'dyrtest').read_bytes()).hexdigest()
    slides = []
    for revision, expected, aslr in ((7, 42, 0), (14, 84, 0), (14, 84, 1), (14, 84, 1), (14, 84, 1)):
        run_index = len(slides)
        crash = run_index == 4
        if revision == 14 and run_index == 1:
            subprocess.run(
                ['debugfs', '-w', '-R', 'rm /System/Library/Frameworks/Leaf.framework/Leaf.so', str(disk)],
                capture_output=True,
                check=True,
            )
            subprocess.run(
                [
                    'debugfs',
                    '-w',
                    '-R',
                    f"write {scratch / 'Leaf-14.so'} /System/Library/Frameworks/Leaf.framework/Leaf.so",
                    str(disk),
                ],
                capture_output=True,
                check=True,
            )
        serial = scratch / f'serial-{run_index}.log'
        enclave = None
        command = [
            'qemu-system-aarch64',
            '-machine',
            'virt,gic-version=3',
            '-cpu',
            'cortex-a72',
            '-smp',
            '4',
            '-m',
            '512M',
            '-kernel',
            str(ROOT / 'BUILD/dyrld-check/kernel.bin'),
            '-display',
            'none',
            '-global',
            'virtio-mmio.force-legacy=false',
            '-drive',
            f'if=none,format=raw,file={disk},id=nxudisk',
            '-device',
            'virtio-blk-device,drive=nxudisk',
            '-device',
            'virtio-gpu-device',
            '-device',
            'virtio-keyboard-device',
            '-device',
            'virtio-mouse-device',
            '-serial',
            f'file:{serial}',
            '-monitor',
            'none',
            '-no-reboot',
            '-append',
            '-no-sound -no-crash-ui -no-patchd log.trace=0 log.stats_ms=0 dyrld.aslr=0',
        ]
        command[-1] += ' -no-windowserver'
        if aslr:
            enclave = EnclaveFixture(scratch / f'tep-{run_index}.sock', 'unused-test-passcode')
            command[-1] = command[-1].replace('dyrld.aslr=0', 'dyrld.aslr=1')
            command.extend(
                [
                    '-chardev',
                    f"socket,id=tep,path={scratch / f'tep-{run_index}.sock'},server=on,wait=off",
                    '-serial',
                    'chardev:tep',
                ],
            )
        if crash:
            command[command.index('-append') + 1] += ' -dyrld-test-crash'
            subprocess.run(
                ['python3', 'tools/crash_symbols.py', str(scratch / 'Leaf-14.so')],
                cwd=ROOT,
                check=True,
            )
            subprocess.run(
                [
                    'debugfs',
                    '-w',
                    '-R',
                    'rm /System/Library/Frameworks/Leaf.framework/Leaf.so.crashsym',
                    str(disk),
                ],
                capture_output=True,
                check=True,
            )
            subprocess.run(
                [
                    'debugfs',
                    '-w',
                    '-R',
                    f"write {scratch / 'Leaf-14.so.crashsym'} /System/Library/Frameworks/Leaf.framework/Leaf.so.crashsym",
                    str(disk),
                ],
                capture_output=True,
                check=True,
            )
        with (scratch / f'qemu-{run_index}.log').open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            if enclave:
                enclave.start()
            try:
                deadline = time.monotonic() + 120
                marker = 'crashd: saved ' if crash else f'dyrtest: PASS result={expected} initializers=1 weak=0'
                while time.monotonic() < deadline and process.poll() is None:
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if marker in text:
                        break
                    if (
                        'panic(' in text
                        or 'kern_fail' in text
                        or 'dyrld: cannot' in text
                        or ('dyrtest: FAILED' in text)
                    ):
                        raise AssertionError(text[-6000:])
                    time.sleep(0.5)
                assert serial.exists(), f"QEMU did not produce a serial log; inspect {scratch / ('qemu-' + str(run_index) + '.log')}"
                assert marker in serial.read_text(errors='replace'), 'dynamic launch timed out; inspect serial log'
                assert 'dyrtest: CoreFoundation parser passed' in serial.read_text(errors='replace')
                assert 'dyrtest: Runtime allocator and GUI host passed' in serial.read_text(errors='replace')
                slides.append(
                    int(
                        re.search('dyrtest: function=([0-9a-f]{16})', serial.read_text(errors='replace')).group(
                            1,
                        ),
                        16,
                    ),
                )
                if enclave:
                    assert enclave.random_calls >= 5 and enclave.error is None
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                if enclave:
                    enclave.close()
        if crash:
            report_path = re.search('crashd: saved (/disk/[^\\r\\n]+?\\.crash)', serial.read_text(errors='replace')).group(
                1,
            )
            report = scratch / 'framework.crash'
            subprocess.run(
                ['debugfs', '-R', f"dump {report_path.removeprefix('/disk')} {report}", str(disk)],
                capture_output=True,
                check=True,
            )
            content = report.read_text()
            assert (
                'nxu_leaf_crash' in content
                and 'nxu_math_crash' in content
                and ('leaf.c:' in content)
                and ('Slide ' in content)
                and ('Leaf.framework/Leaf.so' in content)
            )
            print(
                'PASS framework crash: randomized image slides, framework symbols and source lines',
                flush=True,
            )
        else:
            print(f'PASS framework revision {revision}: result={expected}, ASLR={aslr}', flush=True)
    assert slides[0] == slides[1] and slides[2] != slides[3] and (slides[2] != slides[1])
    assert hashlib.sha256((core / 'dyrtest').read_bytes()).hexdigest() == original
    extracted = scratch / 'app-after'
    subprocess.run(
        ['debugfs', '-R', f'dump /System/Library/CoreServices/dyrtest {extracted}', str(disk)],
        capture_output=True,
        check=True,
    )
    assert hashlib.sha256(extracted.read_bytes()).hexdigest() == original
    print(
        'PASS dyrld: PIE handoff, nested dependencies, relocations, constructors, weak symbols, independent framework replacement',
        flush=True,
    )


if __name__ == '__main__':
    main()
