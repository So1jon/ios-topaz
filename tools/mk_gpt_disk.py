#!/usr/bin/env python3
"""Build a sparse eMMC-like disk: GPT with a few dummy partitions and a USERDATA partition holding IMAGE.

  mk_gpt_disk.py OUT.img IMAGE [--size-gib 8] [--name USERDATA]

The size is a power of two because QEMU's eMMC model insists on it. For testing the Exynos eMMC root
path (rd=emmcroot0) against QEMU's dw-mshc model; layout mimics a phone: small system partitions first,
USERDATA last, backup GPT at the end of the disk.
"""
import os
import struct
import sys
import uuid
import zlib

SECT = 512
args = sys.argv[1:]
size_gib = 8
name = 'USERDATA'
if '--size-gib' in args:
    i = args.index('--size-gib')
    size_gib = int(args[i + 1])
    del args[i:i + 2]
if '--name' in args:
    i = args.index('--name')
    name = args[i + 1]
    del args[i:i + 2]
out, image = args

total = size_gib << 30
nsect = total // SECT
img_sect = (os.path.getsize(image) + SECT - 1) // SECT
align = 2048                                         # 1 MiB
dummy = [('BOTA0', 8 << 11), ('PARAM', 8 << 11), ('EFS', 16 << 11), ('BOOT', 64 << 11), ('RECOVERY', 64 << 11),
         ('SYSTEM', 128 << 11)]                      # sizes in sectors (MiB << 11)
nent = 128
ent_sectors = nent * 128 // SECT
first_usable = 2 + ent_sectors
last_usable = nsect - 1 - ent_sectors - 1

parts = []
pos = (first_usable + align - 1) // align * align
for n, size in dummy:
    parts.append((n, pos, pos + size - 1))
    pos += size
ud_first = (pos + align - 1) // align * align
ud_last = ud_first + img_sect - 1
assert ud_last <= last_usable, 'image does not fit'
parts.append((name, ud_first, ud_last))


def entries():
    raw = bytearray(nent * 128)
    for i, (n, f, l) in enumerate(parts):
        e = struct.pack('<16s16sQQQ', uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le,
                        uuid.uuid4().bytes_le, f, l, 0) + n.encode('utf-16-le').ljust(72, b'\0')
        raw[i * 128:(i + 1) * 128] = e
    return bytes(raw)


def header(my, other, ent_lba, ent_crc):
    h = struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, 0, my, other, first_usable, last_usable,
                    uuid.UUID(int=0x5eed5eed).bytes_le, ent_lba, nent, 128, ent_crc)
    crc = zlib.crc32(h) & 0xffffffff
    h = h[:16] + struct.pack('<I', crc) + h[20:]
    return h.ljust(SECT, b'\0')


ents = entries()
ecrc = zlib.crc32(ents) & 0xffffffff
mbr = bytearray(SECT)
mbr[446 + 4] = 0xee
mbr[446 + 8:446 + 16] = struct.pack('<II', 1, min(nsect - 1, 0xffffffff))
mbr[510:512] = b'\x55\xaa'

with open(out, 'wb') as f:
    f.truncate(total)
    f.seek(0)
    f.write(mbr)
    f.write(header(1, nsect - 1, 2, ecrc))
    f.write(ents)
    f.seek(ud_first * SECT)
    with open(image, 'rb') as src:
        while True:
            b = src.read(8 << 20)
            if not b:
                break
            f.write(b)
    f.seek((nsect - 1 - ent_sectors) * SECT)
    f.write(ents)
    f.write(header(nsect - 1, 1, nsect - 1 - ent_sectors, ecrc))
for n, a, b in parts:
    print('%-10s %10d..%-10d %6d MiB' % (n, a, b, (b - a + 1) * SECT >> 20))
