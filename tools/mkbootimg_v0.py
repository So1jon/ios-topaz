#!/usr/bin/env python3
"""Minimal Android boot image (header v0) writer for Samsung Exynos boards.

  mkbootimg_v0.py --kernel Image [--ramdisk FILE] -o boot.img
        [--base 0x10000000] [--kernel_offset 0x8000] [--ramdisk_offset 0x1000000]
        [--tags_offset 0x100] [--pagesize 2048] [--cmdline STR] [--seandroid]

Defaults are the values commonly seen on Exynos 7870 devices (Galaxy J7/A6 era). They are NOT
verified for the A6: compare with the header of the stock boot.img before flashing anything
(tools/bootimg_info.py prints it). --seandroid appends the "SEANDROIDENFORCE" footer Samsung
images carry.
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
    a = ap.parse_args()

    kernel = open(a.kernel, 'rb').read()
    ramdisk = open(a.ramdisk, 'rb').read() if a.ramdisk else b''
    cmd = a.cmdline.encode()
    assert len(cmd) < 512 + 1024, 'cmdline too long'
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b''):
        sha.update(blob)
        sha.update(struct.pack('<I', len(blob)))
    hdr = b'ANDROID!'
    hdr += struct.pack('<10I', len(kernel), a.base + a.kernel_offset, len(ramdisk), a.base + a.ramdisk_offset,
                       0, a.base + 0x00f00000, a.base + a.tags_offset, a.pagesize, 0, 0)
    hdr += a.name.encode().ljust(16, b'\0')[:16]
    hdr += cmd[:512].ljust(512, b'\0')
    hdr += sha.digest().ljust(32, b'\0')
    hdr += cmd[512:].ljust(1024, b'\0')
    assert len(hdr) == 1632
    img = pad(hdr, a.pagesize) + pad(kernel, a.pagesize) + pad(ramdisk, a.pagesize)
    if a.seandroid:
        img += b'SEANDROIDENFORCE'
    open(a.output, 'wb').write(img)
    print(f'{a.output}: {len(img)} bytes, kernel {len(kernel)} @ {a.base + a.kernel_offset:#x}, '
          f'ramdisk {len(ramdisk)} @ {a.base + a.ramdisk_offset:#x}, page {a.pagesize}')


if __name__ == '__main__':
    main()
