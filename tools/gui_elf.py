"""Final ELF checks matching the ELF32 and ELF64 dyrld limits (no third-party modules)."""
from pathlib import Path
import struct


def inspect(path):
    data = Path(path).read_bytes()
    errors = []

    def require(test, message):
        if not test:
            errors.append(message)

    elf32 = len(data) >= 52 and data[:7] == b'\x7fELF\x01\x01\x01'
    require(elf32 or (len(data) >= 64 and data[:7] == b'\x7fELF\x02\x01\x01'), 'requires little-endian ELF32/ELF64')
    if errors:
        return {'errors': errors}
    _, kind, machine, _, _, phoff, _, _, _, phsize, count, *_ = struct.unpack_from('<16sHHIIIIIHHHHHH' if elf32 else '<16sHHIQQQIHHHHHH', data)
    require(kind == 3 and machine == (3 if elf32 else 183), 'requires native ET_DYN')
    expected_phsize = 32 if elf32 else 56
    require(
        phsize == expected_phsize and 0 < count <= 16 and (phoff + phsize * count <= len(data)),
        'invalid program headers',
    )
    if errors:
        return {'errors': errors}
    headers = []
    for i in range(count):
        if elf32:
            kind, offset, address, physical, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + i * phsize)
            headers.append((kind, flags, offset, address, physical, filesz, memsz, align))
        else:
            headers.append(struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize))
    loads = [h for h in headers if h[0] == 1 and h[6]]
    pages = []
    for _, flags, offset, address, _, filesz, memsz, align in loads:
        require(filesz <= memsz and offset + filesz <= len(data), 'invalid segment file range')
        require(align <= 65536 and (align < 2 or not align & align - 1), 'unsupported segment alignment')
        require(offset % 4096 == address % 4096, 'segment page offset mismatch')
        require(not (flags & 1 and flags & 2), 'writable executable segment')
        require(flags & 4 and (not flags & ~7), 'unsupported segment flags')
        pages.append((address & ~4095, address + memsz + 4095 & ~4095))
    require(bool(loads), 'missing load segments')
    if pages:
        require(
            max((e for _, e in pages)) - min((s for s, _ in pages)) <= 32 * 1024 * 1024,
            'image span exceeds 32 MiB',
        )
        require(
            all((b <= c for (_, b), (c, _) in zip(sorted(pages), sorted(pages)[1:]))),
            'overlapping load pages',
        )
    require(not any((h[0] == 7 for h in headers)), 'TLS unsupported')
    relros = [h for h in headers if h[0] == 0x6474e552]
    require(len(relros) <= 1, 'duplicate RELRO headers')
    for h in relros:
        start, end = (h[3], h[3] + h[6])
        require(h[6] > 0 and end % 4096 == 0, 'invalid RELRO range')
        require(
            any(
                ((
                    flags & 2
                    and (not flags & 1)
                    and (address <= start)
                    and (end <= address + memsz + 4095 & ~4095)
                ) for _, flags, _, address, _, _, memsz, _ in loads),
            ),
            'RELRO outside writable non-executable segment',
        )

    def readva(address, size):
        for _, _, offset, start, _, filesz, _, _ in loads:
            if start <= address and address + size <= start + filesz:
                return data[offset + address - start:offset + address - start + size]
        raise ValueError('dynamic data outside file-backed segments')

    try:
        dyn = [h for h in headers if h[0] == 2]
        require(len(dyn) == 1, 'requires one dynamic table')
        if len(dyn) != 1:
            return {'errors': errors}
        require(0 < dyn[0][6] <= 65536, 'invalid dynamic table size')
        raw = readva(dyn[0][3], dyn[0][5])
        tags = {}
        needed = []
        terminated = False
        dynamic_size = 8 if elf32 else 16
        for offset in range(0, len(raw) - dynamic_size + 1, dynamic_size):
            tag, value = struct.unpack_from('<iI' if elf32 else '<qQ', raw, offset)
            if tag == 0:
                terminated = True
                break
            if tag == 1:
                needed.append(value)
            else:
                tags[tag] = value
        require(terminated, 'unterminated dynamic table')
        unsupported = ({7, 8, 9} if elf32 else {17, 18, 19}) | {16, 22, 35, 36, 37, 0x6ffffff0, 0x6ffffffc, 0x6ffffffe}
        require(
            not unsupported.intersection(tags),
            'symbol versions, SYMBOLIC, text relocations, REL or RELR unsupported',
        )
        require(not tags.get(30, 0) & 6, 'DF_SYMBOLIC/DF_TEXTREL unsupported')
        require(4 in tags and 5 in tags and (6 in tags), 'missing SysV hash/string/symbol tables')
        if errors:
            return {'errors': errors}
        buckets, symbols = struct.unpack('<II', readva(tags[4], 8))
        require(0 < symbols <= 65536 and buckets <= 65536, 'invalid SysV hash size')
        readva(tags[4], (buckets + symbols + 2) * 4)
        strings = readva(tags[5], tags.get(10, 0))
        require(len(strings) <= 1024 * 1024, 'string table exceeds loader limit')

        def string(index):
            end = strings.find(b'\x00', index)
            if not 0 <= index < len(strings) or end < 0:
                raise ValueError('invalid dynamic string')
            return strings[index:end].decode('ascii')

        imports, exports = ([], [])
        symbol_size = 16 if elf32 else 24
        require(tags.get(11, symbol_size) == symbol_size, 'unsupported symbol entry size')
        rawsymbols = readva(tags[6], symbols * symbol_size)
        for i in range(symbols):
            if elf32:
                name, _, _, info, _, section = struct.unpack_from('<IIIBBH', rawsymbols, i * symbol_size)
            else:
                name, info, _, section, _, _ = struct.unpack_from('<IBBHQQ', rawsymbols, i * symbol_size)
            require(info & 15 not in (6, 10), 'TLS/IFUNC symbol unsupported')
            if name:
                (exports if section else imports).append(string(name))
        relocations = set()
        relocation_size = 8 if elf32 else 24
        word_size = 4 if elf32 else 8
        shift = 8 if elf32 else 32
        relative = 8 if elf32 else 1027
        for table, size in (((17, 18) if elf32 else (7, 8)), (23, 2)):
            length = tags.get(size, 0)
            require(length % relocation_size == 0 and length <= 1024 * 1024, 'invalid relocation size')
            if length:
                tag, entry_tag = (17, 19) if elf32 else (7, 9)
                require(tags.get(entry_tag, relocation_size) == relocation_size and tags.get(20, tag) == tag, 'invalid relocation format')
                rawrel = readva(tags[table], length)
                for offset in range(0, length, relocation_size):
                    record = struct.unpack_from('<II' if elf32 else '<QQq', rawrel, offset)
                    address, info = record[:2]
                    rtype = info & ((1 << shift) - 1)
                    relocations.add(rtype)
                    if not rtype:
                        continue
                    require(rtype in ((0, 1, 6, 7, 8) if elf32 else (0, 257, 1025, 1026, 1027)), 'unsupported dynamic relocation')
                    require(address % word_size == 0, 'unaligned relocation target')
                    require(rtype != relative or info >> shift == 0, 'relative relocation has a symbol')
                    require(info >> shift < symbols, 'relocation symbol outside table')
                    require(
                        any(
                            (flags & 2 and start <= address and (address + word_size <= start + memsz) for _, flags, _, start, _, _, memsz, _ in loads),
                        ),
                        'relocation targets nonwritable segment',
                    )
        return {
            'errors': sorted(set(errors)),
            'imports': sorted(set(imports)),
            'exports': sorted(set(exports)),
            'needed': [string(i) for i in needed],
            'relocations': sorted(relocations),
            'relro': any((h[0] == 0x6474e552 for h in headers)),
            'program_headers': count,
        }
    except (ValueError, KeyError, struct.error, UnicodeError) as error:
        return {'errors': sorted(set(errors + [str(error)]))}
