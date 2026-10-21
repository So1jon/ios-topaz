#!/bin/bash
# Build the Exynos 7870 shim on macOS with Apple clang + tools/elflink.py (no aarch64 binutils needed).
#
#   EXY_TARGET=a6|qemu  shim/build-exynos.sh [PAYLOAD.bin]
#
# a6   : framebuffer 0x67000000, 720x1480 (Galaxy J6 board values, NOT verified for the A6)
# qemu : framebuffer 0x9c000000, 1080x2400 (tools/qemu-sweet.py --shim)
# With a payload (tools/mkpayload.py output) it is appended to the Image at the first 4K boundary
# after _end, as for topaz, and image_size in the arm64 header grows to cover it. Output:
# shim/out-exynos/Image, and for a6 also boot.img (tools/mkbootimg_v0.py, unverified defaults).
set -euo pipefail
cd "$(dirname "$0")"
OUT=out-exynos
mkdir -p "$OUT"
case "${EXY_TARGET:-a6}" in
a6)   FBDEF="-DFB_BASE=0x67000000UL -DFB_W=720 -DFB_H=1480" ;;
qemu) FBDEF="-DFB_BASE=0x9c000000UL -DFB_W=1080 -DFB_H=2400" ;;
*) echo "EXY_TARGET must be a6 or qemu" >&2; exit 2 ;;
esac
CF="--target=aarch64-none-elf -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align \
-fno-stack-protector -fno-jump-tables -fno-builtin -Wall -Wextra $FBDEF -I."
clang $CF -c exynos/start.S -o $OUT/start.o
# EXY_MODE=probe: read-only eMMC/GPT probe instead of loading XNU (nothing is written to storage)
EXTRA=""
OBJS="$OUT/start.o $OUT/main.o $OUT/console.o $OUT/fdt.o $OUT/loader.o"
if [ "${EXY_MODE:-boot}" = probe ]; then
	EXTRA="-DEXY_PROBE"
	OBJS="$OBJS $OUT/probe.o $OUT/dwmmc.o $OUT/fdtdump.o"
fi
for f in exynos/main.c console.c fdt.c loader.c exynos/probe.c exynos/dwmmc.c exynos/fdtdump.c; do
	clang $CF $EXTRA -Iexynos -c "$f" -o "$OUT/$(basename "${f%.c}").o"
done
python3 -I ../tools/elflink.py $OUT/Image $OBJS
if [ -n "${1:-}" ]; then
	size=$(stat -f %z $OUT/Image)
	padded=$(( (size + 0xfff) & ~0xfff ))
	truncate -s "$padded" $OUT/Image
	cat "$1" >> $OUT/Image
	python3 -I -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' $OUT/Image
fi
if [ "${EXY_TARGET:-a6}" = a6 ]; then
	# a bootloader may refuse a zero-size ramdisk: ship an empty newc cpio archive
	(cd "$OUT" && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet 2>/dev/null | gzip -9 > ../ramdisk.bin)
	# S-BOOT takes the DTB from the image's own dt section (Samsung DTBH container). The A6's stock one is
	# public (A600FJXU9CVB1 dump boot/dt.img); keep it outside git and point A6_DT at it.
	A6_DT=${A6_DT:-../../a6-artifacts/ref/dt.img}
	[ -f "$A6_DT" ] || { echo "error: A6_DT ($A6_DT) not found: the boot image needs the A6 dt.img" >&2; exit 1; }
	python3 -I ../tools/mkbootimg_v0.py --kernel $OUT/Image --ramdisk $OUT/ramdisk.bin --dt "$A6_DT" \
		--name "${BOARD_NAME:-SRPRA24A008RU}" --os_version 0x14000162 \
		-o $OUT/${BOOTNAME:-boot}.img --seandroid --cmdline "console=null"
fi
ls -l $OUT/Image
