#!/usr/bin/env python3
"""Print the header of an Android boot image (v0..v2): use it on the A6's STOCK boot.img / recovery.img
to get the real kernel/ramdisk addresses, page size and cmdline before building ours.

  bootimg_info.py boot.img
"""
import struct
import sys


def main():
    d = open(sys.argv[1], 'rb').read()
    if d[:8] != b'ANDROID!':
        sys.exit('not an Android boot image (magic ANDROID! missing)')
    (ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page, ver, osv) = struct.unpack_from('<10I', d, 8)
    name = d[48:64].split(b'\0')[0].decode(errors='replace')
    cmd = d[64:576].split(b'\0')[0].decode(errors='replace')
    extra = d[608:1632].split(b'\0')[0].decode(errors='replace')
    print(f'header/dt_size field : {ver:#x}  os_version {osv:#x}')
    print(f'page size            : {page}')
    print(f'kernel               : size {ksz:#x} addr {kaddr:#x}')
    print(f'ramdisk              : size {rsz:#x} addr {raddr:#x}')
    print(f'second               : size {ssz:#x} addr {saddr:#x}')
    print(f'tags                 : addr {taddr:#x}')
    print(f'name                 : {name!r}')
    print(f'cmdline              : {cmd!r} {extra!r}')
    base = kaddr - 0x8000
    print(f'implied base         : {base:#x} (if kernel_offset is 0x8000)  ramdisk_offset {raddr - base:#x}  tags_offset {taddr - base:#x}')
    print('SEANDROIDENFORCE     :', d.rstrip(b'\0').endswith(b'SEANDROIDENFORCE'))
    kimg = d[page:page + 64]
    if kimg[56:60] == b'ARM\x64':
        print('kernel is an arm64 Image, text_offset', struct.unpack_from('<Q', kimg, 8)[0])


if __name__ == '__main__':
    main()
