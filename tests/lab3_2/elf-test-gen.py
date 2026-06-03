#!/usr/bin/python3

import struct
import sys
from posixpath import basename, splitext

try:
    from elftools.elf.elffile import ELFFile
except ImportError:
    ELFFile = None


class SegmentHeader:
    def __init__(self, p_vaddr, p_filesz, p_memsz):
        self.p_vaddr = p_vaddr
        self.p_filesz = p_filesz
        self.p_memsz = p_memsz


def iter_load_segments(fp):
    if ELFFile is not None:
        elf = ELFFile(fp)
        for seg in elf.iter_segments():
            h = seg.header
            if h.p_type == 'PT_LOAD':
                yield h.p_vaddr, seg.data(), h
        return

    data = fp.read()
    if data[:4] != b'\x7fELF':
        raise ValueError('not an ELF file')
    if data[4] != 1:
        raise ValueError('only ELF32 is supported without pyelftools')

    endian = '<' if data[5] == 1 else '>'
    ehdr = struct.unpack_from(endian + '16sHHIIIIIHHHHHH', data, 0)
    e_phoff = ehdr[5]
    e_phentsize = ehdr[9]
    e_phnum = ehdr[10]

    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        phdr = struct.unpack_from(endian + 'IIIIIIII', data, off)
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, _, _ = phdr
        if p_type == 1:
            seg_data = data[p_offset:p_offset + p_filesz]
            yield p_vaddr, seg_data, SegmentHeader(p_vaddr, p_filesz, p_memsz)


if __name__ == '__main__':
    file = sys.argv[1]
    case = splitext(basename(file))[0]

    segs = []
    with open(file, 'rb') as fp:
        for seg in iter_load_segments(fp):
            segs.append(seg)

    for i, (va, data, _) in enumerate(segs):
        print(f'const char {case}_{va:x}[] = {{')
        for b in data:
            print(f'0x{b:x},', end='', sep='')
        print('\n};\n')

    print(f'''void load_icode_check() {{
    printk("testing load_icode for {case}\\n");
    struct Env *e = ENV_CREATE(test_{case});\
''')

    for i, (va, data, h) in enumerate(segs):
        n = len(data)
        std = f'{case}_{va:x}'
        print(f'''    // Segment at 0x{va:x}, memsz={h.p_memsz}, filesz={h.p_filesz}
    seg_check(e->env_pgdir, 0x{va:x}, {std}, sizeof {std});''')
        if h.p_memsz != n:
            print(f'    seg_check(e->env_pgdir, 0x{va + n:x}, NULL, {h.p_memsz - n});')
    print(f'''    printk("load_icode test for {case} passed!\\n");
}}''')
