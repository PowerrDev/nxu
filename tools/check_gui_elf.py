#!/usr/bin/env python3
"""Check final GUI ELF validation against deliberately malformed artifacts."""
import argparse
from pathlib import Path
import struct
import tempfile
from gui_elf import inspect


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    assert not inspect(args.image)['errors']
    data = args.image.read_bytes()
    elf32 = data[4] == 1
    word = '<I' if elf32 else '<Q'
    phoff = struct.unpack_from(word, data, 28 if elf32 else 32)[0]
    count_offset = 44 if elf32 else 56
    count = struct.unpack_from('<H', data, count_offset)[0]
    headers = [phoff + i * (32 if elf32 else 56) for i in range(count)]
    loads = [p for p in headers if struct.unpack_from('<I', data, p)[0] == 1]
    relro = next(p for p in headers if struct.unpack_from('<I', data, p)[0] == 0x6474e552)
    flags_offset = 24 if elf32 else 4
    address_offset = 8 if elf32 else 16
    file_offset = 4 if elf32 else 8
    memory_size_offset = 20 if elf32 else 40
    work = Path(tempfile.mkdtemp(prefix='nxu-gui-elf-check-', dir='/private/tmp'))

    def check(name, mutate):
        changed = bytearray(data)
        mutate(changed)
        path = work / (name + '.so')
        path.write_bytes(changed)
        assert inspect(path)['errors'], name + ' was incorrectly accepted'

    check('wrong-machine', lambda x: struct.pack_into('<H', x, 18, 62))
    check('too-many-headers', lambda x: struct.pack_into('<H', x, count_offset, 17))
    executable = next((p for p in loads if struct.unpack_from('<I', data, p + flags_offset)[0] & 1))
    check('writable-text', lambda x: struct.pack_into('<I', x, executable + flags_offset, 7))
    check('TLS', lambda x: struct.pack_into('<I', x, headers[-1], 7))

    def overlap(x):
        struct.pack_into(word, x, loads[1] + file_offset, 0)
        struct.pack_into(word, x, loads[1] + address_offset, 0)

    check('overlapping-pages', overlap)

    def relro_in_text(x):
        text = struct.unpack_from(word, x, executable + address_offset)[0]
        struct.pack_into(word, x, relro + address_offset, text)
        struct.pack_into(word, x, relro + memory_size_offset, 4096 - text % 4096)

    check('RELRO-in-text', relro_in_text)
    print('GUI ELF validation: valid image and six malformed artifacts checked')


if __name__ == '__main__':
    main()
