#!/usr/bin/env python3
"""Stage identity-checked, address-indexed ELF symbols and DWARF source lines.

NXUSYM1: <8s32sQ header, followed by sorted <QQQ96s128s records.
Identity is SHA-256 of PT_LOAD file bytes in program-header order, exactly
as measured by the kernel loader. No guest parsing of ELF or DWARF is needed.
"""
import argparse
import bisect
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess


HEADER = struct.Struct('<8s32sQ')
RECORD = struct.Struct('<QQQ96s128s')


def elf_info(data):
    if data[:4] != b'\x7fELF' or data[5] != 1:
        raise ValueError('expected little-endian ELF')
    wide = data[4] == 2
    if data[4] not in (1, 2):
        raise ValueError('unsupported ELF class')
    eh = struct.unpack_from('<HHIQQQIHHHHHH' if wide else '<HHIIIIIHHHHHH', data, 16)
    phoff, shoff, phsize, phnum, shsize, shnum = (eh[4], eh[5], eh[8], eh[9], eh[10], eh[11])
    identity = hashlib.sha256()
    for i in range(phnum):
        ph = struct.unpack_from('<IIQQQQQQ' if wide else '<IIIIIIII', data, phoff + i * phsize)
        if ph[0] == 1:
            offset, size = (ph[2], ph[5]) if wide else (ph[1], ph[4])
            if offset + size > len(data):
                raise ValueError('invalid load segment')
            identity.update(data[offset:offset + size])
    sections = [struct.unpack_from('<IIQQQQIIQQ' if wide else '<IIIIIIIIII', data, shoff + i * shsize) for i in range(shnum)]
    symbols = {}
    for sh in sections:
        if sh[1] != 2 or not sh[9]:
            continue
        strings = sections[sh[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for offset in range(sh[4], sh[4] + sh[5], sh[9]):
            fields = struct.unpack_from('<IBBHQQ' if wide else '<IIIBBH', data, offset)
            name, info, index, address, size = (fields[0], fields[1], fields[3], fields[4], fields[5]) if wide else (fields[0], fields[3], fields[5], fields[1], fields[2])
            if info & 15 != 2 or index == 0 or address == 0:
                continue
            end = names.find(b'\x00', name)
            if end < 0:
                continue
            label = names[name:end].decode('utf-8', 'replace')
            symbols.setdefault(address, (address, address + size, label))
    ordered = sorted(symbols.values())
    # A zero-sized assembly symbol covers only up to the next defined function.
    ordered = [(start, end if end > start else ordered[i + 1][0] if i + 1 < len(ordered) else start + 1, name) for i, (start, end, name) in enumerate(ordered)]
    demangler = shutil.which('llvm-cxxfilt')
    if demangler and ordered:
        output = subprocess.run(
            [demangler],
            input='\n'.join((s[2] for s in ordered)) + '\n',
            capture_output=True,
            text=True,
            check=True,
        ).stdout.splitlines(
        )
        if len(output) == len(ordered):
            ordered = [(start, end, name) for (start, end, _), name in zip(ordered, output)]
    return (identity.digest(), ordered)


def source_lines(binary):
    tool = shutil.which('llvm-dwarfdump')
    if tool is None:
        raise RuntimeError('llvm-dwarfdump is required to build crash symbol maps')
    dump = subprocess.run([tool, '--debug-line', str(binary)], check=True, capture_output=True, text=True).stdout
    directories, files, intervals = ({}, {}, [])
    current_file, pending = (None, None)
    version = 5
    for line in dump.splitlines():
        if line.startswith('debug_line['):
            directories, files, pending = ({}, {}, None)
        match = re.match('\\s*version:\\s*(\\d+)', line)
        if match:
            version = int(match[1])
        match = re.match('include_directories\\[\\s*(\\d+)\\]\\s*=\\s*"(.*)"', line)
        if match:
            directories[int(match[1])] = match[2]
        match = re.match('file_names\\[\\s*(\\d+)\\]:', line)
        if match:
            current_file = int(match[1])
        match = re.match('\\s*name:\\s*"(.*)"', line)
        if match and current_file is not None:
            files[current_file] = (match[1], 0)
        match = re.match('\\s*dir_index:\\s*(\\d+)', line)
        if match and current_file in files:
            files[current_file] = (files[current_file][0], int(match[1]))
        match = re.match('(0x[0-9a-fA-F]+)\\s+(\\d+)\\s+\\d+\\s+(\\d+)\\s+', line)
        if not match:
            continue
        address, row, file_index = (int(match[1], 16), int(match[2]), int(match[3]))
        if pending and address > pending[0]:
            intervals.append((pending[0], address, pending[1]))
        pending = None
        if 'end_sequence' not in line and row:
            name, directory = files.get(file_index, ('', 0))
            if name:
                path = os.path.join(directories.get(directory, ''), name)
                try:
                    path = str(Path(path).resolve().relative_to(Path.cwd()))
                except ValueError:
                    pass
                pending = (address, f'{path}:{row}')
    return sorted(intervals)


def make_records(symbols, lines):
    starts = [s[0] for s in symbols]
    line_starts = [s[0] for s in lines]
    boundaries = sorted({x for s in symbols for x in s[:2]} | {x for s in lines for x in s[:2]})
    records = []
    for start, end in zip(boundaries, boundaries[1:]):
        index = bisect.bisect_right(starts, start) - 1
        if index < 0 or start >= symbols[index][1]:
            continue
        function_start, _, function = symbols[index]
        source_index = bisect.bisect_right(line_starts, start) - 1
        source = lines[source_index][2] if source_index >= 0 and start < lines[source_index][1] else ''
        item = (start, end, function_start, function, source)
        if records and records[-1][1] == start and (records[-1][2:] == item[2:]):
            records[-1] = (records[-1][0], end, *item[2:])
        else:
            records.append(item)
    return records


def generate(binary, output=None):
    identity, symbols = elf_info(binary.read_bytes())
    records = make_records(symbols, source_lines(binary))
    output = output or Path(str(binary) + '.crashsym')
    temporary = Path(str(output) + '.tmp')
    with temporary.open('wb') as stream:
        stream.write(HEADER.pack(b'NXUSYM1\x00', identity, len(records)))
        for start, end, function_start, function, source in records:
            stream.write(
                RECORD.pack(start, end, function_start, function.encode()[:95], source.encode()[:127]),
            )
    temporary.replace(output)
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path, help='ELF image, or staged disk root')
    args = parser.parse_args()
    if args.path.is_file():
        generate(args.path)
        return
    cache = {}
    for directory in (
        args.path / 'System/Library/CoreServices',
        args.path / 'Applications',
        args.path / 'System/Library/Frameworks',
    ):
        if not directory.exists():
            continue
        for binary in sorted(directory.rglob('*')):
            if not binary.is_file() or binary.suffix in ('.crashsym', '.tmp'):
                continue
            with binary.open('rb') as stream:
                if stream.read(4) != b'\x7fELF':
                    continue
            output = Path(str(binary) + '.crashsym')
            digest = hashlib.sha256(binary.read_bytes()).digest()
            if digest in cache:
                shutil.copyfile(cache[digest], output)
            else:
                generate(binary, output)
                cache[digest] = output
    print(f'crash symbols: staged {len(cache)} distinct ELF images')


if __name__ == '__main__':
    main()
