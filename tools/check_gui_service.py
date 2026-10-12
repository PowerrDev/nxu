#!/usr/bin/env python3
"""Check the dynamic WindowServer service, scanout, input and bootd restarts.

All fixture binaries, service jobs, crash reports and screenshots live on a
private disk. The normal staged disk and sibling framework sources stay intact.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

from gui_elf import inspect


ROOT = Path(__file__).resolve().parent.parent


def read_ppm(path):
    data = path.read_bytes()
    header, pixels = data.split(b'\n255\n', 1)
    lines = [line for line in header.splitlines() if not line.startswith(b'#')]
    assert lines[0] == b'P6'
    width, height = map(int, lines[1].split())
    assert len(pixels) == width * height * 3
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--desktop', action='store_true', help='also exercise the dynamic CoreGraphics desktop')
    parser.add_argument('--width', type=int, default=640)
    parser.add_argument('--height', type=int, default=480)
    parser.add_argument('--scale', type=int, default=1000)
    parser.add_argument('--timeout', type=float, default=180)
    options = parser.parse_args()
    scratch = Path(tempfile.mkdtemp(prefix='nxu-gui-service-', dir='/private/tmp'))
    print(f'GUI service artifacts: {scratch}', flush=True)

    targets = ['bootd', 'windowserver_service', 'dyrld', 'dyrld-runtime', 'crashd', 'logd']
    if options.desktop:
        targets.append('desktop_service')

    with (scratch / 'build.log').open('w') as log:
        def run(command):
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)

        run(['make', '-j8', 'USER_CC=clang', *['BUILD/userland/' + name for name in targets]])
        run([
            'clang', '--target=aarch64-none-elf', '-ffreestanding', '-fno-stack-protector',
            '-fno-pic', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-O2',
            '-Iframeworks/include', '-I.',
            *(['-DNXU_COREGRAPHICS_TEST'] if options.desktop else []),
            '-c', 'tools/fixtures/gui/service.c',
            '-o', str(scratch / 'fixture.o'),
        ])
        objects = [
            'frameworks/crt0', 'frameworks/lib/syscall', 'frameworks/lib/string',
            'frameworks/lib/thread', 'frameworks/CoreFoundation.framework/lib/plist/plist',
            'frameworks/CoreFoundation.framework/lib/service/service_config',
            'frameworks/CoreFoundation.framework/lib/service/bootstrap_client',
            'frameworks/CoreFoundation.framework/sbin/bootd/job',
            'frameworks/CoreFoundation.framework/sbin/bootd/manager',
            'frameworks/CoreFoundation.framework/sbin/bootd/registry',
        ]
        service_image = inspect(ROOT / 'BUILD/userland/windowserver_service')
        assert not service_image['errors']
        assert set(service_image['needed']) == {
            name + '.framework/' + name + '.so'
            for name in ('WindowServer', 'CoreFoundation', 'Runtime')
        }
        assert not any(name.startswith('WS_') for name in service_image['exports'])

        fixture = scratch / 'bootd'
        run([
            'ld.lld', '-T', 'makedefs/user.ld', '-nostdlib', '-static',
            *['BUILD/userland/' + name + '.o' for name in objects],
            str(scratch / 'fixture.o'), '-o', str(fixture),
        ])

        diskroot = scratch / 'DiskRoot'
        shutil.copytree(ROOT / 'tools/DiskRoot', diskroot)
        services = diskroot / 'System/Library/CoreServices'
        for name in targets:
            shutil.copyfile(ROOT / 'BUILD/userland' / name, services / name)
        shutil.copyfile(fixture, services / 'bootd')
        shutil.copyfile(fixture, services / 'bootd.recovery')
        if options.desktop:
            client = services / 'coregraphics-client'
            run([
                'clang', '--target=aarch64-none-elf', '-ffreestanding', '-fno-stack-protector',
                '-fno-pic', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Iframeworks/include', '-I.', '-c', 'tools/fixtures/gui/desktop_client.c',
                '-o', str(scratch / 'client.o'),
            ])
            run([
                'ld.lld', '-T', 'makedefs/user.ld', '-nostdlib', '-static',
                *['BUILD/userland/' + name + '.o' for name in [*objects[:4], objects[6]]],
                str(scratch / 'client.o'), '-o', str(client),
            ])

        run([
            'clang', '--target=aarch64-none-elf', '-ffreestanding', '-fno-stack-protector',
            '-fno-pic', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
            '-Iframeworks/include', '-I.', '-c', 'tools/fixtures/gui/security_client.c',
            '-o', str(scratch / 'security.o'),
        ])
        run([
            'ld.lld', '-T', 'makedefs/user.ld', '-nostdlib', '-static',
            *['BUILD/userland/' + name + '.o' for name in [*objects[:4], objects[6]]],
            str(scratch / 'security.o'), '-o', str(services / 'gui-security-client'),
        ])

        # Only the exercised services participate in this private boot.
        retained = set(targets) | {'bootd.recovery', 'crashd.recovery', 'logd.recovery', 'gui-security-client'}
        if options.desktop:
            retained.add('coregraphics-client')
        for path in services.iterdir():
            if path.name not in retained:
                if path.is_dir():
                    shutil.rmtree(path)
                else:
                    path.unlink()

        for name in ('crashd', 'logd'):
            shutil.copyfile(services / name, services / (name + '.recovery'))

        libraries = diskroot / 'System/Library/Frameworks'
        frameworks = ['Runtime', 'CoreFoundation', 'WindowServer']
        if options.desktop:
            frameworks.extend(['CoreGraphics', 'DesktopService'])
        for name in frameworks:
            directory = libraries / (name + '.framework')
            directory.mkdir(parents=True, exist_ok=True)
            source = ROOT / 'BUILD/userland' / (name + '.so')
            assert not inspect(source)['errors'], name
            shutil.copyfile(source, directory / (name + '.so'))

        jobs = diskroot / 'System/Library/BootDaemons'
        keep = {'com.nxu.' + name + '.plist' for name in ('windowserver', 'dyrld', 'crashd', 'logd')}
        for path in jobs.glob('*.plist'):
            if path.name not in keep:
                path.unlink()

        run(['python3', 'tools/crash_symbols.py', str(diskroot)])
        disk = scratch / 'disk.img'
        run(['truncate', '-s', '1G', str(disk)])
        run([
            'mkfs.ext4', '-F', '-q', '-b', '4096', '-I', '256',
            '-O', 'has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit',
            '-E', 'lazy_itable_init=0,lazy_journal_init=0', '-d', str(diskroot), str(disk),
        ])

    serial = scratch / 'serial.log'
    command = [
        'qemu-system-aarch64', '-machine', 'virt,gic-version=3', '-cpu', 'cortex-a72',
        '-smp', '4', '-m', '512M', '-kernel', str(ROOT / 'BUILD/default/kernel.bin'),
        '-display', 'none', '-global', 'virtio-mmio.force-legacy=false',
        '-drive', f'if=none,format=raw,file={disk},id=nxudisk',
        '-device', 'virtio-blk-device,drive=nxudisk',
        '-device', f'virtio-gpu-device,xres={options.width},yres={options.height}',
        '-device', 'virtio-keyboard-device', '-device', 'virtio-mouse-device',
        '-serial', f'file:{serial}', '-monitor', 'stdio', '-no-reboot',
        '-append', '-no-sound -no-windowserver -no-crash-ui -no-patchd '
        f'log.trace=0 log.stats_ms=0 dyrld.aslr=0 ui.scale={options.scale}',
    ]
    with (scratch / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + options.timeout

        def monitor(text):
            process.stdin.write((text + '\n').encode())
            process.stdin.flush()

        def wait_marker(marker, count=1):
            while time.monotonic() < deadline and process.poll() is None:
                text = serial.read_text(errors='replace') if serial.exists() else ''
                assert 'gui-service: FAILED' not in text, text[-6000:]
                if text.count(marker) >= count:
                    return text
                time.sleep(0.1)
            raise AssertionError(f'Timeout waiting for {marker}; see {serial}')

        def capture(name):
            path = scratch / (name + '.ppm')
            path.unlink(missing_ok=True)
            monitor('screendump ' + str(path))
            while time.monotonic() < deadline:
                try:
                    return read_ppm(path)
                except (FileNotFoundError, ValueError, AssertionError):
                    time.sleep(0.1)
            raise AssertionError('Incomplete screenshot: ' + str(path))

        def pixel(frame, x, y):
            width, _, pixels = frame
            offset = (y * width + x) * 3
            return tuple(pixels[offset:offset + 3])

        def green_surface(name):
            while time.monotonic() < deadline and process.poll() is None:
                frame = capture(name)
                width, height, pixels = frame
                green = [(index % width, index // width) for index in range(width * height)
                         if pixels[index * 3:index * 3 + 3] == b'\x00\xff\x00']
                if len(green) >= 1000:
                    return frame, green
                time.sleep(0.1)
            raise AssertionError('App surface did not reach scanout')

        try:
            wait_marker('gui-service: SECURITY-WINDOW')
            assert pixel(capture('security-window'), 248, 248) == (255, 255, 0)
            wait_marker('gui-service: SECURITY-REAPED')
            assert pixel(capture('security-reaped'), 248, 248) != (255, 255, 0)
            wait_marker('gui-service: SECURITY-PASS')
            wait_marker('gui-service: RED')
            red = capture('red')
            assert pixel(red, 48, 48) == (255, 0, 0)

            # The first packet establishes the service's relative-input baseline.
            monitor('mouse_move 1 1')
            time.sleep(0.2)
            baseline = capture('pointer-baseline')
            monitor('mouse_move 120 100')
            time.sleep(0.2)
            moved = capture('pointer-moved')
            destination_x = moved[0] // 2 + 120
            destination_y = moved[1] // 2 + 100
            assert any(
                pixel(baseline, x, y) != pixel(moved, x, y)
                for y in range(destination_y - 10, destination_y + 30)
                for x in range(destination_x - 10, destination_x + 30)
            ), 'Pointer motion did not update the destination region'
            assert pixel(moved, 48, 48) == (255, 0, 0)

            wait_marker('gui-service: RESIZED')
            resized = capture('resized')
            assert pixel(resized, 88, 88) == (255, 0, 0)
            assert pixel(resized, 48, 48) != (255, 0, 0)

            wait_marker('gui-service: DESTROYED')
            destroyed = capture('destroyed')
            assert pixel(destroyed, 88, 88) != (255, 0, 0)

            wait_marker('gui-service: BLUE')
            assert pixel(capture('restarted'), 48, 48) == (0, 0, 255)
            if options.desktop:
                wait_marker('gui-service: DESKTOP')
                wait_marker('coregraphics-client: FRAME')
                frame, green = green_surface('desktop')
                width, height, pixels = frame
                x = (min(x for x, _ in green) + max(x for x, _ in green)) // 2
                y = (min(y for _, y in green) + max(y for _, y in green)) // 2
                monitor('mouse_move 1 1')
                time.sleep(2)
                dx = round((x - width // 2) * 1000 / options.scale)
                dy = round((y - height // 2) * 1000 / options.scale)
                monitor(f'mouse_move {dx} {dy}')
                time.sleep(2)
                monitor('mouse_button 1')
                time.sleep(2)
                monitor('mouse_button 0')
                wait_marker('coregraphics-client: CLICK')
                wait_marker('event-wait-probe: lost-wakeup, timeout and IPC wake PASS')
                title_y = min(py for _, py in green) - 12
                original_x = min(px for px, _ in green)
                monitor(f'mouse_move 0 {round((title_y - y) * 1000 / options.scale)}')
                time.sleep(0.1)
                monitor('mouse_button 1')
                time.sleep(0.1)
                monitor(f'mouse_move {round(60 * 1000 / options.scale)} {round(40 * 1000 / options.scale)}')
                time.sleep(0.1)
                monitor('mouse_button 0')
                drag_start = time.monotonic()
                while time.monotonic() - drag_start < 5:
                    _, moved = green_surface('dragged')
                    if abs(min(px for px, _ in moved) - original_x - 60) <= 1:
                        break
                    time.sleep(0.05)
                else:
                    raise AssertionError('Window drag did not reach its destination')
                print(f'PASS drag scanout after move: {time.monotonic() - drag_start:.3f}s', flush=True)
                monitor(f'mouse_move 0 {round((y - title_y) * 1000 / options.scale)}')
                time.sleep(0.1)
                monitor('mouse_move 0 0 1')
                wait_marker('coregraphics-client: SCROLL UP')
                monitor('mouse_move 0 0 -1')
                wait_marker('coregraphics-client: SCROLL DOWN')
                monitor('sendkey a')
                wait_marker('coregraphics-client: KEY')
                for iteration, marker in enumerate(['COMPOSITOR-RECOVERED', 'DESKTOP-RECOVERED'], 2):
                    wait_marker('gui-service: ' + marker)
                    # Scanout can retain the old pixels while the desktop restarts.
                    wait_marker('desktop: dynamic host ready', iteration)
                    recovered, green = green_surface(marker.lower())
                    x = (min(x for x, _ in green) + max(x for x, _ in green)) // 2
                    y = (min(y for _, y in green) + max(y for _, y in green)) // 2
                    monitor('mouse_move 1 1')
                    time.sleep(2)
                    dx = round((x - width // 2) * 1000 / options.scale)
                    dy = round((y - height // 2) * 1000 / options.scale)
                    monitor(f'mouse_move {dx} {dy}')
                    time.sleep(2)
                    monitor('mouse_button 1')
                    time.sleep(2)
                    monitor('mouse_button 0')
                    wait_marker('coregraphics-client: CLICK', iteration)
                    monitor('sendkey a')
                    wait_marker('coregraphics-client: KEY', iteration)
                wait_marker('gui-service: APP-CLOSED')
                closed = capture('app-closed')
                assert sum(
                    pixel(closed, x, y) == (0, 255, 0)
                    for y in range(height) for x in range(width)
                ) < 100, 'The dead app retained its visible surface'

            text = wait_marker('gui-service: PASS')
            assert text.count('gui-service: RESTARTED') == 2
            assert 'crashd: saved ' in text
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    reports = re.findall(r'crashd: saved (/disk/Library/Logs/DiagnosticReports/[^\s]+)', text)
    assert len(reports) == 1, reports
    report = scratch / 'windowserver.crash'
    subprocess.run(
        ['debugfs', '-R', f'dump {reports[0].removeprefix("/disk")} {report}', str(disk)],
        capture_output=True, check=True,
    )
    contents = report.read_text()
    assert 'Exception Type: SIGABRT\n' in contents
    assert 'Symbolication: matched image SHA-256' in contents

    print(
        'PASS dynamic service: scanout, pointer input, resize/destroy, malformed IPC, '
        'SIGABRT/SIGKILL restarts'
        + (
            ', app frames/input/exit cleanup, desktop/compositor recovery and host boundary checks'
            if options.desktop else ''
        ),
        flush=True,
    )


if __name__ == '__main__':
    main()
