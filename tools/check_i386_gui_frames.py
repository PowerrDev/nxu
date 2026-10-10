#!/usr/bin/env python3
"""Check the i386 desktop's packed/padded app frames on a private disk under TCG."""
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
    parser.add_argument('--build-root', default='BUILD/BUILD-i386-desktop')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--enclave', action='store_true', help='test ASLR and trusted login with an isolated mailbox fixture')
    options = parser.parse_args()
    build = ROOT / options.build_root / 'i386'
    scratch = Path(tempfile.mkdtemp(prefix='nxu-i386-gui-frames-', dir='/private/tmp'))
    print('i386 GUI frame artifacts: ' + str(scratch), flush=True)
    root = scratch / 'DiskRoot'
    shutil.copytree(build / 'diskroot', root)
    services = root / 'System/Library/CoreServices'
    for name in ('bootd', 'logd'):
        shutil.copyfile(build / 'user' / name, services / name)
    shutil.copyfile(services / 'bootd', services / 'bootd.recovery')
    jobs = root / 'System/Library/BootDaemons'
    shutil.rmtree(jobs)
    jobs.mkdir()
    for name in ('dyrld', 'windowserver', 'desktop', 'dock'):
        shutil.copyfile(ROOT / 'frameworks/BootDaemons.framework/Services' / ('com.nxu.' + name + '.plist'), jobs / ('com.nxu.' + name + '.plist'))
    for name in ('logd', 'packed-frame-probe', 'framework-probe'):
        (jobs / ('com.nxu.' + name + '.plist')).write_text(
            '<plist><dict><key>Label</key><string>com.nxu.' + name + '</string>'
            '<key>Program</key><string>/disk/System/Library/CoreServices/' + name + '</string>'
            '<key>RunAtLoad</key><true/><key>KeepAlive</key><false/></dict></plist>\n'
        )
    obj = scratch / 'probe.o'
    subprocess.run([
        'clang', '--target=i386-none-elf', '-ffreestanding', '-fno-stack-protector',
        '-fno-pic', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Iframeworks/include', '-I.', '-c', 'tools/fixtures/gui/packed_frame_client.c', '-o', str(obj),
    ], cwd=ROOT, check=True)
    subprocess.run([
        'ld.lld', '-m', 'elf_i386', '-T', 'makedefs/user-i386.ld', '-nostdlib', '-static',
        *[str(build / 'user' / (name + '.o')) for name in
          ('frameworks/crt0_i386', 'frameworks/lib/syscall_i386', 'frameworks/lib/string')],
        str(obj), str(build / 'user/libnxurt.a'), '-o', str(services / 'packed-frame-probe'),
    ], cwd=ROOT, check=True)
    if options.enclave:
        trusted_object = scratch / 'trusted-probe.o'
        subprocess.run([
            'clang', '--target=i386-none-elf', '-ffreestanding', '-fno-stack-protector',
            '-fno-pic', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
            '-Iframeworks/include', '-I.', '-c', 'tools/fixtures/gui/trusted_probe.c',
            '-o', str(trusted_object),
        ], cwd=ROOT, check=True)
        subprocess.run([
            'ld.lld', '-m', 'elf_i386', '-T', 'makedefs/user-i386.ld', '-nostdlib', '-static',
            *[str(build / 'user' / (name + '.o')) for name in
              ('frameworks/crt0_i386', 'frameworks/lib/syscall_i386', 'frameworks/lib/string')],
            str(trusted_object), str(build / 'user/libnxurt.a'), '-o', str(services / 'trusted-probe'),
        ], cwd=ROOT, check=True)
        (jobs / 'com.nxu.trusted-probe.plist').write_text(
            '<plist><dict><key>Label</key><string>com.nxu.trusted-probe</string>'
            '<key>Program</key><string>/disk/System/Library/CoreServices/trusted-probe</string>'
            '<key>RunAtLoad</key><true/><key>AllowDisplay</key><true/></dict></plist>\n'
        )
    framework_object = scratch / 'framework-probe.o'
    subprocess.run([
        'clang', '--target=i386-none-elf', '-ffreestanding', '-fno-stack-protector',
        '-fPIC', '-mgeneral-regs-only', '-O2', '-Wall', '-Wextra', '-Werror',
        '-Iframeworks/include', '-I.', '-c', 'tools/fixtures/gui/i386_framework_probe.c',
        '-o', str(framework_object),
    ], cwd=ROOT, check=True)
    subprocess.run([
        'ld.lld', '-m', 'elf_i386', '-pie', '--no-undefined', '--hash-style=sysv',
        '-z', 'max-page-size=4096', '-z', 'relro', '-z', 'now',
        '--dynamic-linker', '/System/Library/CoreServices/dyrld-runtime',
        str(build / 'user/framework-pic/frameworks/crt0_i386.o'), str(framework_object),
        str(build / 'user/Runtime.so'), '-o', str(services / 'framework-probe'),
    ], cwd=ROOT, check=True)
    disk = scratch / 'disk.img'
    with disk.open('wb') as stream:
        stream.truncate(64 * 1024 * 1024)
    subprocess.run([
        'mkfs.ext4', '-q', '-F', '-b', '4096', '-I', '256',
        '-O', 'has_journal,metadata_csum,^metadata_csum_seed,^dir_index,^orphan_file,^fast_commit',
        '-E', 'lazy_itable_init=0,lazy_journal_init=0', '-d', str(root), str(disk),
    ], check=True)
    serial = scratch / 'serial.log'
    command = [
        'qemu-system-i386', '-M', 'pc', '-smp', '4', '-m', '512M',
        '-kernel', str(build / 'kernel.elf'), '-display', 'none', '-vga', 'none',
        '-drive', f'if=none,format=raw,file={disk},id=d0',
        '-device', 'virtio-blk-pci,drive=d0,disable-modern=on',
        '-device', 'virtio-keyboard-pci,disable-modern=on',
        '-device', 'virtio-mouse-pci,disable-modern=on',
        '-device', 'virtio-gpu-pci,xres=640,yres=480',
        '-serial', 'file:' + str(serial), '-monitor', 'stdio', '-no-reboot',
        '-append', 'test=desktop ui.userland=1 dyrld.aslr=0 virtio-irq=1 ui.scale=1000 log.trace=0 log.stats_ms=0 -no-sound',
    ]
    fixture = None
    if options.enclave:
        pipe = scratch / 'tep'
        for suffix in ('.in', '.out'):
            os.mkfifo(str(pipe) + suffix)
        fixture = EnclaveFixture(pipe, 'testpass', transport='pipe')
        command += ['-chardev', f'pipe,id=tep,path={pipe}', '-serial', 'chardev:tep']
        command[command.index('-append') + 1] = command[command.index('-append') + 1].replace('dyrld.aslr=0 ', '')
    with (scratch / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        if fixture:
            fixture.start()
        deadline = time.monotonic() + options.timeout

        def monitor(value):
            process.stdin.write((value + '\n').encode())
            process.stdin.flush()

        def text():
            return serial.read_text(errors='replace') if serial.exists() else ''

        def wait(marker):
            while time.monotonic() < deadline and process.poll() is None:
                content = text()
                assert 'phase failed' not in content and 'check failed' not in content, content[-4000:]
                if marker in content:
                    return
                time.sleep(0.1)
            raise AssertionError('Timeout waiting for ' + marker + '; see ' + str(serial))

        def frame(name):
            path = scratch / (name + '.ppm')
            path.unlink(missing_ok=True)
            monitor('screendump ' + str(path))
            for _ in range(100):
                try:
                    return read_ppm(path)
                except (FileNotFoundError, ValueError, AssertionError):
                    time.sleep(0.1)
            raise AssertionError('Incomplete screenshot')

        def colored(name, color):
            while time.monotonic() < deadline:
                width, height, pixels = frame(name)
                points = [(i % width, i // width) for i in range(width * height)
                          if pixels[i * 3:i * 3 + 3] == bytes(color)]
                if len(points) >= 1000:
                    return points
                time.sleep(0.1)
            raise AssertionError('App frame did not reach scanout')

        try:
            if fixture:
                wait('trusted-login: waiting for tepOS authentication')
                wait('tep_monitor_health: tepOS health: ok')
                wait('trusted-probe: scanout/input/lease/host excluded')
                time.sleep(2)
                for character in 'incorrect':
                    monitor('sendkey ' + character)
                    time.sleep(0.15)
                monitor('sendkey ret')
                wait('trusted-login: password refused')
                assert 'desktop: dynamic host ready' not in text()
                for character in 'testpass':
                    monitor('sendkey ' + character)
                    time.sleep(0.15)
                monitor('sendkey ret')
                wait('trusted-login: unlocked by tepOS')
            wait('framework-probe: allocator/binding/constructor PASS; writing RELRO')
            wait('bootd: com.nxu.framework-probe: exited')
            assert 'framework-probe) terminated by #PF Page Fault' in text()
            assert 'framework-probe: RELRO WRITE SUCCEEDED' not in text()
            wait('windowserver: registered as ')
            wait('desktop: dynamic host ready')
            wait('frame-copy-probe: PACKED')
            points = colored('packed', (0, 255, 0))
            x = (min(x for x, _ in points) + max(x for x, _ in points)) // 2
            y = (min(y for _, y in points) + max(y for _, y in points)) // 2
            monitor('mouse_move 1 1')
            time.sleep(1)
            monitor(f'mouse_move {x - 320} {y - 240}')
            time.sleep(1)
            monitor('mouse_button 1')
            time.sleep(1)
            monitor('mouse_button 0')
            wait('frame-copy-probe: CLICK')
            monitor('sendkey a')
            wait('frame-copy-probe: PADDED and FAULT')
            colored('padded', (0, 0, 255))
            monitor('sendkey b')
            wait('frame-copy-probe: EXIT')
            for _ in range(100):
                width, height, pixels = frame('closed')
                if sum(pixels[i:i + 3] == bytes([0, 0, 255])
                       for i in range(0, len(pixels), 3)) < 100:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError('App exit retained its surface')
            steady = text().split('frame-copy-probe: PACKED', 1)[1]
            assert 'syscall: cpu=' not in steady and 'syscall: interval' not in steady
            assert 'check failed' not in text() and 'kern_fail' not in text()
            assert 'trusted-probe: FAILED' not in text()
            if fixture:
                assert fixture.error is None and fixture.random_calls > 0
                assert fixture.denials > 0 and fixture.accepts > 0
            wait('dock: showing ')
            time.sleep(1)
            assert 'bootd: com.nxu.dock: exited' not in text(), text()[-4000:]
        finally:
            if fixture:
                fixture.close()
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    if options.enclave:
        print('PASS i386 ELF32: enclave ASLR, trusted input/display exclusion and password handling', flush=True)
    print('PASS i386 ELF32: constructors, Runtime binding/allocation and RELRO write protection', flush=True)
    print('PASS i386 TCG: packed/padded frames, copy fault, mouse/keyboard, exit cleanup and quiet logs', flush=True)


if __name__ == '__main__':
    main()
