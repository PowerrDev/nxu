#!/usr/bin/env python3
"""Build isolated userland GUI PIC archives and run reachable ELF link audits.

Never changes sibling repositories or installed frameworks. --freestanding-sysroot
builds only core/alloc/compiler_builtins from installed nightly rust-src, avoiding
unneeded std registry dependencies. All results still need deployment validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
TARGET = 'aarch64-unknown-none-softfloat'
TARGET_ARGUMENT = TARGET
ARCH = 'arm64'
PIC_FLAGS = '-C relocation-model=pic -C panic=abort -C force-frame-pointers=yes'


def execute(command, log, env=None):
    child_env = dict(os.environ if env is None else env)
    # Cargo owns its jobserver; Make's pipe descriptors were not inherited.
    for name in ('MAKEFLAGS', 'MFLAGS', 'CARGO_MAKEFLAGS'):
        child_env.pop(name, None)

    print(f'Building {log.stem}; log: {log}', flush=True)
    with log.open('w') as stream:
        with subprocess.Popen(command, env=child_env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, bufsize=1) as process:
            for line in process.stdout:
                stream.write(line)
                stream.flush()
                print(line, end='', flush=True)
            return process.wait()


def sync_tree(source, destination):
    """Keep Cargo input mtimes stable, including adapted files and deletions."""
    destination.mkdir(parents=True, exist_ok=True)
    for path in destination.iterdir():
        if not (source / path.name).exists():
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()

    for path in source.iterdir():
        target = destination / path.name
        if path.is_dir():
            if target.exists() and not target.is_dir():
                target.unlink()
            sync_tree(path, target)
        else:
            if target.is_dir():
                shutil.rmtree(target)
            if not target.exists() or path.read_bytes() != target.read_bytes():
                shutil.copy2(path, target)


def userland_sources(output, frameworks):
    # Adapt a scratch copy first: copying original files over their adapted
    # versions would invalidate Cargo on every invocation.
    with tempfile.TemporaryDirectory(prefix='sources-', dir=output) as temporary:
        prepared = Path(temporary)
        prepare_userland_sources(prepared, frameworks)
        sync_tree(prepared / 'sources', output / 'sources')
        if (prepared / 'include').exists():
            sync_tree(prepared / 'include', output / 'include')
    return output / 'sources'


def prepare_userland_sources(output, frameworks):
    sources = output / 'sources'
    for name in ('WindowServer.framework', 'UIService.framework'):
        if name == 'UIService.framework' and 'CoreGraphics' not in frameworks:
            continue
        shutil.copytree(
            ROOT.parent / name,
            sources / name,
            dirs_exist_ok=True,
            ignore=shutil.ignore_patterns('.git', 'target', 'BUILD', 'build', 'BUILD-i386', 'build-i386', '.DS_Store'),
        )
    paths = [sources / 'WindowServer.framework/nxu/src/lib.rs']
    if 'CoreGraphics' in frameworks:
        paths.append(sources / 'UIService.framework/crates/ui-service-nxu/src/lib.rs')
    for path in paths:
        original = path.read_text()
        text = original.replace('use core::panic::PanicInfo;', '')
        text, count = re.subn(
            "#\\[panic_handler\\]\\nfn panic\\(_info: &PanicInfo<'_>\\) -> ! \\{.*?\\n\\}",
            'include!(' + json.dumps(str(ROOT / 'frameworks/Runtime.framework/rust/panic.rs')) + ');',
            text,
            flags=re.S,
        )
        if count != 1:
            raise RuntimeError(f'Panic handler changed; review adapter for {path}')
        path.write_text(text)
    allocator = sources / 'WindowServer.framework/crates/windowserver-nxu/src/runtime.rs'
    allocator.write_text(
        'include!(' + json.dumps(str(ROOT / 'frameworks/Runtime.framework/rust/allocator.rs')) + ');\n',
    )
    if 'CoreGraphics' in frameworks:
        crate = sources / 'UIService.framework/crates/ui-service-nxu'
        manifest = crate / 'Cargo.toml'
        text = manifest.read_text()
        text, count = re.subn(r'^windowserver-nxu = .*\n', '', text, flags=re.M)
        if count != 1:
            raise RuntimeError('Review CoreGraphics compositor dependency removal')
        manifest.write_text(text)

        lib = crate / 'src/lib.rs'
        text = lib.read_text().replace('use windowserver_nxu as _;', '')
        text += '\nmod runtime { include!(' + json.dumps(
            str(ROOT / 'frameworks/Runtime.framework/rust/allocator.rs')
        ) + '); }\n'
        lib.write_text(text)

        desktop = crate / 'src/desktop.rs'
        desktop.write_text(desktop.read_text().replace(
            'core::hint::spin_loop();', 'unsafe { nxu_desktop_yield(); }'
        ) + '\nunsafe extern "C" { #[link_name = "nxu_yield"] fn nxu_desktop_yield() -> i64; }\n')

        bridge = crate / 'src/bridge.rs'
        bridge.write_text(
            bridge.read_text()
            .replace('ui_bridge_', 'cg_host_')
            .replace('kputln', 'cg_host_log')
            .replace('kmalloc', 'cg_host_alloc')
            .replace('kfree', 'cg_host_free')
        )
        exports = crate / 'src/abi_exports.rs'
        exports.write_text(exports.read_text().replace('UIService', 'CoreGraphics').replace(
            'CoreGraphicsRunDesktop', 'DesktopServiceRun'
        ))

        compositor = crate / 'src/windowserver.rs'
        compositor.write_text(compositor.read_text().replace('WS_', 'CG_'))

        header = output / 'include/CoreGraphics.h'
        header.parent.mkdir(parents=True, exist_ok=True)
        contents = (sources / 'UIService.framework/include/UIService.h').read_text()
        contents = contents.replace('UIService', 'CoreGraphics').replace('UI_SERVICE', 'CORE_GRAPHICS')
        login_start = contents.index('/*\n * The login screen')
        login_end = contents.index('uint32_t CoreGraphicsAPIVersion', login_start)
        contents = contents[:login_start] + contents[login_end:]
        desktop_start = contents.index('/*\n * The desktop:')
        desktop_end = contents.index('/* Bring-up helpers.', desktop_start)
        contents = contents[:desktop_start] + contents[desktop_end:]

        declarations = (ROOT.parent / 'WindowServer.framework/include/WindowServer/WSPrivate.h').read_text()
        clients = [
            line for line in declarations.splitlines()
            if line.startswith(('bool WS_', 'WSWindowID WS_', 'void WS_'))
            and not line.startswith(('bool WS_Initialize', 'bool WS_Pointer_Button'))
        ]
        client_api = '\ntypedef uint32_t CGWindowID;\n' + '\n'.join(clients)
        client_api = client_api.replace('WSWindowID', 'CGWindowID').replace('WS_', 'CG_')
        client_api += '\nbool CoreGraphicsDisplaySize(uint32_t *width, uint32_t *height);\n'
        client_api += 'bool CoreGraphicsConnectionAlive(void);\n'
        header.write_text(contents.replace('\n#endif', client_api + '\n#endif'))

        (header.parent / 'DesktopService.h').write_text(
            '#ifndef NXU_DESKTOP_SERVICE_H\n#define NXU_DESKTOP_SERVICE_H\n'
            '#include <CoreGraphics.h>\n\n'
            'uint32_t DesktopServiceRun(const CoreGraphicsHostV5 *host);\n'
            'bool DesktopServiceBeginSession(uint32_t scale, uint32_t width, uint32_t height, uint32_t menubar);\n'
            'bool DesktopServiceGetTime(uint64_t *seconds);\n'
            'void DesktopServiceShutdown(void);\n'
            'uint32_t DesktopServiceScale(void);\n'
            'uint32_t DesktopServiceGetActivity(void *, CoreGraphicsActivity *, CoreGraphicsProcessInfo *, '
            'uint32_t, uint32_t *);\n'
            'uint32_t DesktopServiceListDirectory(void *, const char *, uint32_t, CoreGraphicsDirEntry *, '
            'uint32_t, uint32_t *, bool *);\n\n#endif\n'
        )

        subprocess.run([
            'cargo', '+nightly', 'generate-lockfile', '--offline',
            '--manifest-path', str(sources / 'UIService.framework/Cargo.toml'),
        ], check=True, capture_output=True)

    return sources


def freestanding_sysroot(output):
    installed = Path(subprocess.check_output(['rustc', '+nightly', '--print', 'sysroot'], text=True).strip())
    library = installed / 'lib/rustlib/src/rust/library'
    copied = output / 'rust-library'
    with tempfile.TemporaryDirectory(prefix='sysroot-', dir=output) as temporary:
        prepared = Path(temporary) / 'library'
        shutil.copytree(library, prepared)
        (prepared / 'Cargo.toml').write_text(
            """[workspace]
