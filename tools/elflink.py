#!/usr/bin/env python3
"""Tiny static linker: AArch64 ELF relocatable objects -> flat binary at address 0.

  elflink.py OUT.bin A.o B.o ...

Mirrors shim/shim.ld (the shim is position independent and runs wherever the
bootloader puts it): .text.head first, then .text*, .rodata*, .data*, then
16-aligned .bss/COMMON, a 32 KiB stack, a 4 KiB-aligned block of 8 secondary
stacks, and _end. Exists so the shim builds on a Mac with plain Apple clang
(no aarch64 binutils). Handles the relocations clang -fno-pic emits for this
code; anything else (and any absolute pointer in data) is an error, because the
image has no relocation pass. Linker-script symbols are absolute, so the
arm64 Image header's `.quad _end` (ABS64) is fine.
"""
import struct
import sys

SHN_ABS, SHN_COMMON = 0xfff1, 0xfff2
SHF_ALLOC = 2
SHT_NOBITS, SHT_RELA = 8, 4
STB_LOCAL = 0

R_ABS64, R_PREL32 = 257, 261
R_LD_PREL_LO19, R_ADR_PREL_LO21, R_ADR_PREL_PG_HI21, R_ADR_PREL_PG_HI21_NC = 273, 274, 275, 276
R_ADD_ABS_LO12_NC, R_LDST8, R_TSTBR14, R_CONDBR19, R_JUMP26, R_CALL26 = 277, 278, 279, 280, 282, 283
R_LDST16, R_LDST32, R_LDST64, R_LDST128 = 284, 285, 286, 299


class Section:
    def __init__(self, idx, name, typ, flags, off, size, link, info, align, data):
        self.idx, self.name, self.type, self.flags = idx, name, typ, flags
        self.off, self.size, self.link, self.info, self.align = off, size, link, info, align
        self.data = data
        self.addr = None


