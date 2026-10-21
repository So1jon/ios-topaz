/*
 * Host test for dwmmc.c: a behavioural model of the DW-MSHC register file (command engine, 64-word
 * FIFO with RXDR/DTO like the databook, card state, timeouts). It checks the driver's control flow,
 * NOT the real hardware. Build and run:  shim/exynos/dwmmc_sim_test.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dwmmc.h"
#include "gpt.h"

#define BASE	0x13540000u
#define SECT	128			/* 64 KiB disk */
#define FIFO_DEPTH	64

static uint32_t regs[0x400 / 4];
static uint8_t disk[SECT * 512];
static uint32_t fifo[FIFO_DEPTH * 2];
static int fifo_n, fifo_h;
static uint32_t pend_words;	/* words of the running transfer not yet in the FIFO */
static uint32_t pend_src;		/* word index into disk */
static int card_state = 4;		/* 3 stby, 4 tran */
static int no_card, corrupt_dcrc, data_off = 0x200, short_xfer;
static int cmds_seen[64];

static void refill(void)
{
	while (pend_words && fifo_n < FIFO_DEPTH) {
		memcpy(&fifo[(fifo_h + fifo_n) % (FIFO_DEPTH * 2)], disk + pend_src * 4, 4);
		pend_src++;
		pend_words--;
		fifo_n++;
	}
	if (!pend_words)
		regs[DW_RINTSTS / 4] |= (1u << 3);	/* DTO once everything is in the FIFO */
}

static void exec(uint32_t cmd)
{
	uint32_t idx = cmd & 0x3f, arg = regs[DW_CMDARG / 4];
	cmds_seen[idx]++;
	regs[DW_RINTSTS / 4] |= 1;	/* CD */
	if (no_card) {
		regs[DW_RINTSTS / 4] |= (1u << 8);	/* RTO */
		return;
	}
	switch (idx) {
	case 13:
		regs[DW_RESP0 / 4] = (uint32_t)card_state << 9 | (1u << 8);
		break;
	case 7:
		card_state = 4;
		regs[DW_RESP0 / 4] = 4u << 9;
		break;
	case 17:
	case 18: {
		uint32_t words = regs[DW_BYTCNT / 4] / 4;
		if (short_xfer)
			words -= 4;
		pend_words = words;
		pend_src = arg * 128;
		if (corrupt_dcrc)
			regs[DW_RINTSTS / 4] |= (1u << 7);
		refill();
		break;
	}
	default:
		regs[DW_RINTSTS / 4] |= (1u << 8);
	}
}

uint32_t sim_rd(uintptr_t a)
{
	uint32_t off = (uint32_t)(a - BASE);
	if (off == (uint32_t)data_off) {
		if (!fifo_n)
			return 0xdeadbeef;
		uint32_t v = fifo[fifo_h];
		fifo_h = (fifo_h + 1) % (FIFO_DEPTH * 2);
		fifo_n--;
		refill();
		return v;
	}
	if (off == DW_STATUS)
		return (regs[off / 4] & ~(0x1fffu << 17)) | ((uint32_t)fifo_n << 17);
	if (off == DW_RINTSTS) {
		uint32_t v = regs[off / 4];
		if (fifo_n >= FIFO_DEPTH / 2 || (!pend_words && fifo_n))
			v |= (1u << 5);	/* RXDR */
		return v;
	}
	return regs[off / 4];
}

void sim_wr(uintptr_t a, uint32_t v)
{
	uint32_t off = (uint32_t)(a - BASE);
	if (off == DW_RINTSTS) {
		regs[off / 4] &= ~v;
	} else if (off == DW_CTRL) {
		if (v & 2) {
			fifo_n = 0;
			fifo_h = 0;
			pend_words = 0;
		}
		regs[off / 4] = v & ~7u;
	} else if (off == DW_CMD) {
		regs[off / 4] = v & ~(1u << 31);
		if (v & (1u << 31))
			exec(v);
	} else {
		regs[off / 4] = v;
	}
}

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); fails++; } else printf("ok:   %s\n", what); } while (0)

static void reset(void)
{
	memset(regs, 0, sizeof regs);
	fifo_n = fifo_h = 0;
	pend_words = 0;
	card_state = 4;
	no_card = corrupt_dcrc = short_xfer = 0;
	regs[DW_VERID / 4] = 0x5342270a;
}

