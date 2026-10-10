#!/usr/bin/env python3
"""Exercise kernel login and SAP with the dynamic ARM64 desktop on a private disk."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from check_gui_service import read_ppm
from tep_test_fixture import EnclaveFixture

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unavailable', action='store_true', help='verify an unreachable enclave keeps login locked')
    parser.add_argument("--setup", action="store_true", help="exercise initial password setup and confirmation")
    parser.add_argument("--restart", action="store_true", help="restart WindowServer while SAP owns scanout")
    parser.add_argument("--production", action="store_true", help="use production jobs and check Dock/app launches")
    parser.add_argument("--kernel", type=Path, default=ROOT / "BUILD/default/kernel.bin")
    options = parser.parse_args()
    scratch = Path(tempfile.mkdtemp(prefix='nxu-gui-trusted-', dir='/private/tmp'))
    print(f'Trusted GUI artifacts: {scratch}', flush=True)
    services = ['bootd', 'windowserver_service', 'desktop_service', 'dyrld', 'dyrld-runtime', 'logd', 'crashd']
    if options.production:
        services.extend(['problemreporter', 'crashtest'])
    with (scratch / 'build.log').open('w') as log:
        def run(command):
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)

        run(['make', '-j8', 'USER_CC=clang', *['BUILD/userland/' + name for name in services]])
        for name, source in [('saptest', 'tools/fixtures/sap/app.c'),
                             ('trusted-probe', 'tools/fixtures/gui/trusted_probe.c'),
                             *([('production-session', 'tools/fixtures/gui/production_session.c')]
                               if options.production else [])]:
            obj = scratch / (name + '.o')
            run(['clang', '--target=aarch64-none-elf', '-ffreestanding', '-fno-stack-protector',
                 '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-O2', '-Iframeworks/include', '-I.',
                 '-c', source, '-o', str(obj)])
            run(['ld.lld', '-T', 'makedefs/user.ld', '-static',
                 *['BUILD/userland/frameworks/' + path + '.o' for path in
                   ['crt0', 'lib/syscall', 'lib/string', 'lib/thread']], str(obj), '-o', str(scratch / name)])

    with (scratch / 'build.log').open('a') as log:
        subprocess.run([
            'clang', '--target=aarch64-none-elf', '-ffreestanding', '-fno-stack-protector', '-fPIE',
            '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-O2', '-Iframeworks/include',
            '-IBUILD/userland/coregraphics-pic/include', '-I.', '-c', 'tools/fixtures/gui/host_callbacks.c',
            '-o', str(scratch / 'host-callbacks.o'),
        ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([
            'ld.lld', '-pie', '--no-undefined', '--hash-style=sysv', '-z', 'max-page-size=4096',
            '-z', 'relro', '-z', 'now',
            '--dynamic-linker=/System/Library/CoreServices/dyrld-runtime',
            'BUILD/userland/dyrld-pic/frameworks/crt0.o', str(scratch / 'host-callbacks.o'),
            'BUILD/userland/DesktopService.so', 'BUILD/userland/CoreGraphics.so', 'BUILD/userland/Runtime.so',
            '-o', str(scratch / 'host-callbacks'),
        ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)

    if options.restart:
        with (scratch / 'build.log').open('a') as log:
            subprocess.run([
                'clang', '--target=aarch64-none-elf', '-ffreestanding', '-fno-stack-protector',
                '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-O2', '-Iframeworks/include', '-I.',
                '-c', 'tools/fixtures/gui/trusted_bootd.c', '-o', str(scratch / 'trusted-bootd.o'),
            ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects = [
                'frameworks/crt0', 'frameworks/lib/syscall', 'frameworks/lib/string', 'frameworks/lib/thread',
                'frameworks/CoreFoundation.framework/lib/plist/plist',
                'frameworks/CoreFoundation.framework/lib/service/service_config',
                'frameworks/CoreFoundation.framework/lib/service/bootstrap_client',
                'frameworks/CoreFoundation.framework/sbin/bootd/job',
                'frameworks/CoreFoundation.framework/sbin/bootd/manager',
                'frameworks/CoreFoundation.framework/sbin/bootd/registry',
            ]
            subprocess.run([
                'ld.lld', '-T', 'makedefs/user.ld', '-static',
                *['BUILD/userland/' + name + '.o' for name in objects], str(scratch / 'trusted-bootd.o'),
                '-o', str(scratch / 'trusted-bootd'),
            ], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)

    root = scratch / 'DiskRoot'
    shutil.copytree(ROOT / 'tools/DiskRoot', root)
    directory = root / 'System/Library/CoreServices'
    for name in services:
        shutil.copyfile(ROOT / 'BUILD/userland' / name, directory / name)
    if options.restart:
        shutil.copyfile(scratch / 'trusted-bootd', directory / 'bootd')
    shutil.copyfile(directory / 'bootd', directory / 'bootd.recovery')
    for name in ['saptest', 'trusted-probe', 'host-callbacks',
                 *(['production-session'] if options.production else [])]:
        shutil.copyfile(scratch / name, directory / name)
    for framework in ['Runtime', 'CoreFoundation', 'WindowServer', 'CoreGraphics', 'DesktopService']:
        target = root / 'System/Library/Frameworks' / (framework + '.framework')
        target.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / 'BUILD/userland' / (framework + '.so'), target / (framework + '.so'))

    jobs = root / 'System/Library/BootDaemons'
    if not options.production:
        shutil.rmtree(jobs)
        jobs.mkdir()
        for name in ['dyrld', 'logd', 'crashd', 'windowserver']:
            shutil.copyfile(ROOT / 'tools/DiskRoot/System/Library/BootDaemons' / ('com.nxu.' + name + '.plist'),
                            jobs / ('com.nxu.' + name + '.plist'))
    for name, program, display in [('desktop', 'desktop_service', True), ('saptest', 'saptest', False),
                                   ('trusted-probe', 'trusted-probe', True),
                                   ('host-callbacks', 'host-callbacks', False),
                                   *([('production-session', 'production-session', False)]
                                     if options.production else [])]:
        if options.production and name == 'desktop':
            continue
        (jobs / ('com.nxu.' + name + '.plist')).write_text(
            '<plist version="1.0"><dict><key>Label</key><string>com.nxu.' + name + '</string>'
            '<key>Program</key><string>/disk/System/Library/CoreServices/' + program + '</string>'
            '<key>RunAtLoad</key><true/><key>KeepAlive</key>' + ('<true/>' if name in ('desktop', 'host-callbacks') else '<false/>')
            + ('<key>AllowDisplay</key><true/>' if display else '') + '</dict></plist>\n'
        )
    fixtures = root / 'System/Library/Frameworks/SAPTest.framework'
    fixtures.mkdir(exist_ok=True)
    for name in ['keep', 'remove']:
        (fixtures / (name + '.txt')).write_text('Private SAP integration fixture.\n')
    disk = scratch / 'disk.img'
    subprocess.run(['truncate', '-s', '256M', str(disk)], check=True)
    subprocess.run([
        'mkfs.ext4', '-F', '-q', '-b', '4096', '-I', '256',
        '-O', 'has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit',
        '-E', 'lazy_itable_init=0,lazy_journal_init=0', '-d', str(root), str(disk),
    ], check=True)

    pipe = scratch / 'tep'
    for suffix in ['.in', '.out']:
        os.mkfifo(str(pipe) + suffix)
    password = 'sap-password'
    enclave = None if options.unavailable else EnclaveFixture(
        pipe, password, transport='pipe', password_set=not options.setup
    )
    serial = scratch / 'serial.log'
    command = [
        'qemu-system-aarch64', '-machine', 'virt,gic-version=3', '-cpu', 'cortex-a72', '-smp', '4', '-m', '512M',
        '-kernel', str(options.kernel.resolve()), '-display', 'none',
        '-global', 'virtio-mmio.force-legacy=false', '-drive', f'if=none,format=raw,file={disk},id=disk',
        '-device', 'virtio-blk-device,drive=disk', '-device', 'virtio-gpu-device,xres=640,yres=480',
        '-device', 'virtio-keyboard-device', '-device', 'virtio-mouse-device', '-serial', f'file:{serial}',
        '-chardev', f'pipe,id=tep,path={pipe}', '-serial', 'chardev:tep', '-monitor', 'stdio', '-no-reboot',
        '-append', 'ui.userland=1 ui.scale=1000 -no-sound -no-patchd log.trace=0 log.stats_ms=0'
                   + ('' if options.production else ' -no-crash-ui'),
    ]
    with (scratch / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        if enclave:
            enclave.start()
        deadline = time.monotonic() + 180

        def monitor(command):
            process.stdin.write((command + '\n').encode())
            process.stdin.flush()

        def text():
            return serial.read_text(errors='replace') if serial.exists() else ''

        def wait(marker, count=1):
            while time.monotonic() < deadline and process.poll() is None:
                content = text()
                assert not any(marker in content for marker in ['FAILED', 'kern_fail', 'mount failed']), content[-6000:]
                if content.count(marker) >= count:
                    return content
                time.sleep(0.1)
            raise AssertionError(f'Timeout waiting for {marker}; see {serial}')

        def key(code):
            monitor('sendkey ' + code)
            time.sleep(0.15)

        def type_password(value):
            for character in value:
                key('minus' if character == '-' else character)

        def capture(name):
            path = scratch / (name + '.ppm')
            path.unlink(missing_ok=True)
            monitor('screendump ' + str(path))
            for _ in range(100):
                try:
                    return read_ppm(path)
                except (FileNotFoundError, ValueError, AssertionError):
                    time.sleep(0.1)
            raise AssertionError('Screenshot incomplete')

        def capture_panel(name, x, y):
            for _ in range(100):
                frame = capture(name)
                offset = (y * frame[0] + x) * 3
                if frame[2][offset:offset + 3] == bytes([239, 239, 241]):
                    return frame
                time.sleep(0.1)
            raise AssertionError('Trusted panel did not reach scanout')

        try:
            wait('trusted-login: waiting for tepOS authentication')
            wait('trusted-probe: scanout/input/lease/host excluded')
            login = capture_panel('login', 41, 111)
            assert login[2][(111 * 640 + 41) * 3:(111 * 640 + 41) * 3 + 3] == bytes([239, 239, 241])
            if options.unavailable:
                type_password(password)
                key('ret')
                time.sleep(5)
                assert 'trusted-login: unlocked' not in text()
                assert 'desktop: dynamic host ready' not in text()
                assert 'ui_bridge_session_begin: apps may connect' not in text()
                print('PASS unreachable enclave retains trusted login and excludes userland GUI', flush=True)
                return
            # Input must follow enclave discovery, which can be slow under TCG.
            wait('tep_monitor_health: tepOS health: ok')
            time.sleep(2)
            type_password('abcd' if options.setup else 'incorrect')
            key('ret')
            if options.setup:
                type_password('incorrect')
                key('ret')
            wait('trusted-login: password refused')
            assert 'desktop: dynamic host ready' not in text()
            type_password(password)
            key('ret')
            if options.setup:
                type_password(password)
                key('ret')
            wait('trusted-login: unlocked by tepOS')
            wait('desktop: dynamic host ready')
            wait('host-callbacks: PASS directory errors/truncation/metadata and activity snapshots')
            wait('sap: confirmation requested')
            modal = capture_panel('sap-confirmation', 41, 91)
            assert modal[2][(91 * 640 + 41) * 3:(91 * 640 + 41) * 3 + 3] == bytes([239, 239, 241])
            if options.restart:
                wait('trusted-manager: compositor killed during modal')
                time.sleep(1)
                modal = capture_panel('sap-after-compositor-exit', 41, 91)
                assert modal[2][(91 * 640 + 41) * 3:(91 * 640 + 41) * 3 + 3] == bytes([239, 239, 241])
            time.sleep(0.5)
            key('esc')
            wait('sap: confirmation requested', 2)
            key('ret')
            wait('sap: password requested')
            type_password('incorrect')
            key('ret')
            wait('sap: password refused')
            capture('sap-refused')
            key('esc')
            wait('sap: confirmation requested', 3)
            key('ret')
            wait('sap: password requested', 2)
            type_password(password)
            key('ret')
            wait('saptest: restored app surface')
            restored = capture('app-restored')
            assert sum(restored[2][offset:offset + 3] == bytes([0, 255, 0])
                       for offset in range(0, len(restored[2]), 3)) >= 1000
            wait(
                'saptest: PASS cancellation, wrong password, exact-file deletion, '
                'no recursive deletion, exclusive input'
            )
            assert password not in text()
            assert enclave.error is None
            assert enclave.denials >= (1 if options.setup else 2) and enclave.accepts >= 2
            capture('desktop-restored')
            if options.production:
                wait('production-session: PASS Dock launches, app processes and activity snapshots')
                for app in ['Voyager', 'Activity Monitor']:
                    wait('ui_bridge_syscall_connect: ' + app)
                wait('ui_bridge_syscall_connect: About ')
                wait('problemreporter: dialog ready')
                wait('crashd: saved ')
                assert 'bootd: com.nxu.about-sevos: started' not in text()
                for _ in range(150):
                    frame = capture('production-apps-and-reporter')
                    pixels = frame[2]
                    light_pixels = sum(
                        pixels[offset:offset + 3] == bytes([239, 239, 241])
                        for offset in range(0, len(pixels), 3)
                    )
                    if light_pixels >= 20000:
                        break
                    time.sleep(0.2)
                else:
                    raise AssertionError('Production app windows did not reach scanout')
                print('PASS production jobs, Dock launches, real apps and crash reporter', flush=True)
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if enclave:
                enclave.close()
    print('PASS dynamic desktop trusted login, SAP authentication and input/scanout exclusion', flush=True)


if __name__ == '__main__':
    main()
