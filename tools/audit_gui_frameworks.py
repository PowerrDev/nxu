#!/usr/bin/env python3
"""Inspect existing ARM64 GUI archives without rebuilding sibling repositories.

The shared-link probes are diagnostic artifacts, not deployable frameworks.
Returns 1 while any existing archive fails the reachable shared-link probe.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
from gui_elf import inspect

RUNTIME_DIR = Path(__file__).resolve().parent.parent / "BUILD/userland"


ROOT = Path(__file__).resolve().parent.parent


def audit(name, archive, entries, output, userland_exports=False, adapter_objects=()):
    if not archive.is_file():
        return {'name': name, 'archive': str(archive), 'error': 'archive missing', 'shared_link': False}
    relocations = subprocess.run(['llvm-readelf', '-r', str(archive)], capture_output=True, text=True, check=True)
    result = {
        'name': name,
        'archive': str(archive),
        'sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'object_relocations_including_debug': dict(sorted(
            Counter(re.findall('R_(?:AARCH64|386)_[A-Z0-9_]+', relocations.stdout)).items()
        )),
    }
    command = [
        'ld.lld',
        '-shared',
        '--hash-style=sysv',
        '--gc-sections',
        '-z',
        'max-page-size=4096',
        '-z',
        'relro',
        '-z',
        'now',
        *['--undefined=' + entry for entry in entries],
        str(archive),
        *map(str, adapter_objects),
        '-o',
        str(output / (name + '.so')),
    ]
    if userland_exports:
        command += [
            '--version-script=' + str(ROOT / 'frameworks' / (name + '.framework') / 'exports.map'),
            '--soname=' + name + '.framework/' + name + '.so',
            str(RUNTIME_DIR / 'Runtime.so'),
        ]
        command.append('--no-undefined')
        if name == 'CoreGraphics':
            command.append(str(RUNTIME_DIR / 'CoreFoundation.so'))
        elif name == 'DesktopService':
            command.append(str(output / 'CoreGraphics.so'))
    link = subprocess.run(command, capture_output=True, text=True)
    (output / (name + '.link.log')).write_text(link.stdout + link.stderr)
    result['shared_link'] = link.returncode == 0
    result['link_command'] = command
    result['diagnostics'] = link.stderr.splitlines()[:30]
    if link.returncode == 0:
        image = str(output / (name + '.so'))
        for key, flags in [('dynamic_table', ['-d']), ('program_headers', ['-l']), ('dynamic_relocations', ['-r'])]:
            report = subprocess.run(['llvm-readelf', *flags, image], capture_output=True, text=True, check=True)
            (output / (name + '.' + key + '.txt')).write_text(report.stdout)
        symbols = subprocess.run(['llvm-nm', '--undefined-only', image], capture_output=True, text=True, check=True)
        result['undefined_symbols'] = symbols.stdout.splitlines()
        result['elf'] = inspect(image)
        result['kernel_bridge_imports'] = [
            name for name in result['elf'].get('imports', [])
            if name.startswith('ui_bridge_')
        ]
        if userland_exports and (not result['elf']['errors']):
            export_map = (ROOT / 'frameworks' / (name + '.framework') / 'exports.map').read_text()
            expected_exports = set(re.findall('^\\s+([A-Za-z_][A-Za-z0-9_]*);', export_map, re.M))
            if set(result['elf']['exports']) != expected_exports:
                result['elf']['errors'].append('C exports differ from reviewed export map')
            expected_dependencies = {'Runtime.framework/Runtime.so'}
            if name == 'CoreGraphics':
                expected_dependencies.add('CoreFoundation.framework/CoreFoundation.so')
            elif name == 'DesktopService':
                expected_dependencies.add('CoreGraphics.framework/CoreGraphics.so')
            if set(result['elf']['needed']) != expected_dependencies:
                result['elf']['errors'].append('unexpected framework dependencies')
            runtime = inspect(RUNTIME_DIR / 'Runtime.so')
            allowed_imports = set(runtime.get('exports', []))
            if name == 'CoreGraphics':
                allowed_imports |= set(inspect(RUNTIME_DIR / 'CoreFoundation.so')['exports'])
            elif name == 'DesktopService':
                allowed_imports |= set(inspect(output / 'CoreGraphics.so')['exports'])
            unexpected = set(result['elf']['imports']) - allowed_imports
            if unexpected:
                result['elf']['errors'].append('unexpected imports: ' + ', '.join(sorted(unexpected)))
        result['deployment_ready'] = False  # RELRO and host/runtime validation are separate gates.
    return result


def main():
    global RUNTIME_DIR
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--runtime-dir', type=Path, default=RUNTIME_DIR)
    parser.add_argument(
        '--coregraphics-archive',
        type=Path,
        default=None,
    )
    parser.add_argument(
        '--windowserver-archive',
        type=Path,
        default=ROOT.parent / 'WindowServer.framework/BUILD/libWindowServerService.a',
    )
    parser.add_argument(
        '--userland-exports',
        action='store_true',
        help='hide Rust internals with the reviewed C export maps',
    )
    parser.add_argument('--coregraphics-object', type=Path, action='append', default=[])
    parser.add_argument('--framework', choices=['WindowServer', 'CoreGraphics', 'all'], default='all')
    args = parser.parse_args()
    RUNTIME_DIR = args.runtime_dir
    output = args.output or Path(tempfile.mkdtemp(prefix='nxu-gui-audit-', dir='/private/tmp'))
    output.mkdir(parents=True, exist_ok=True)
    results = []
    if args.framework in ('all', 'CoreGraphics'):
        if args.coregraphics_archive is None:
            matches = list((ROOT / 'BUILD/userland/coregraphics-pic/CoreGraphics').rglob('libui_service_nxu.a'))
            args.coregraphics_archive = matches[0] if matches else ROOT / 'BUILD/userland/CoreGraphics.a'
        results.append(
            audit(
                'CoreGraphics',
                args.coregraphics_archive,
                ['CoreGraphicsDrawButton', 'CoreGraphicsValidateHostV5'],
                output,
                args.userland_exports,
                [p for p in args.coregraphics_object if p.stem == 'windowserver'],
            ),
        )
        if args.userland_exports:
            results.append(audit(
                'DesktopService', args.coregraphics_archive, ['DesktopServiceRun'], output,
                True, [p for p in args.coregraphics_object if p.stem in ('session', 'host')],
            ))

    if args.framework in ('all', 'WindowServer'):
        results.append(
            audit(
                'WindowServer',
                args.windowserver_archive,
                ['WS_Initialize', 'WS_Present'],
                output,
                args.userland_exports,
            ),
        )
    (output / 'audit.json').write_text(json.dumps(results, indent=2) + '\n')
    print(f'GUI linker audit: {output}')
    for result in results:
        status = (
            'shared-link probe passed; runtime validation still required'
            if result['shared_link']
            else 'shared-link probe blocked (see audit.json and link log)'
        )
        print(f"{result['name']}: {status}")
    return 0 if all((result['shared_link'] and (not result.get('elf', {}).get('errors')) for result in results)) else 1


if __name__ == '__main__':
    raise SystemExit(main())
