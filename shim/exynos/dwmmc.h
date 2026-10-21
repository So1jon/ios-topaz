#pragma once
/*
 * Read-only polled driver for the Synopsys DesignWare Mobile Storage host (DW-MSHC) that Exynos 7870
 * uses for eMMC (mmc0 @ 0x13540000 in mainline Linux). It TAKES OVER the state the bootloader left
 * (clocks, bus width, timing, card selected): it never touches clocks, pinmux, power or the card
 * configuration, and it never writes to the card. PIO through the FIFO, no DMA/IDMAC, so the
 * Exynos "SMU" address translation does not matter.
 *
 * Untested on hardware: first use is shim/exynos/probe_main.c (prints, writes nothing to storage).
 */
#include <stdint.h>

#define DWMMC_ETIMEOUT	(-1)	/* no response / controller stuck */
#define DWMMC_ECMD	(-2)	/* response error or CRC error */
#define DWMMC_EDATA	(-3)	/* data timeout, CRC, start/end bit or short transfer */
#define DWMMC_EARG	(-4)

struct dwmmc {
	uintptr_t base;
	uint32_t data_off;	/* FIFO port offset: 0x100 before VERID 0x240a, 0x200 from then on */
	uint32_t rca;		/* relative card address; the bootloader normally leaves 1 */
	uint32_t hold;		/* extra CMD flag: USE_HOLD_REG (needed in high-speed timings) */
	uint32_t rintsts;	/* RINTSTS seen at the end of the last command / transfer */
};

/* Reads VERID, picks the FIFO port offset. No writes. */
int dwmmc_init(struct dwmmc *h, uintptr_t base);
/* CMD13: raw card status (R1). State is bits [12:9]: 3 = stby, 4 = tran. */
int dwmmc_card_status(struct dwmmc *h, uint32_t *status);
/* CMD7 with the RCA: select the card (stby -> tran). */
int dwmmc_select(struct dwmmc *h);
/* Read nsect 512-byte sectors starting at lba (high-capacity addressing). */
int dwmmc_read(struct dwmmc *h, uint64_t lba, uint32_t nsect, void *buf);

/* register offsets, for diagnostics */
#define DW_CTRL		0x00
#define DW_PWREN	0x04
#define DW_CLKDIV	0x08
#define DW_CLKSRC	0x0c
#define DW_CLKENA	0x10
#define DW_TMOUT	0x14
#define DW_CTYPE	0x18
#define DW_BLKSIZ	0x1c
#define DW_BYTCNT	0x20
#define DW_INTMASK	0x24
#define DW_CMDARG	0x28
#define DW_CMD		0x2c
#define DW_RESP0	0x30
#define DW_RINTSTS	0x44
#define DW_STATUS	0x48
#define DW_FIFOTH	0x4c
#define DW_CDETECT	0x50
#define DW_VERID	0x6c
#define DW_HCON		0x70
#define DW_UHS		0x74
#define DW_BMOD		0x80

uint32_t dwmmc_reg(struct dwmmc *h, uint32_t off);