resolver="2"
members=["core","alloc","compiler-builtins/compiler-builtins"]
[profile.release]
panic="abort"
[profile.release.package.compiler_builtins]
codegen-units=10000
""",
        )
        # This build deliberately excludes compiler-rt C support. Cargo otherwise
        # resolves the optional cc registry dependency even though it is disabled.
        manifest = prepared / 'compiler-builtins/compiler-builtins/Cargo.toml'
        text = manifest.read_text()
        dependency = '[build-dependencies]\ncc = { version = "1.2", optional = true }'
        if dependency not in text or 'c = ["dep:cc"]' not in text:
            raise RuntimeError('compiler_builtins manifest changed; review freestanding configuration')
        manifest.write_text(text.replace(dependency, '').replace('c = ["dep:cc"]', 'c = []'))
        sync_tree(prepared, copied)
    target = output / 'sysroot-target'
    env = dict(
        os.environ,
        CARGO_TARGET_DIR=str(target),
        CARGO_ENCODED_RUSTFLAGS='\x1f'.join(PIC_FLAGS.split() + ['-Z', 'force-unstable-if-unmarked']),
    )
    command = [
        'cargo',
        '+nightly',
        'build',
        '--manifest-path',
        str(copied / 'Cargo.toml'),
        '--release',
        '--target',
        TARGET_ARGUMENT,
        '--offline',
        '-p',
        'alloc',
    ]
    if ARCH == 'i386':
        command += ['-Z', 'json-target-spec']
    result = execute(command, output / 'sysroot.build.log', env)
    (output / 'sysroot.json').write_text(
        json.dumps(
            {
                'command': command,
                'exit_code': result,
                'toolchain': subprocess.check_output(['rustc', '+nightly', '-Vv'], text=True),
                'source_manifest_sha256': hashlib.sha256((library / 'Cargo.toml').read_bytes()).hexdigest(),
                'features': 'Rust-only compiler_builtins; compiler-rt C disabled',
            },
            indent=2,
        ) + '\n',
    )
    if result:
        raise RuntimeError(f"PIC sysroot build failed; inspect {output / 'sysroot.build.log'}")
    sysroot = output / 'sysroot'
    libs = sysroot / 'lib/rustlib' / TARGET / 'lib'
    libs.mkdir(parents=True, exist_ok=True)
    release = target / TARGET / 'release'
    # New nightly uses build/<crate>/<hash>/out, older Cargo uses deps/.
    artifacts = [p for p in release.rglob('*') if p.suffix in ('.rlib', '.rmeta') and '-' in p.stem]
    for path in artifacts:
        destination = libs / path.name
        if not destination.exists() or path.read_bytes() != destination.read_bytes():
            shutil.copy2(path, destination)
    for name in ('core', 'alloc', 'compiler_builtins'):
        if not list(libs.glob(f'lib{name}-*.rlib')):
            raise RuntimeError(f'PIC sysroot missing {name}')
    return sysroot


def main():
    global TARGET, TARGET_ARGUMENT, ARCH
    for name in ('MAKEFLAGS', 'MFLAGS', 'CARGO_MAKEFLAGS'):
        os.environ.pop(name, None)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--arch', choices=['arm64', 'i386'], default='arm64')
    parser.add_argument('--runtime-dir', type=Path, default=ROOT / 'BUILD/userland')
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--freestanding-sysroot', action='store_true')
    parser.add_argument('--framework', choices=['WindowServer', 'CoreGraphics', 'all'], default='all')
    args = parser.parse_args()
    output = (args.output or Path(tempfile.mkdtemp(prefix='nxu-gui-pic-', dir='/private/tmp'))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    ARCH = args.arch
    if ARCH == 'i386':
        TARGET = 'i686-nxu-none'
        specification = json.loads((ROOT.parent / 'UIService.framework/i686-nxu-none.json').read_text())
        specification['relocation-model'] = 'pic'
        specification['position-independent-executables'] = True
        target_file = output / (TARGET + '.json')
        contents = json.dumps(specification, indent=2) + '\n'
        if not target_file.exists() or target_file.read_text() != contents:
            target_file.write_text(contents)
        TARGET_ARGUMENT = str(target_file)
    try:
        frameworks = ['WindowServer', 'CoreGraphics'] if args.framework == 'all' else [args.framework]
        sources = userland_sources(output, frameworks)
        sysroot = freestanding_sysroot(output) if args.freestanding_sysroot else None
        results = []
        for name, manifest, package in [
            ('WindowServer', sources / 'WindowServer.framework/nxu/Cargo.toml', None),
            ('CoreGraphics', sources / 'UIService.framework/Cargo.toml', 'ui-service-nxu'),
        ]:
            if name not in frameworks:
                continue
            flags = PIC_FLAGS.split() + (['--sysroot=' + str(sysroot)] if sysroot else [])
            env = dict(os.environ, CARGO_TARGET_DIR=str(output / name), CARGO_ENCODED_RUSTFLAGS='\x1f'.join(flags))
            if name == 'CoreGraphics':
                assets = sources / 'UIService.framework/assets'
                defaults = {
                    'INTER_REGULAR': 'fonts/Inter-Regular.ttf',
                    'INTER_SEMIBOLD': 'fonts/Inter-SemiBold.ttf',
                    'BOREL_REGULAR': 'fonts/Borel-Regular.ttf',
                    'BACKGROUND': 'Backgrounds/DefaultWallpaper.jpg',
                    'MENUBAR_LOGO': 'Branding/logo.png',
                }
                for cursor in ('arrow', 'hand', 'move', 'resize-ew', 'resize-ns',
                               'resize-nesw', 'resize-nwse', 'text'):
                    defaults['CURSOR_' + cursor.upper().replace('-', '_')] = 'Cursors/' + cursor + '.cur'
                for asset, relative in defaults.items():
                    env.setdefault('UISERVICE_' + asset, str(assets / relative))
            command = [
                'cargo',
                '+nightly',
                'build',
                '--manifest-path',
                str(manifest),
                '--release',
                '--target',
                TARGET_ARGUMENT,
                '--locked',
            ]
            if ARCH == 'i386':
                command += ['-Z', 'json-target-spec']
            if not sysroot:
                command += ['-Z', 'build-std=core,alloc,compiler_builtins']
            if package:
                command += ['-p', package]
            if args.offline:
                command.append('--offline')
            result = execute(command, output / (name + '.build.log'), env)
            results.append({'framework': name, 'command': command, 'rustflags': flags, 'exit_code': result})
            print(
                f"{name}: {('archive built' if not result else 'build blocked')}; {output / (name + '.build.log')}",
                flush=True,
            )
        (output / 'build.json').write_text(json.dumps(results, indent=2) + '\n')
        if any((result['exit_code'] for result in results)):
            return 1
        # Locate archives across nightly's output-directory layout changes.
        archives = {}
        for name, filename in [('CoreGraphics', 'libui_service_nxu.a'), ('WindowServer', 'libwindowserver_nxu.a')]:
            if name not in frameworks:
                continue
            matches = list((output / name / TARGET / 'release').rglob(filename))
            if not matches:
                raise RuntimeError(f'{name}: archive missing')
            archives[name] = matches[0]
        command = [
            'python3',
            str(ROOT / 'tools/audit_gui_frameworks.py'),
            '--framework',
            args.framework,
            '--output',
            str(output / 'audit'),
            '--userland-exports',
            '--runtime-dir', str(args.runtime_dir.resolve()),
        ]
        for name, archive in archives.items():
            command += ['--coregraphics-archive' if name == 'CoreGraphics' else '--windowserver-archive', str(archive)]
        if 'CoreGraphics' in frameworks:
            for name in ('session', 'host', 'windowserver'):
                framework = 'CoreGraphics' if name == 'windowserver' else 'DesktopService'
                source = ROOT / 'frameworks' / (framework + '.framework/lib') / (name + '.c')
                object_path = output / (name + '.o')
                subprocess.run([
                    'clang', '--target=' + ('i386-none-elf' if ARCH == 'i386' else 'aarch64-none-elf'), '-ffreestanding', '-fPIC',
                    '-fno-stack-protector', '-mgeneral-regs-only', '-O2', '-g',
                    '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT),
                    '-I' + str(ROOT / 'frameworks/include'),
                    '-I' + str(output / 'include'),
                    '-I' + str(ROOT.parent / 'WindowServer.framework/include'),
                    '-c', str(source), '-o', str(object_path),
                ], check=True)
                command += ['--coregraphics-object', str(object_path)]

        return subprocess.run(command).returncode
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'GUI build: {error}', flush=True)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
