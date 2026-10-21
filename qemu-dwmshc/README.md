# QEMU DW-MSHC model (test only)

`dw_mshc.c` is a behavioural Synopsys DW-MSHC host (PIO read path, CMD12 auto-stop, RXDR/DTO) for QEMU 11.1.2, used to
run the real kernel eMMC driver (`rd=emmcroot0`, `shim/exynos/dwmmc.c` + `gpt.c`) without the phone. It was written from the
same databook reading as the driver, so it finds logic/integration bugs, not silicon surprises.

Apply to a QEMU 11.1.2 tree:
1. copy `dw_mshc.c` to `hw/sd/`; `hw/sd/Kconfig`: `config DW_MSHC` (`bool`, `select SD`); `hw/sd/meson.build`:
   `system_ss.add(when: 'CONFIG_DW_MSHC', if_true: files('dw_mshc.c'))`; `hw/arm/Kconfig` ARM_VIRT: `select DW_MSHC`.
2. `hw/arm/virt.c` after `create_virtio_devices(vms)`: if `drive_get(IF_SD, 0, 0)`, create `dw-mshc` at 0x09100000
   (SPI 40), add a `TYPE_EMMC` card on its `sd-bus` with that drive (pattern of hw/arm/aspeed.c). Includes: hw/sd/sd.h,
   system/block-backend.h, system/blockdev.h.
3. configure needs `--extra-cflags=-I/opt/local/include --extra-ldflags=-L/opt/local/lib` (MacPorts libfdt) and, offline,
   an emptied `[tooling]` group in pythondeps.toml.
Run: `tools/mk_gpt_disk.py emmc.img rootfs.img` (8 GiB, power of two), ADT with `SWEET_EMMC_PA=0x09100000 tools/mkadt.py ... --qemu --gicv2`,
`tools/qemu-sweet.py --emmc emmc.img --qemu-bin .../qemu-system-aarch64`.

Result (k4l kernel, 2026-10-21): GPT lookup finds USERDATA, HFS mounts from emmcroot0, launchd runs; then the early
`mount` job exits 1 (root is read-only, `mount -uw /` -> EROFS) and the kernel panics "mount[3] exited". iOS needs a writable root.
