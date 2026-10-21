/*
 * Read-only polled DW-MSHC driver, see dwmmc.h. Register layout: Synopsys DWC_mobile_storage databook
 * as implemented by Linux drivers/mmc/host/dw_mmc.c.
 */
#include "dwmmc.h"

#ifdef DWMMC_SIM
extern uint32_t sim_rd(uintptr_t addr);
extern void sim_wr(uintptr_t addr, uint32_t v);
#define RD(h, off)	sim_rd((h)->base + (off))
#define WR(h, off, v)	sim_wr((h)->base + (off), (v))
#else
#define RD(h, off)	(*(volatile uint32_t *)((h)->base + (off)))
#define WR(h, off, v)	(*(volatile uint32_t *)((h)->base + (off)) = (v))
#endif

/* CTRL */
#define CTRL_FIFO_RESET		(1u << 1)
#define CTRL_DMA_RESET		(1u << 2)
#define CTRL_INT_ENABLE		(1u << 4)
#define CTRL_DMA_ENABLE		(1u << 5)
#define CTRL_USE_IDMAC		(1u << 25)
/* CMD */
#define CMD_START		(1u << 31)
#define CMD_USE_HOLD_REG	(1u << 29)
#define CMD_WAIT_PRVDATA	(1u << 13)
#define CMD_SEND_AUTO_STOP	(1u << 12)
#define CMD_DATA_EXPECT		(1u << 9)
#define CMD_CHECK_RESP_CRC	(1u << 8)
#define CMD_RESP_EXPECT		(1u << 6)
/* RINTSTS */
#define INT_CD			(1u << 0)
#define INT_RE			(1u << 1)
#define INT_DTO			(1u << 3)
#define INT_RXDR		(1u << 5)
#define INT_RCRC		(1u << 6)
#define INT_DCRC		(1u << 7)
#define INT_RTO			(1u << 8)
#define INT_DRTO		(1u << 9)
#define INT_HLE			(1u << 12)
#define INT_SBE			(1u << 13)
#define INT_EBE			(1u << 15)
/* STATUS */
#define ST_DATA_BUSY		(1u << 9)

#define SPIN_CMD		2000000u
#define SPIN_DATA		60000000u

uint32_t dwmmc_reg(struct dwmmc *h, uint32_t off)
{
	return RD(h, off);
}

int dwmmc_init(struct dwmmc *h, uintptr_t base)
{
	h->base = base;
	uint32_t ver = RD(h, DW_VERID);
	h->data_off = (ver & 0xffff) >= 0x240a ? 0x200 : 0x100;
	h->rca = 1;
	h->hold = 0;
	h->rintsts = 0;
	return 0;
}

static int wait_idle(struct dwmmc *h)
{
	for (uint32_t i = 0; i < SPIN_CMD; i++)
		if (!(RD(h, DW_STATUS) & ST_DATA_BUSY))
			return 0;
	return DWMMC_ETIMEOUT;
}

/* PIO only: no IDMAC, no DMA interface, no interrupts, FIFO empty. Touches nothing else. */
static void prepare_pio(struct dwmmc *h)
{
	uint32_t ctrl = RD(h, DW_CTRL);
	ctrl &= ~(CTRL_USE_IDMAC | CTRL_DMA_ENABLE | CTRL_INT_ENABLE);
	WR(h, DW_CTRL, ctrl | CTRL_FIFO_RESET | CTRL_DMA_RESET);
	for (uint32_t i = 0; i < SPIN_CMD; i++)
		if (!(RD(h, DW_CTRL) & (CTRL_FIFO_RESET | CTRL_DMA_RESET)))
			break;
	WR(h, DW_INTMASK, 0);
	WR(h, DW_RINTSTS, 0xffffffffu);
}

/* Issue one command and wait for it to be accepted and answered; RINTSTS is left for the caller. */
static int send(struct dwmmc *h, uint32_t idx, uint32_t arg, uint32_t flags)
{
	int e = wait_idle(h);
	if (e)
		return e;
	WR(h, DW_RINTSTS, 0xffffffffu);
	WR(h, DW_CMDARG, arg);
	WR(h, DW_CMD, CMD_START | (h->hold ? CMD_USE_HOLD_REG : 0) | flags | (idx & 0x3f));
	uint32_t i;
	for (i = 0; i < SPIN_CMD; i++)
		if (!(RD(h, DW_CMD) & CMD_START))
			break;
	if (i == SPIN_CMD)
		return DWMMC_ETIMEOUT;
	for (i = 0; i < SPIN_CMD; i++) {
		uint32_t r = RD(h, DW_RINTSTS);
		if (r & INT_HLE)
			return DWMMC_ECMD;
		if (r & INT_CD) {
			h->rintsts = r;
			if (r & INT_RTO)
				return DWMMC_ETIMEOUT;
			if (r & (INT_RE | INT_RCRC))
				return DWMMC_ECMD;
			return 0;
		}
	}
	return DWMMC_ETIMEOUT;
}

int dwmmc_card_status(struct dwmmc *h, uint32_t *status)
{
	prepare_pio(h);
	int e = send(h, 13, h->rca << 16, CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC);
	if (e)
		return e;
	*status = RD(h, DW_RESP0);
	return 0;
}

int dwmmc_select(struct dwmmc *h)
{
	prepare_pio(h);
	return send(h, 7, h->rca << 16, CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC);
}

static int fifo_count(struct dwmmc *h)
{
	return (int)((RD(h, DW_STATUS) >> 17) & 0x1fff);
}

/* One command with a read data phase of nsect sectors (nsect <= 255). */
static int read_chunk(struct dwmmc *h, uint64_t lba, uint32_t nsect, uint32_t *p)
{
	uint32_t words = nsect * 128, got = 0;

	prepare_pio(h);
	WR(h, DW_BLKSIZ, 512);
	WR(h, DW_BYTCNT, nsect * 512);
	int e = send(h, nsect == 1 ? 17 : 18, (uint32_t)lba,
	    CMD_RESP_EXPECT | CMD_CHECK_RESP_CRC | CMD_DATA_EXPECT | CMD_WAIT_PRVDATA |
	    (nsect > 1 ? CMD_SEND_AUTO_STOP : 0));
	if (e)
		return e;
	uint32_t r = h->rintsts;
	for (uint32_t spin = 0; spin < SPIN_DATA; spin++) {
		r |= RD(h, DW_RINTSTS);
		if (r & (INT_DCRC | INT_DRTO | INT_SBE | INT_EBE))
			return DWMMC_EDATA;
		if (r & (INT_RXDR | INT_DTO)) {
			int n = fifo_count(h);
			while (n-- > 0 && got < words)
				p[got++] = RD(h, h->data_off);
			WR(h, DW_RINTSTS, INT_RXDR);
			r &= ~INT_RXDR;
			if (r & INT_DTO) {
				/* transfer over: whatever is still in the FIFO is the tail */
				for (int k = fifo_count(h); k > 0 && got < words; k--)
					p[got++] = RD(h, h->data_off);
				return got == words ? 0 : DWMMC_EDATA;
			}
		}
	}
	return DWMMC_ETIMEOUT;
}

int dwmmc_read(struct dwmmc *h, uint64_t lba, uint32_t nsect, void *buf)
{
	uint32_t *p = buf;

	if (!buf || !nsect || lba > 0xffffffffull)
		return DWMMC_EARG;
	while (nsect) {
		uint32_t n = nsect > 64 ? 64 : nsect;
		int e = read_chunk(h, lba, n, p);
		if (e)
			return e;
		lba += n;
		nsect -= n;
		p += n * 128;
	}
	return 0;
}
