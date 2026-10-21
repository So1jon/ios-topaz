#!/usr/bin/env python3
"""Generate xnu/experimental/0230-experimental-exynos-emmc-root.patch.

  mk_emmc_patch.py OUT.patch [--tree DIR]

Embeds the shim's portable eMMC code (shim/exynos/dwmmc.[ch], gpt.[ch]) and the BSD wrapper
(shim/exynos/xnu_sweet_emmc.c) as bsd/dev/sweet_{dwmmc,gpt,emmc}.* so the kernel and the shim probe
share one driver, and hooks rd=emmcroot0 into IOFindBSDRoot. DIR is a pre-0200 xnu build tree (the
CI "xnu-buildtree" artifact, directory xnu/); the 0220 virtio-blk patch is applied to scratch copies
first because the hunks here sit next to its hooks.
"""
import difflib
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
EXY = os.path.join(REPO, 'shim', 'exynos')
P0220 = os.path.join(REPO, 'xnu', 'experimental', '0220-experimental-virtio-blk-root.patch')
TREE = '/Volumes/DATA/Downloads/xnu4k-artifacts/bt/xnu'

args = [a for a in sys.argv[1:]]
tree = TREE
if '--tree' in args:
    i = args.index('--tree')
    tree = args[i + 1]
    del args[i:i + 2]
out = args[0]

work = tempfile.mkdtemp()
for rel in ('bsd/conf/files', 'iokit/bsddev/IOKitBSDInit.cpp'):
    os.makedirs(os.path.dirname(os.path.join(work, rel)), exist_ok=True)
    shutil.copy(os.path.join(tree, rel), os.path.join(work, rel))
subprocess.run(['patch', '-p1', '-s', '-i', P0220], cwd=work, check=True)


def edit(rel, subs):
    path = os.path.join(work, rel)
    old = open(path).read()
    new = old
    for a, b in subs:
        assert new.count(a) == 1, (rel, a[:60], new.count(a))
        new = new.replace(a, b)
    d = difflib.unified_diff(old.splitlines(True), new.splitlines(True), 'a/' + rel, 'b/' + rel, n=3)
    return 'diff --git a/%s b/%s\n' % (rel, rel) + ''.join(d)


def newfile(rel, text):
    lines = text.splitlines(True)
    if lines and not lines[-1].endswith('\n'):
        lines[-1] += '\n'
    return ('diff --git a/%s b/%s\nnew file mode 100644\n--- /dev/null\n+++ b/%s\n@@ -0,0 +1,%d @@\n' %
            (rel, rel, rel, len(lines))) + ''.join('+' + l for l in lines)


def src(name, subs=()):
    t = open(os.path.join(EXY, name)).read()
    for a, b in subs:
        assert a in t, (name, a)
        t = t.replace(a, b)
    return t


patch = ''
patch += edit('bsd/conf/files', [(
    'bsd/dev/sweet_vblk.c\t\t\tstandard\n',
    'bsd/dev/sweet_vblk.c\t\t\tstandard\nbsd/dev/sweet_emmc.c\t\t\tstandard\n'
    'bsd/dev/sweet_dwmmc.c\t\t\tstandard\nbsd/dev/sweet_gpt.c\t\t\tstandard\n'
    'bsd/dev/sweet_cowovl.c\t\t\tstandard\n')])
patch += edit('iokit/bsddev/IOKitBSDInit.cpp', [(
    '\tif (strcmp(rdBootVar, "vblk0") == 0) {\n',
    '''\tif (strcmp(rdBootVar, "emmcroot0") == 0) {
		extern int sweet_emmc_rootdev(dev_t *dev);
		OSSafeReleaseNULL(matching);
		int em_error = sweet_emmc_rootdev(root);
		if (em_error) {
			panic("IOFindBSDRoot: SWEET eMMC root %s unavailable: %d", rdBootVar, em_error);
		}
		strlcpy(rootName, rdBootVar, rootNameSize);
		IOLog("BSD root: %s, major %d, minor %d\\n", rootName, major(*root), minor(*root));
		*oflags = 0;
		goto iofrootx;
	}
	if (strcmp(rdBootVar, "vblk0") == 0) {
''')])
patch += newfile('bsd/dev/sweet_dwmmc.h', src('dwmmc.h'))
patch += newfile('bsd/dev/sweet_dwmmc.c', src('dwmmc.c', [('#include "dwmmc.h"', '#include "sweet_dwmmc.h"')]))
patch += newfile('bsd/dev/sweet_gpt.h', src('gpt.h', [('#include "dwmmc.h"', '#include "sweet_dwmmc.h"')]))
patch += newfile('bsd/dev/sweet_gpt.c', src('gpt.c', [('#include "gpt.h"', '#include "sweet_gpt.h"')]))
patch += newfile('bsd/dev/sweet_cowovl.h', src('cowovl.h'))
patch += newfile('bsd/dev/sweet_cowovl.c', src('cowovl.c', [('#include "cowovl.h"', '#include "sweet_cowovl.h"')]))
patch += newfile('bsd/dev/sweet_emmc.c', src('xnu_sweet_emmc.c'))
open(out, 'w').write(patch)
shutil.rmtree(work)
print('wrote', out, len(patch), 'bytes')
