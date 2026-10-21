#!/usr/bin/env python3
"""Minimal Android boot image (header v0) writer for Samsung Exynos boards.

  mkbootimg_v0.py --kernel Image [--ramdisk FILE] -o boot.img
        [--base 0x10000000] [--kernel_offset 0x8000] [--ramdisk_offset 0x1000000]
        [--tags_offset 0x100] [--pagesize 2048] [--cmdline STR] [--seandroid]
        [--dt dt.img] [--name SRPRA24A008RU] [--os_version 0x14000162]

Defaults match the A6 (a6lte) stock images: base 0x10000000, kernel_offset 0x8000, ramdisk_offset
0x1000000, tags_offset 0x100, page 2048 (OrangeFox BoardConfig.mk and the A600FJXU9CVB1 firmware dump).
S-BOOT picks the DTB from the separate `dt` section (a Samsung "DTBH" container, one DTB per hardware
revision), so --dt is required for a bootable image. --seandroid appends the "SEANDROIDENFORCE" footer
Samsung images carry. tools/bootimg_info.py prints an image's header.
"""
import argparse
import hashlib
import struct


def pad(b, page):
    return b + b'\0' * (-len(b) % page)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--base', type=lambda x: int(x, 0), default=0x10000000)
    ap.add_argument('--kernel_offset', type=lambda x: int(x, 0), default=0x00008000)
    ap.add_argument('--ramdisk_offset', type=lambda x: int(x, 0), default=0x01000000)
    ap.add_argument('--tags_offset', type=lambda x: int(x, 0), default=0x00000100)
    ap.add_argument('--pagesize', type=int, default=2048)
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--name', default='')
    ap.add_argument('--seandroid', action='store_true')
    ap.add_argument('--dt', help='Samsung DTBH device tree container (the boot image dt section)')
    ap.add_argument('--os_version', type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()

    kernel = open(a.kernel, 'rb').read()
    ramdisk = open(a.ramdisk, 'rb').read() if a.ramdisk else b''
    dt = open(a.dt, 'rb').read() if a.dt else b''
    if dt:
        assert dt[:4] == b'DTBH', 'dt is not a Samsung DTBH container'
    cmd = a.cmdline.encode()
    assert len(cmd) < 512 + 1024, 'cmdline too long'
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b'', dt):
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    hdr = b'ANDROID!'
    hdr += struct.pack('<10I', len(kernel), a.base + a.kernel_offset, len(ramdisk), a.base + a.ramdisk_offset,
                       0, a.base + 0x00f00000, a.base + a.tags_offset, a.pagesize, len(dt), a.os_version)
    hdr += a.name.encode().ljust(16, b'\0')[:16]
    hdr += cmd[:512].ljust(512, b'\0')
    hdr += sha.digest().ljust(32, b'\0')
    hdr += cmd[512:].ljust(1024, b'\0')
    assert len(hdr) == 1632
    img = pad(hdr, a.pagesize) + pad(kernel, a.pagesize) + pad(ramdisk, a.pagesize) + pad(dt, a.pagesize)
    if a.seandroid:
        img += b'SEANDROIDENFORCE'
    open(a.output, 'wb').write(img)
    print(f'{a.output}: {len(img)} bytes, kernel {len(kernel)} @ {a.base + a.kernel_offset:#x}, '
          f'ramdisk {len(ramdisk)} @ {a.base + a.ramdisk_offset:#x}, dt {len(dt)}, page {a.pagesize}')


if __name__ == '__main__':
    main()
