#!/usr/bin/env python3
"""Run shared WindowServer and Rust allocator/panic probes on a private ARM64 disk.
Requires the output of build_gui_pic.py --offline --freestanding-sysroot.
Does not exercise scanout, migrate the desktop, or replace installed frameworks.
"""
import argparse
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
from gui_elf import inspect


ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument(
        '--modes',
        nargs='+',
        choices=('normal', 'panic', 'relro', 'text', 'relro-fork'),
        default=['normal', 'panic', 'relro', 'text', 'relro-fork'],
    )
    args = parser.parse_args()
    build = args.build.resolve()
    scratch = Path(tempfile.mkdtemp(prefix='nxu-gui-runtime-', dir='/private/tmp'))
    print(f'GUI runtime artifacts: {scratch}', flush=True)
    diskroot = scratch / 'DiskRoot'
    shutil.copytree(ROOT / 'tools/DiskRoot', diskroot)
    libs = diskroot / 'System/Library/Frameworks'
    for name in ('Runtime', 'WindowServer', 'RustProbe'):
        (libs / (name + '.framework')).mkdir(parents=True, exist_ok=True)
    runtime = libs / 'Runtime.framework/Runtime.so'
    shutil.copyfile(ROOT / 'BUILD/userland/Runtime.so', runtime)
    window = libs / 'WindowServer.framework/WindowServer.so'
    shutil.copyfile(build / 'audit/WindowServer.so', window)
    fixture = ROOT / 'tools/fixtures/gui'
    env = dict(os.environ, NXU_RUNTIME_RUST=str(ROOT / 'frameworks/Runtime.framework/rust'))
    flags = [
        '--target=aarch64-none-elf',
        '-ffreestanding',
        '-fPIC',
        '-fno-stack-protector',
        '-fno-omit-frame-pointer',
        '-O1',
        '-g',
        '-mgeneral-regs-only',
    ]
    common = ['--hash-style=sysv', '-z', 'max-page-size=4096', '-z', 'relro', '-z', 'now']
    with (scratch / 'build.log').open('w') as log:

        def run(cmd):
            subprocess.run(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

        run(
            [
                'rustc',
                '+nightly',
                '--edition=2024',
                '--crate-type=staticlib',
                '--target=aarch64-unknown-none-softfloat',
                '--sysroot=' + str(build / 'sysroot'),
                '-C',
                'relocation-model=pic',
                '-C',
                'panic=abort',
                '-C',
                'force-frame-pointers=yes',
                '-C',
                'opt-level=2',
                str(fixture / 'runtime.rs'),
                '-o',
                str(scratch / 'probe.a'),
            ],
        )
        exports = scratch / 'probe.map'
        exports.write_text('{global: nxu_rust_allocation_probe; nxu_rust_panic_probe; local: *;};\n')
        probe = libs / 'RustProbe.framework/RustProbe.so'
        run(
            [
                'ld.lld',
                '-shared',
                *common,
                '--no-undefined',
                '--undefined=nxu_rust_allocation_probe',
                '--undefined=nxu_rust_panic_probe',
                '--version-script=' + str(exports),
                '--soname=RustProbe.framework/RustProbe.so',
                str(scratch / 'probe.a'),
                str(runtime),
                '-o',
                str(probe),
            ],
        )
        assert not inspect(window)['errors']
        assert not inspect(probe)['errors']
        run(
            [
                'clang',
                *flags,
                '-Iframeworks/include',
                '-I.',
                '-I' + str(ROOT.parent / 'WindowServer.framework/include'),
                '-c',
                str(fixture / 'app.c'),
                '-o',
                str(scratch / 'app.o'),
            ],
        )
        run(['clang', *flags, '-c', 'frameworks/crt0.S', '-o', str(scratch / 'crt.o')])
        app = diskroot / 'System/Library/CoreServices/gui-runtime-test'
        run(
            [
                'ld.lld',
                '-pie',
                *common,
                '--no-undefined',
                '--dynamic-linker=/System/Library/CoreServices/dyrld-runtime',
                str(scratch / 'crt.o'),
                str(scratch / 'app.o'),
                str(window),
                str(probe),
                str(runtime),
                '-o',
                str(app),
            ],
        )
        jobs = diskroot / 'System/Library/BootDaemons'
        (jobs / 'com.nxu.crashtest.plist').unlink(missing_ok=True)
        (jobs / 'com.nxu.gui-runtime-test.plist').write_text(
            '<plist><dict><key>Label</key><string>com.nxu.gui-runtime-test</string><key>Program</key><string>/disk/System/Library/CoreServices/gui-runtime-test</string><key>RunAtLoad</key><true/><key>KeepAlive</key><false/></dict></plist>',
        )
        run(['python3', 'tools/crash_symbols.py', str(diskroot)])
        disk = scratch / 'disk.img'
        run(['truncate', '-s', '1G', str(disk)])
        run(
            [
                'mkfs.ext4',
                '-F',
                '-q',
                '-b',
                '4096',
                '-I',
                '256',
                '-O',
                'has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit',
                '-E',
                'lazy_itable_init=0,lazy_journal_init=0',
                '-d',
                str(diskroot),
                str(disk),
            ],
        )
    for mode in args.modes:
        fatal = mode != 'normal'
        serial = scratch / (mode + '.log')
        args = '-no-sound -no-windowserver -no-crash-ui -no-patchd log.trace=0 log.stats_ms=0 dyrld.aslr=0' + (' -gui-' + mode + '-probe' if fatal else '')
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
            str(ROOT / 'BUILD/default/kernel.bin'),
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
            args,
        ]
        with (scratch / ('qemu-' + mode + '.log')).open('w') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 120
                while time.monotonic() < deadline and process.poll() is None:
                    text = serial.read_text(errors='replace') if serial.exists() else ''
                    if 'gui-runtime: FAILED' in text or 'kern_fail' in text or 'dyrld: cannot' in text:
                        raise AssertionError(text[-6000:])
                    if (
                        'gui-runtime: PASS' in text
                        and (not fatal or 'crashd: saved ' in text)
                        and (mode != 'relro-fork' or 'fork RELRO remained sealed' in text)
                    ):
                        break
                    time.sleep(0.5)
                text = serial.read_text(errors='replace')
                assert 'gui-runtime: PASS' in text
                if fatal:
                    assert 'crashd: saved ' in text
                if mode == 'relro-fork':
                    assert 'fork RELRO remained sealed' in text
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if fatal:
            paths = re.findall('crashd: saved (/disk/Library/Logs/DiagnosticReports/[^\\s]+)', text)
            assert len(paths) == 1
            report = scratch / (mode + '.crash')
            subprocess.run(
                ['debugfs', '-R', f"dump {paths[0].removeprefix('/disk')} {report}", str(disk)],
                capture_output=True,
                check=True,
            )
            contents = report.read_text()
            assert 'Exception Type: ' + ('SIGABRT' if mode == 'panic' else 'SIGSEGV') + '\n' in contents
            assert 'Symbolication: matched image SHA-256' in contents
            if mode == 'panic':
                assert 'nxu_runtime_abort' in contents and 'nxu_rust_panic_probe' in contents
        print(
            'PASS ' + mode + ': ' + ('persisted symbolicated crash report' if fatal else 'shared WindowServer composition/churn and aligned Rust allocation'),
            flush=True,
        )
    print(f'Inspect persisted report on {disk}', flush=True)


if __name__ == '__main__':
    main()
