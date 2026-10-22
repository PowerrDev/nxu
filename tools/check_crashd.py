#!/usr/bin/env python3
"""Boot ARM64 with crashd and verify persisted, symbolicated crash reports.
Uses a private disk image; never stages into the tracked DiskRoot or tools/disk.img.
"""
import argparse
import json
import socket
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import sys


ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument(
        '--ui',
        action='store_true',
        help='also verify a desktop dialog and capture its screen',
    )
    parser.add_argument(
        '--stale-symbols',
        action='store_true',
        help='verify raw-address fallback with a mismatched symbol map',
    )
    args = parser.parse_args()
    scratch = Path(tempfile.mkdtemp(prefix='nxu-crashd-', dir='/private/tmp'))
    print(f'crashd check artifacts: {scratch}', flush=True)
    diskroot = scratch / 'DiskRoot'
    shutil.copytree(ROOT / 'tools/DiskRoot', diskroot)
    jobs = diskroot / 'System/Library/BootDaemons'
    jobs.mkdir(parents=True, exist_ok=True)
    test_executable = '/Volumes/mainOS/Applications/Crash Test.app/Contents/SevOS/Crash Test' if args.ui else '/Volumes/mainOS/System/Library/CoreServices/crashtest'
    test_job = f'<plist version="1.0"><dict>\n<key>Label</key><string>com.nxu.crashtest</string>\n<key>Program</key><string>{test_executable}</string>\n<key>RunAtLoad</key><true/>\n</dict></plist>'
    disk = scratch / 'disk.img'
    config = 'crashd-ui-check' if args.ui else 'crashd-check'
    overrides = [
        f'DISK={disk}',
        f'DISK_ROOT={diskroot}',
        f"USER_STAGE_STAMP={scratch / 'stage.stamp'}",
        f"DISK_FORMAT_STAMP={scratch / 'format.stamp'}",
        f'CONFIG={config}',
    ]
    if args.ui:
        overrides.extend(['EXTRA_CFLAGS=-DNXU_DESKTOP_BOOT', 'UISERVICE=1', 'WINDOWSERVER=1'])
    with (scratch / 'build.log').open('w') as log:
        if args.ui:
            subprocess.run(
                ['make', '-j8', 'BUILD/userland/crashtest'],
                cwd=ROOT,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
            )
            contents = diskroot / 'Applications/Crash Test.app/Contents'
            (contents / 'SevOS').mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / 'BUILD/userland/crashtest', contents / 'SevOS/Crash Test')
            (contents / 'Info.plist').write_text(
                '<plist version="1.0"><dict><key>CFBundleName</key><string>Crash Test</string><key>CFBundleIdentifier</key><string>com.nxu.crashtest</string><key>CFBundleExecutable</key><string>Crash Test</string></dict></plist>',
            )
        subprocess.run(
            ['make', '-j8', *overrides, str(disk), 'all'],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
    # Staging includes the registered manual test job. Replace it only on this
    # private disk so --ui runs the test inside its application bundle.
    job = jobs / 'com.nxu.crashtest.plist'
    job.write_text(test_job)
    guest_job = '/System/Library/BootDaemons/com.nxu.crashtest.plist'
    subprocess.run(['debugfs', '-w', '-R', f'rm {guest_job}', str(disk)], capture_output=True, check=True)
    subprocess.run(
        ['debugfs', '-w', '-R', f'write {job} {guest_job}', str(disk)],
        capture_output=True,
        check=True,
    )
    if args.stale_symbols:
        sidecar_guest = test_executable.removeprefix('/Volumes/mainOS') + '.crashsym'
        sidecar = diskroot / sidecar_guest.lstrip('/')
        data = bytearray(sidecar.read_bytes())
        data[8] ^= 0xff
        stale = scratch / 'stale.crashsym'
        stale.write_bytes(data)
        subprocess.run(
            ['debugfs', '-w', '-R', f'rm "{sidecar_guest}"', str(disk)],
            capture_output=True,
            check=True,
        )
        subprocess.run(
            ['debugfs', '-w', '-R', f'write {stale} "{sidecar_guest}"', str(disk)],
            capture_output=True,
            check=True,
        )
    serial = scratch / 'serial.log'
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
        str(ROOT / f'BUILD/{config}/kernel.bin'),
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
        '-no-sound -no-crash-ui -no-patchd log.trace=0 log.stats_ms=0 crash.test=suite',
    ]
    if args.ui:
        command[-1] = command[-1].replace('-no-crash-ui ', '')
        command.extend(['-qmp', f"unix:{scratch / 'qmp.sock'},server=on,wait=off"])
    with (scratch / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline and process.poll() is None:
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if text.count('crashd: saved ') >= 3 and (not args.ui or 'problemreporter: dialog ready' in text):
                    time.sleep(1)
                    if args.ui:
                        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                            client.settimeout(10)
                            client.connect(str(scratch / 'qmp.sock'))
                            stream = client.makefile('rwb')
                            json.loads(stream.readline())
                            for request in (
                                {'execute': 'qmp_capabilities'},
                                {
                                    'execute': 'screendump',
                                    'arguments': {'filename': str(scratch / 'screen.ppm')},
                                },
                            ):
                                stream.write(json.dumps(request).encode() + b'\n')
                                stream.flush()
                                while True:
                                    response = json.loads(stream.readline())
                                    if 'return' in response:
                                        break
                                    if 'error' in response:
                                        raise AssertionError(response['error'])
                    break
                if 'panic(' in text or 'kern_fail' in text:
                    raise AssertionError('kernel failed; inspect serial.log')
                time.sleep(0.5)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    serial_text = serial.read_text(errors='replace')
    assert 'crashtest: normal exit, caught signal, access control, SIGABRT and SIGILL passed' in serial_text, 'negative cases or signal tests failed'
    report_directory = '/Volumes/mainOS/Users/Shared/Library/Logs/DiagnosticReports/' if args.ui else '/Volumes/mainOS/Library/Logs/DiagnosticReports/'
    paths = re.findall(
        'crashd: saved (' + re.escape(report_directory) + 'crash-[0-9]+-[0-9]+-[0-9]+\\.crash)',
        serial_text,
    )
    assert len(paths) == 3 and len(set(paths)) == 3, f'expected exactly 3 reports, got {len(paths)}'
    reports = []
    for i, path in enumerate(paths):
        output = scratch / f'report-{i}.crash'
        subprocess.run(
            ['debugfs', '-R', f"dump {path.removeprefix('/Volumes/mainOS')} {output}", str(disk)],
            capture_output=True,
            check=True,
        )
        reports.append(output.read_text())
    for signal in ('SIGABRT', 'SIGILL', 'SIGSEGV'):
        matching = [report for report in reports if f'Exception Type: {signal}\n' in report]
        assert len(matching) == 1, f'missing or duplicate {signal} report'
        expected = 'Symbolication: unavailable' if args.stale_symbols else 'Symbolication: matched image SHA-256'
        assert expected in matching[0], 'symbol identity handling failed'
        assert 'Snapshot Flags: 0x0' in matching[0], 'thread register snapshot incomplete'
        assert 'x30:' in matching[0] and 'pstate:' in matching[0], 'registers missing'
    segv = next((report for report in reports if 'Exception Type: SIGSEGV\n' in report))
    assert 'far: 0x12345000' in segv, 'fault address lost'
    if not args.stale_symbols:
        assert 'crash_leaf' in segv and 'crash_middle' in segv, 'backtrace symbols missing'
        assert 'frameworks/BootDaemons.framework/crashtest.c:' in segv, 'source lines missing'
    else:
        assert not re.search('^\\d+  .*crash_leaf', segv, re.M), 'stale symbols were used'
    if args.ui:
        assert 'problemreporter: dialog ready' in serial_text and (scratch / 'screen.ppm').exists(), 'desktop dialog missing'
    assert len(re.findall('^Thread \\d+(?: Crashed)?:', segv, re.M)) == 3, 'sibling threads missing'
    # Stale symbol maps must fail closed; generating a map also proves its
    # PT_LOAD digest agrees with the runtime kernel loader's measurement.
    coverage = 'raw-address fallback with stale symbols' if args.stale_symbols else 'backtrace symbols and source lines'
    ui = ', application report routing and desktop dialog' if args.ui else ''
    print(
        f'PASS crashd: SIGSEGV, SIGILL, SIGABRT, registers, sibling threads, {coverage}, persistence and access control{ui}',
        flush=True,
    )


if __name__ == '__main__':
    try:
        main()
    except (AssertionError, subprocess.CalledProcessError) as error:
        print(f'FAIL crashd: {error}', file=sys.stderr)
        sys.exit(1)
