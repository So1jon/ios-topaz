/*
 * Read-only first-contact probe for the Exynos 7870 eMMC host. Shows on the framebuffer, step by
 * step (the last STEP line on screen tells where it stopped if the SoC hangs):
 *   host registers -> CMD13 card status -> LBA0/LBA1 -> GPT partition list.
 * It never writes to the card; it only resets the host FIFO and clears interrupt status.
 */
#include <stdint.h>
#include "console.h"
#include "dwmmc.h"
#include "probe.h"

#define MMC0_BASE	0x13540000UL	/* mainline exynos7870.dtsi mmc0 (eMMC) */

static uint8_t sec[512];
static uint8_t gpt[32 * 512];		/* 128 entries x 128 bytes */

static uint32_t le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static const char *rc_str(int rc)
{
	switch (rc) {
	case 0: return "ok";
	case DWMMC_ETIMEOUT: return "TIMEOUT";
	case DWMMC_ECMD: return "CMD ERROR";
	case DWMMC_EDATA: return "DATA ERROR";
	default: return "BAD ARG";
	}
}

/* NUL-separated list, not an array of pointers: the image has no relocation pass */
static int want(const char *n)
{
	static const char keep[] = "BOOT\0RECOVERY\0SYSTEM\0USERDATA\0CACHE\0VENDOR\0ODM\0EFS\0PARAM\0"
				   "PERSIST\0HIDDEN\0DTBO\0";
	for (const char *k = keep; *k; ) {
		int j = 0;
		while (k[j] && n[j] == k[j])
			j++;
		if (!k[j] && !n[j])
			return 1;
		while (*k)
			k++;
		k++;
	}
	return 0;
}

void probe_run(void)
{
	struct dwmmc h;
	uint32_t st = 0;
	int rc;

	/* unmask SError: a bus error from a gated/absent controller then shows on screen instead of hanging */
	__asm__ volatile("msr daifclr, #4" ::: "memory");
	con_str("PROBE", "read-only eMMC probe");
	con_str("STEP", "verid (first MMIO)");
	dwmmc_init(&h, MMC0_BASE);
	con_hex2("VERID", "ver=", dwmmc_reg(&h, DW_VERID), "hcon=", dwmmc_reg(&h, DW_HCON));
	con_str("STEP", "registers");
	con_hex2("CTRL", "ctl=", dwmmc_reg(&h, DW_CTRL), "sts=", dwmmc_reg(&h, DW_STATUS));
	con_hex2("CLK", "ena=", dwmmc_reg(&h, DW_CLKENA), "div=", dwmmc_reg(&h, DW_CLKDIV));
	con_hex2("BUS", "typ=", dwmmc_reg(&h, DW_CTYPE), "tmo=", dwmmc_reg(&h, DW_TMOUT));
	con_hex2("MODE", "uhs=", dwmmc_reg(&h, DW_UHS), "fif=", dwmmc_reg(&h, DW_FIFOTH));
	con_hex2("MISC", "cdt=", dwmmc_reg(&h, DW_CDETECT), "pwr=", dwmmc_reg(&h, DW_PWREN));
	con_str("STEP", "CMD13 card status");
	rc = dwmmc_card_status(&h, &st);
	if (rc == DWMMC_ETIMEOUT) {
		h.hold = 1;
		con_str("RETRY", "with USE_HOLD_REG");
		rc = dwmmc_card_status(&h, &st);
	}
	con_hex2("CMD13", "rc=", (uint32_t)rc, "st=", st);
	con_str("", rc_str(rc));
	if (rc)
		return;
	con_hex2("CARD", "state=", (st >> 9) & 0xf, "hold=", h.hold);
	if (((st >> 9) & 0xf) == 3) {
		rc = dwmmc_select(&h);
		con_hex2("CMD7", "rc=", (uint32_t)rc, "", 0);
	}
	con_str("STEP", "read LBA 0 and 1");
	rc = dwmmc_read(&h, 0, 1, sec);
	con_hex2("LBA0", "rc=", (uint32_t)rc, "sig=", sec[510] | sec[511] << 8);
	if (rc)
		return;
	rc = dwmmc_read(&h, 1, 1, sec);
	con_hex2("LBA1", "rc=", (uint32_t)rc, "magic=", le32(sec));
	if (rc)
		return;
	if (le32(sec) != 0x20494645) {	/* "EFI " */
		con_str("GPT", "no EFI PART signature");
		return;
	}
	uint32_t nent = le32(sec + 80), esz = le32(sec + 84), elba = le32(sec + 72);
	con_hex2("GPT", "ents=", nent, "size=", esz);
	if (esz != 128 || nent > 128 || elba != 2) {
		con_hex2("GPT", "lba=", elba, "unusual", 0);
		return;
	}
	con_str("STEP", "partition table");
	rc = dwmmc_read(&h, elba, 32, gpt);
	if (rc) {
		con_hex2("GPT", "rc=", (uint32_t)rc, "", 0);
		return;
	}
	int used = 0, shown = 0;
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = gpt + i * 128;
		int empty = 1;
		for (int k = 0; k < 16; k++)
			if (e[k])
				empty = 0;
		if (empty)
			continue;
		used++;
		char name[16];
		int n = 0;
		for (; n < 15 && e[56 + n * 2]; n++)
			name[n] = (char)e[56 + n * 2];
		name[n] = 0;
		if (shown < 14 && want(name)) {
			char lab[8];
			for (int k = 0; k < 7; k++)
				lab[k] = name[k];
			lab[7] = 0;
			con_hex2(lab, "lba=", le32(e + 32), "end=", le32(e + 40));
			shown++;
		}
	}
	con_hex2("DONE", "parts=", (uint32_t)used, "shown=", (uint32_t)shown);
}