class Obj:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        assert d[:4] == b'\x7fELF' and d[4] == 2 and d[5] == 1, f'{path}: not ELF64 LE'
        (_t, machine, _v, _e, _ph, shoff, _f, _eh, _pes, _pn, shentsize, shnum, shstrndx) = \
            struct.unpack_from('<HHIQQQIHHHHHH', d, 16)
        assert machine == 183, f'{path}: not AArch64'
        raw = []
        for i in range(shnum):
            raw.append(struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize))
        stroff = raw[shstrndx][4]

        def cstr(base, off):
            e = d.index(b'\0', base + off)
            return d[base + off:e].decode()

        self.secs = []
        for i, (nm, typ, fl, _ad, off, size, link, info, al, _es) in enumerate(raw):
            data = b'' if typ == SHT_NOBITS else d[off:off + size]
            self.secs.append(Section(i, cstr(stroff, nm), typ, fl, off, size, link, info, max(al, 1), data))
        self.syms = []
        symtab = next(s for s in self.secs if s.type == 2)
        strtab = self.secs[symtab.link]
        for i in range(len(symtab.data) // 24):
            nm, info, _o, shndx, val, size = struct.unpack_from('<IBBHQQ', symtab.data, i * 24)
            self.syms.append((cstr(strtab.off, nm), info >> 4, shndx, val, size))
        self.relas = [s for s in self.secs if s.type == SHT_RELA]


def group(sec):
    n = sec.name
    if n == '.text.head':
        return 0
    if n.startswith('.text'):
        return 1
    if n.startswith('.rodata'):
        return 2
    if n.startswith('.data'):
        return 3
    if n.startswith('.bss'):
        return 4
    return None


def align(x, a):
    return (x + a - 1) // a * a


def main():
    out, paths = sys.argv[1], sys.argv[2:]
    objs = [Obj(p) for p in paths]
    placed = []
    for o in objs:
        for s in o.secs:
            if s.flags & SHF_ALLOC and s.size:
                g = group(s)
                if g is None:
                    sys.exit(f'{o.path}: unsupported allocated section {s.name}')
                placed.append((g, s))
    pos = 0
    for g in (0, 1, 2, 3):
        for gg, s in placed:
            if gg == g:
                pos = align(pos, s.align)
                s.addr = pos
                pos += s.size
    pos = align(pos, 16)
    sym_abs = {'_bss_start': pos}
    for gg, s in placed:
        if gg == 4:
            pos = align(pos, s.align)
            s.addr = pos
            pos += s.size
    # COMMON symbols
    common = {}
    for o in objs:
        for (nm, _b, shndx, val, size) in o.syms:
            if shndx == SHN_COMMON and nm not in common:
                pos = align(pos, val or 8)
                common[nm] = pos
                pos += size
    pos = align(pos, 16)
    sym_abs['_bss_end'] = pos
    pos += 0x8000
    pos = align(pos, 16)
    sym_abs['_stack_top'] = pos
    pos = align(pos, 4096)
    sym_abs['_sec_stacks'] = pos
    pos += 8 * 0x1000
    sym_abs['_end'] = pos
    total = pos

    glob = dict(sym_abs)
    for o in objs:
        for (nm, bind, shndx, val, size) in o.syms:
            if nm and bind != STB_LOCAL and shndx not in (0,):
                if shndx == SHN_ABS:
                    glob[nm] = val
                elif shndx == SHN_COMMON:
                    glob[nm] = common[nm]
                else:
                    s = o.secs[shndx]
                    if s.addr is not None:
                        glob[nm] = s.addr + val

    def sym_addr(o, idx):
        nm, bind, shndx, val, _size = o.syms[idx]
        if shndx == 0:
            if nm in glob:
                return glob[nm], True
            sys.exit(f'{o.path}: undefined symbol {nm}')
        if shndx == SHN_ABS:
            return val, True
        if shndx == SHN_COMMON:
            return common[nm], False
        s = o.secs[shndx]
        if s.addr is None:
            sys.exit(f'{o.path}: symbol {nm or idx} in discarded section {s.name}')
        return s.addr + val, False

    img = bytearray(total)
    for _g, s in placed:
        if s.type != SHT_NOBITS:
            img[s.addr:s.addr + s.size] = s.data

    def u32(p):
        return struct.unpack_from('<I', img, p)[0]

    def w32(p, v):
        struct.pack_into('<I', img, p, v & 0xffffffff)

    def chk(v, bits, what, where):
        lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        if not lo <= v <= hi:
            sys.exit(f'relocation overflow: {what} at {where}: {v}')

    nrel = 0
    for o in objs:
        for rs in o.relas:
            tgt = o.secs[rs.info]
            if tgt.addr is None:
                continue
            for i in range(len(rs.data) // 24):
                off, info, add = struct.unpack_from('<QQq', rs.data, i * 24)
                typ, si = info & 0xffffffff, info >> 32
                S, is_abs = sym_addr(o, si)
                P = tgt.addr + off
                V = S + add
                where = f'{o.path}:{tgt.name}+{off:#x}'
                ins = u32(P) if typ not in (R_ABS64, R_PREL32) else 0
                if typ == R_ABS64:
                    if not is_abs:
                        sys.exit(f'{where}: absolute pointer to a relocatable symbol (image has no relocation pass)')
                    struct.pack_into('<Q', img, P, V)
                elif typ == R_PREL32:
                    struct.pack_into('<i', img, P, V - P)
                elif typ == R_ADR_PREL_LO21:
                    d = V - P
                    chk(d, 21, 'ADR_PREL_LO21', where)
                    w32(P, (ins & ~((3 << 29) | (0x7ffff << 5))) | ((d & 3) << 29) | (((d >> 2) & 0x7ffff) << 5))
                elif typ in (R_ADR_PREL_PG_HI21, R_ADR_PREL_PG_HI21_NC):
                    d = (V >> 12) - (P >> 12)
                    chk(d, 21, 'ADR_PREL_PG_HI21', where)
                    w32(P, (ins & ~((3 << 29) | (0x7ffff << 5))) | ((d & 3) << 29) | (((d >> 2) & 0x7ffff) << 5))
                elif typ == R_ADD_ABS_LO12_NC:
                    w32(P, (ins & ~(0xfff << 10)) | ((V & 0xfff) << 10))
                elif typ in (R_LDST8, R_LDST16, R_LDST32, R_LDST64, R_LDST128):
                    sh = {R_LDST8: 0, R_LDST16: 1, R_LDST32: 2, R_LDST64: 3, R_LDST128: 4}[typ]
                    if (V & 0xfff) & ((1 << sh) - 1):
                        sys.exit(f'{where}: misaligned LDST target')
                    w32(P, (ins & ~(0xfff << 10)) | (((V & 0xfff) >> sh) << 10))
                elif typ in (R_JUMP26, R_CALL26):
                    d = (V - P) >> 2
                    chk(d, 26, 'CALL26/JUMP26', where)
                    w32(P, (ins & ~0x3ffffff) | (d & 0x3ffffff))
                elif typ in (R_CONDBR19, R_LD_PREL_LO19):
                    d = (V - P) >> 2
                    chk(d, 19, 'CONDBR19/LD_PREL_LO19', where)
                    w32(P, (ins & ~(0x7ffff << 5)) | ((d & 0x7ffff) << 5))
                elif typ == R_TSTBR14:
                    d = (V - P) >> 2
                    chk(d, 14, 'TSTBR14', where)
                    w32(P, (ins & ~(0x3fff << 5)) | ((d & 0x3fff) << 5))
                else:
                    sys.exit(f'{where}: unsupported relocation type {typ}')
                nrel += 1
    open(out, 'wb').write(img)
    print(f'{out}: {total} bytes, {nrel} relocations, _end={total:#x}', file=sys.stderr)
    for k in ('_bss_start', '_bss_end', '_stack_top', '_sec_stacks'):
        print(f'  {k} = {sym_abs[k]:#x}', file=sys.stderr)


if __name__ == '__main__':
    main()
