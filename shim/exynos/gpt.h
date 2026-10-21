#pragma once
/*
 * Find a GPT partition by name through the read-only DW-MSHC driver. Portable: used by the shim probe,
 * the host simulator test and (embedded by tools/mk_emmc_patch.py) the xnu eMMC root device.
 */
#include <stdint.h>
#include "dwmmc.h"

#define GPT_ENOGPT	(-10)	/* LBA 1 has no "EFI PART" signature */
#define GPT_EBAD	(-11)	/* header values outside what this reader accepts */
#define GPT_ENOTFOUND	(-12)	/* no partition with that name */

/* Name match is ASCII, case-insensitive. On success first/last are the partition's LBA range (inclusive).
 * nparts (optional) receives the number of non-empty entries seen up to the match. */
int gpt_find(struct dwmmc *h, const char *name, uint64_t *first, uint64_t *last, uint32_t *nparts);
