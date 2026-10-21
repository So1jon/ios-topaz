/*
 * SWEET eMMC root device (read-only), Exynos 7870 / Galaxy A6.
 *
 * Not built by the shim: tools/mk_emmc_patch.py embeds this file (as bsd/dev/sweet_emmc.c) together with
 * dwmmc.c and gpt.c into xnu/experimental/0230-experimental-exynos-emmc-root.patch.
 *
 * It reuses the state S-BOOT left on the DW-MSHC host (see dwmmc.h), finds one GPT partition by name
 * (boot-arg emmcpart=, default USERDATA) and exposes it as the BSD disk emmcroot0 (remmcroot0) with
 * 512-byte sectors. Root on it with rd=emmcroot0. It never writes: opens for writing and write
 * requests fail with EROFS, so a bug here cannot damage the partition table (cf. RECOVERY.md of the
 * Redmi port). The /sweet-emmc ADT node gives the controller base (tools/mkadt.py --exynos7870).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include "sweet_dwmmc.h"
#include "sweet_gpt.h"

/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define SE_SECTOR	512u
#define SE_BOUNCE_SECT	64u

static struct {
	struct dwmmc h;
	uint64_t first, nsect;		/* partition, in 512-byte sectors */
	uint32_t secsize;		/* logical sector size for the BSD layer */
	int bmajor, cmajor;
	dev_t bdev;
	bool ready;
} se;

static uint32_t se_bounce[SE_BOUNCE_SECT * 128] __attribute__((aligned(64)));

static LCK_GRP_DECLARE(sweet_emmc_grp, "sweet_emmc");
static LCK_MTX_DECLARE(sweet_emmc_lock, &sweet_emmc_grp);

static int
sweet_emmc_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!se.ready) {
		return ENXIO;
	}
	return (flags & FWRITE) ? EROFS : 0;
}

static int
sweet_emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static void
sweet_emmc_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * se.secsize;
	uint32_t count = buf_count(bp);
	uint64_t part_bytes = se.nsect * SE_SECTOR;

	buf_setresid(bp, count);
	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;
		goto done;
	}
	if (off > part_bytes || count > part_bytes - off) {
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}

	lck_mtx_lock(&sweet_emmc_lock);
	for (uint32_t done = 0; done < count;) {
		uint64_t pos = off + done;
		uint32_t skip = (uint32_t)(pos % SE_SECTOR);
		uint32_t want = MIN(count - done, SE_BOUNCE_SECT * SE_SECTOR - skip);
		uint32_t nsec = (skip + want + SE_SECTOR - 1) / SE_SECTOR;
		int rc = dwmmc_read(&se.h, se.first + pos / SE_SECTOR, nsec, se_bounce);
		if (rc) {
			printf("SWEET EMMC: read error %d at sector %llu\n", rc, se.first + pos / SE_SECTOR);
			error = EIO;
			break;
		}
		memcpy(vaddr + done, (const uint8_t *)se_bounce + skip, want);
		done += want;
		buf_setresid(bp, count - done);
	}
	lck_mtx_unlock(&sweet_emmc_lock);
	buf_unmap(bp);

done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
sweet_emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, int is_char)
{
	uint32_t *f = (uint32_t *)data;
	uint64_t *o = (uint64_t *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*f = se.secsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (is_char) {
			return ENODEV;
		}
		if (*f < DEV_BSIZE || *f > 4096 || (*f & (*f - 1))) {
			return EINVAL;
		}
		se.secsize = *f;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = SE_SECTOR;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*o = se.nsect * SE_SECTOR / se.secsize;
		return 0;
	case DKIOCISWRITABLE:
		*f = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = SE_BOUNCE_SECT * SE_SECTOR / se.secsize;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
sweet_emmc_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return sweet_emmc_ioctl(dev, cmd, data, flag, p, 0);
}

static int
sweet_emmc_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return sweet_emmc_ioctl(dev, cmd, data, flag, p, 1);
}

