#!/usr/bin/env python3
"""Exercise the trusted SAP modal on a private ARM64 desktop and test enclave."""
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
from tep_test_fixture import EnclaveFixture


ROOT = Path(__file__).resolve().parent.parent


class QMP:

    def __init__(self, path):
        self.client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.client.settimeout(10)
        self.client.connect(str(path))
        self.stream = self.client.makefile('rwb')
        self.stream.readline()
        self.call('qmp_capabilities')

    def call(self, name, arguments=None):
        request = {'execute': name}
        if arguments is not None:
            request['arguments'] = arguments
        self.stream.write(json.dumps(request).encode() + b'\n')
        self.stream.flush()
        while True:
            response = json.loads(self.stream.readline())
            if 'return' in response:
                return response['return']
            if 'error' in response:
                raise AssertionError(response['error'])

    def key(self, key):
        self.call('send-key', {'keys': [{'type': 'qcode', 'data': key}], 'hold-time': 30})
        time.sleep(0.09)

    def type(self, text):
        for character in text:
            self.key('minus' if character == '-' else character)

    def screen(self, path):
        self.call('screendump', {'filename': str(path)})

    def close(self):
        self.stream.close()
        self.client.close()


def main():
    scratch = Path(tempfile.mkdtemp(prefix='nxu-sap-', dir='/private/tmp'))
    print(f'SAP check artifacts: {scratch}', flush=True)
    root = scratch / 'DiskRoot'
    shutil.copytree(ROOT / 'tools/DiskRoot', root)
    disk = scratch / 'disk.img'
    serial = scratch / 'serial.log'
    qmp_path = scratch / 'qmp.sock'
    with (scratch / 'build.log').open('w') as log:
        subprocess.run(
            [
                'make',
                '-j8',
                'CONFIG=sap-check',
                'EXTRA_CFLAGS=-DNXU_DESKTOP_BOOT',
                'UISERVICE=1',
                'WINDOWSERVER=1',
                f'DISK={disk}',
                f'DISK_ROOT={root}',
                f"USER_STAGE_STAMP={scratch / 'stage.stamp'}",
                f"DISK_FORMAT_STAMP={scratch / 'format.stamp'}",
                str(disk),
                'all',
            ],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
        subprocess.run(
            [
                'clang',
                '--target=aarch64-none-elf',
                '-ffreestanding',
                '-fno-stack-protector',
                '-mgeneral-regs-only',
                '-O1',
                '-g',
                '-Iframeworks/include',
                '-I.',
                '-c',
                'tools/fixtures/sap/app.c',
                '-o',
                str(scratch / 'app.o'),
            ],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )
        objects = [ROOT / 'BUILD/userland' / p for p in (
            'frameworks/crt0.o',
            'frameworks/lib/syscall.o',
            'frameworks/lib/string.o',
            'frameworks/lib/thread.o',
        )]
        subprocess.run(
            [
                'ld.lld',
                '-T',
                'makedefs/user.ld',
                '-static',
                *[str(o) for o in objects],
                str(scratch / 'app.o'),
                '-o',
                str(scratch / 'saptest'),
            ],
            cwd=ROOT,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=True,
        )

    def fs(command):
        subprocess.run(['debugfs', '-w', '-R', command, str(disk)], capture_output=True, check=True)

    fs('rm /System/Library/BootDaemons/com.nxu.crashtest.plist')
    fs(f"write {scratch / 'saptest'} /System/Library/CoreServices/saptest")
    plist = scratch / 'job.plist'
    plist.write_text(
        '<plist version="1.0"><dict><key>Label</key><string>com.nxu.saptest</string><key>Program</key><string>/disk/System/Library/CoreServices/saptest</string><key>RunAtLoad</key><true/></dict></plist>',
    )
    fs(f'write {plist} /System/Library/BootDaemons/com.nxu.saptest.plist')
    fs('mkdir /System/Library/Frameworks/SAPTest.framework')
    fixture = scratch / 'fixture.txt'
    fixture.write_text('SAP test data; safe to delete on this private disk.\n')
    for name in ('keep', 'remove'):
        fs(f'write {fixture} /System/Library/Frameworks/SAPTest.framework/{name}.txt')
    password = 'sap-password'
    enclave = EnclaveFixture(scratch / 'tep.sock', password)
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
        str(ROOT / 'BUILD/sap-check/kernel.bin'),
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
        '-chardev',
        f"socket,id=tep,path={scratch / 'tep.sock'},server=on,wait=off",
        '-serial',
        'chardev:tep',
        '-qmp',
        f'unix:{qmp_path},server=on,wait=off',
        '-monitor',
        'none',
        '-no-reboot',
        '-append',
        '-no-sound -no-patchd -no-crash-ui log.trace=0 log.stats_ms=0',
    ]
    with (scratch / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        enclave.start()
        client = None

        def wait_for(marker, count=1, seconds=120):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline and process.poll() is None:
                text = serial.read_text(errors='replace') if serial.exists() else ''
                if text.count(marker) >= count:
                    return
                if 'panic(' in text or 'kern_fail' in text:
                    raise AssertionError(text[-5000:])
                time.sleep(0.1)
            if client:
                client.screen(scratch / 'timeout.ppm')
                for cpu in client.call('query-cpus-fast'):
                    client.call('human-monitor-command', {'command-line': f"cpu {cpu['cpu-index']}"})
                    registers = client.call('human-monitor-command', {'command-line': 'info registers'})
                    (scratch / f"cpu-{cpu['cpu-index']}.txt").write_text(registers)
            raise AssertionError(
                f'timed out waiting for {marker}; enclave error={enclave.error}, commands={enclave.commands}; inspect {serial}',
            )

        try:
            wait_for('the desktop waits for the passcode')
            client = QMP(qmp_path)
            time.sleep(2)
            client.type(password)
            client.key('ret')
            wait_for('sap: confirmation requested')
            time.sleep(0.5)
            client.screen(scratch / 'confirmation.ppm')
            client.key('esc')
            wait_for('sap: confirmation requested', 2)
            client.key('ret')
            wait_for('sap: password requested')
            client.type('incorrect')
            client.key('ret')
            wait_for('sap: password refused')
            client.screen(scratch / 'refused.ppm')
            client.key('esc')
            wait_for('sap: confirmation requested', 3)
            client.key('ret')
            wait_for('sap: password requested', 2)
            client.type(password)
            client.screen(scratch / 'password.ppm')
            client.key('ret')
            wait_for(
                'saptest: PASS cancellation, wrong password, exact-file deletion, no recursive deletion, exclusive input',
            )
            assert enclave.denials >= 1 and enclave.accepts >= 2 and (enclave.error is None)
            assert password not in serial.read_text(errors='replace')
        finally:
            if client:
                client.close()
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            enclave.close()
    keep = scratch / 'keep.txt'
    removed = scratch / 'removed.txt'
    subprocess.run(
        ['debugfs', '-R', f'dump /System/Library/Frameworks/SAPTest.framework/keep.txt {keep}', str(disk)],
        capture_output=True,
        check=True,
    )
    subprocess.run(
        [
            'debugfs',
            '-R',
            f'dump /System/Library/Frameworks/SAPTest.framework/remove.txt {removed}',
            str(disk),
        ],
        capture_output=True,
        check=True,
    )
    assert keep.read_bytes() == fixture.read_bytes() and (not removed.exists())
    print(
        'PASS SAP: trusted modal, password masking, rejected credentials, cancellation, exact-file persistence and exclusive input (test mailbox provider)',
        flush=True,
    )


if __name__ == '__main__':
    main()