int main(void)
{
	for (uint32_t i = 0; i < sizeof disk; i++)
		disk[i] = (uint8_t)(i * 7 + (i >> 9));
	memcpy(disk + 512, "EFI PART", 8);
	{
		/* GPT header fields at LBA 1: entries LBA 2, 128 entries x 128 bytes; three named partitions */
		uint8_t *hd = disk + 512;
		uint32_t nent = 128, esz = 128;
		uint64_t elba = 2;
		memcpy(hd + 80, &nent, 4);
		memcpy(hd + 84, &esz, 4);
		memcpy(hd + 72, &elba, 8);
		memset(disk + 2 * 512, 0, 32 * 512);
		static const struct { int idx; const char *name; uint64_t f, l; } parts[] = {
			{ 0, "BOOTA0", 34, 63 }, { 5, "RECOVERY", 64, 99 }, { 40, "UserData", 100, 120 },
		};
		for (int i = 0; i < 3; i++) {
			uint8_t *e = disk + 2 * 512 + parts[i].idx * 128;
			memset(e, 0xa5, 16);	/* non-empty type GUID */
			memcpy(e + 32, &parts[i].f, 8);
			memcpy(e + 40, &parts[i].l, 8);
			for (int c = 0; parts[i].name[c]; c++)
				e[56 + c * 2] = (uint8_t)parts[i].name[c];
		}
	}

	struct dwmmc h;
	uint8_t buf[SECT * 512];
	uint32_t st;

	reset();
	dwmmc_init(&h, BASE);
	CHECK(h.data_off == 0x200, "VERID 0x270a selects the 0x200 FIFO port");
	regs[DW_VERID / 4] = 0x5342230a;
	dwmmc_init(&h, BASE);
	CHECK(h.data_off == 0x100, "VERID 0x230a selects the 0x100 FIFO port");

	reset();
	dwmmc_init(&h, BASE);
	CHECK(dwmmc_card_status(&h, &st) == 0 && ((st >> 9) & 0xf) == 4, "CMD13 reports the transfer state");
	card_state = 3;
	dwmmc_card_status(&h, &st);
	CHECK(((st >> 9) & 0xf) == 3, "CMD13 reports the stand-by state");
	CHECK(dwmmc_select(&h) == 0 && card_state == 4, "CMD7 selects the card");

	memset(buf, 0, sizeof buf);
	CHECK(dwmmc_read(&h, 1, 1, buf) == 0, "CMD17 single sector returns 0");
	CHECK(!memcmp(buf, disk + 512, 512), "sector 1 matches the disk (\"EFI PART\")");
	CHECK(cmds_seen[17] == 1, "single sector used CMD17");

	memset(buf, 0, sizeof buf);
	CHECK(dwmmc_read(&h, 0, SECT, buf) == 0, "128 sectors (two 64-sector chunks) return 0");
	CHECK(!memcmp(buf, disk, sizeof disk), "all 128 sectors match the disk");
	CHECK(cmds_seen[18] >= 2, "multi-sector used CMD18");

	memset(buf, 0, sizeof buf);
	CHECK(dwmmc_read(&h, 5, 3, buf) == 0 && !memcmp(buf, disk + 5 * 512, 3 * 512), "3 sectors at LBA 5");

	reset();
	dwmmc_init(&h, BASE);
	no_card = 1;
	CHECK(dwmmc_card_status(&h, &st) == DWMMC_ETIMEOUT, "no card: command timeout is reported");
	CHECK(dwmmc_read(&h, 0, 1, buf) == DWMMC_ETIMEOUT, "no card: read reports a timeout");

	reset();
	dwmmc_init(&h, BASE);
	corrupt_dcrc = 1;
	CHECK(dwmmc_read(&h, 0, 1, buf) == DWMMC_EDATA, "data CRC error is reported");

	reset();
	dwmmc_init(&h, BASE);
	short_xfer = 1;
	CHECK(dwmmc_read(&h, 0, 1, buf) == DWMMC_EDATA, "short transfer is reported");

	reset();
	dwmmc_init(&h, BASE);
	{
		uint64_t f = 0, l = 0;
		uint32_t n = 0;
		CHECK(gpt_find(&h, "RECOVERY", &f, &l, &n) == 0 && f == 64 && l == 99, "GPT: RECOVERY found with its LBA range");
		CHECK(gpt_find(&h, "recovery", &f, &l, NULL) == 0 && f == 64, "GPT: name match is case-insensitive");
		CHECK(gpt_find(&h, "USERDATA", &f, &l, &n) == 0 && f == 100 && l == 120 && n == 3, "GPT: USERDATA (entry 40) found, 3 entries seen");
		CHECK(gpt_find(&h, "RECOVER", &f, &l, NULL) == GPT_ENOTFOUND, "GPT: a prefix is not a match");
		CHECK(gpt_find(&h, "NOSUCH", &f, &l, &n) == GPT_ENOTFOUND && n == 3, "GPT: unknown name reports not found");
		disk[512] = 'X';
		CHECK(gpt_find(&h, "RECOVERY", &f, &l, NULL) == GPT_ENOGPT, "GPT: missing signature is reported");
		disk[512] = 'E';
		uint32_t bad = 64;
		memcpy(disk + 512 + 84, &bad, 4);
		CHECK(gpt_find(&h, "RECOVERY", &f, &l, NULL) == GPT_EBAD, "GPT: bad entry size is rejected");
		bad = 128;
		memcpy(disk + 512 + 84, &bad, 4);
	}
	no_card = 1;
	{
		uint64_t f, l;
		CHECK(gpt_find(&h, "RECOVERY", &f, &l, NULL) == DWMMC_ETIMEOUT, "GPT: no card gives a timeout, not a hang");
	}
	no_card = 0;

	reset();
	dwmmc_init(&h, BASE);
	CHECK(dwmmc_read(&h, 0, 0, buf) == DWMMC_EARG, "zero sectors is rejected");
	CHECK(dwmmc_read(&h, 1ull << 33, 1, buf) == DWMMC_EARG, "LBA beyond 32 bits is rejected");

	printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
	return fails != 0;
}