static int
sweet_emmc_psize(__unused dev_t dev)
{
	return se.ready ? (int)MIN(se.nsect * SE_SECTOR / se.secsize, INT_MAX) : -1;
}

static int
sweet_emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(sweet_emmc_strategy, NULL, dev, B_READ, minphys, uio, (int)se.secsize);
}

static int
sweet_emmc_write(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return EROFS;
}

static const struct bdevsw sweet_emmc_bdevsw = {
	.d_open     = sweet_emmc_open,
	.d_close    = sweet_emmc_close,
	.d_strategy = sweet_emmc_strategy,
	.d_ioctl    = sweet_emmc_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = sweet_emmc_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw sweet_emmc_cdevsw = {
	.d_open       = sweet_emmc_open,
	.d_close      = sweet_emmc_close,
	.d_read       = sweet_emmc_read,
	.d_write      = sweet_emmc_write,
	.d_ioctl      = sweet_emmc_cioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static int
sweet_emmc_init(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	char want[64] = "USERDATA";
	uint32_t status = 0, nparts = 0;
	uint64_t first = 0, last = 0;
	int rc;

	if (SecureDTLookupEntry(NULL, "/sweet-emmc", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t)) {
		printf("SWEET EMMC: no /sweet-emmc node in the device tree\n");
		return ENXIO;
	}
	PE_parse_boot_argn("emmcpart", want, sizeof(want));

	vm_offset_t base = ml_io_map(reg[0], reg[1]);
	if (!base) {
		printf("SWEET EMMC: cannot map the host registers\n");
		return ENOMEM;
	}
	dwmmc_init(&se.h, base);
	printf("SWEET EMMC: host at %#llx, VERID %#x, FIFO port +%#x\n", (uint64_t)reg[0],
	    dwmmc_reg(&se.h, DW_VERID), se.h.data_off);

	lck_mtx_lock(&sweet_emmc_lock);
	rc = dwmmc_card_status(&se.h, &status);
	if (rc == DWMMC_ETIMEOUT) {
		se.h.hold = 1;
		rc = dwmmc_card_status(&se.h, &status);
	}
	if (!rc && ((status >> 9) & 0xf) == 3) {	/* stand-by: select the card */
		rc = dwmmc_select(&se.h);
	}
	if (!rc) {
		rc = gpt_find(&se.h, want, &first, &last, &nparts);
	}
	lck_mtx_unlock(&sweet_emmc_lock);
	if (rc) {
		printf("SWEET EMMC: card/GPT lookup of \"%s\" failed: %d (status %#x)\n", want, rc, status);
		return rc == GPT_ENOTFOUND ? ENOENT : EIO;
	}
	se.first = first;
	se.nsect = last - first + 1;
	se.secsize = DEV_BSIZE;

	se.bmajor = bdevsw_add(-1, &sweet_emmc_bdevsw);
	if (se.bmajor < 0) {
		return ENXIO;
	}
	se.cmajor = cdevsw_add_with_bdev(-1, &sweet_emmc_cdevsw, se.bmajor);
	if (se.cmajor < 0) {
		return ENXIO;
	}
	se.bdev = makedev(se.bmajor, 0);
	devfs_make_node(se.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmcroot0");
	devfs_make_node(makedev(se.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmcroot0");
	se.ready = true;
	printf("SWEET EMMC: \"%s\" first sector %llu, %llu MiB, read-only\n", want, first,
	    se.nsect * SE_SECTOR >> 20);
	return 0;
}

/* Root device for rd=emmcroot0 (IOFindBSDRoot). */
int sweet_emmc_rootdev(dev_t *dev);

int
sweet_emmc_rootdev(dev_t *dev)
{
	if (!se.ready) {
		int error = sweet_emmc_init();
		if (error) {
			return error;
		}
	}
	*dev = se.bdev;
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
