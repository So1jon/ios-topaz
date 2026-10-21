/* GPT partition lookup by name, see gpt.h. 512-byte logical sectors, 128-byte entries. */
#include "gpt.h"

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static char fold(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* entry name is UTF-16LE at +56 (36 code units); compare its ASCII range with `want` */
static int name_is(const uint8_t *e, const char *want)
{
	for (int i = 0; i < 36; i++) {
		char a = (e[56 + i * 2 + 1] == 0) ? (char)e[56 + i * 2] : '?';
		char w = want[i];
		if (fold(a) != fold(w))
			return 0;
		if (!w)
			return 1;
	}
	return want[36] == 0;
}

int gpt_find(struct dwmmc *h, const char *name, uint64_t *first, uint64_t *last, uint32_t *nparts)
{
	uint32_t sec[128];		/* 512 bytes, 4-byte aligned for the FIFO reads */
	uint32_t buf[4 * 128];		/* 16 entries per read */
	const uint8_t *s = (const uint8_t *)sec;
	const uint8_t *b = (const uint8_t *)buf;
	static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
	uint32_t used = 0;
	int rc = dwmmc_read(h, 1, 1, sec);

	if (rc)
		return rc;
	for (int i = 0; i < 8; i++)
		if ((char)s[i] != sig[i])
			return GPT_ENOGPT;
	uint32_t nent = le32(s + 80), esz = le32(s + 84);
	uint64_t elba = le64(s + 72);
	if (esz != 128 || nent == 0 || nent > 1024 || elba < 2 || elba > 0xffff0000ull)
		return GPT_EBAD;
	for (uint32_t i = 0; i < nent; i += 16) {
		rc = dwmmc_read(h, elba + i / 4, 4, buf);
		if (rc)
			return rc;
		for (uint32_t j = 0; j < 16 && i + j < nent; j++) {
			const uint8_t *e = b + j * 128;
			int empty = 1;
			for (int k = 0; k < 16; k++)
				if (e[k])
					empty = 0;
			if (empty)
				continue;
			used++;
			if (name_is(e, name)) {
				*first = le64(e + 32);
				*last = le64(e + 40);
				if (nparts)
					*nparts = used;
				return (*first <= *last) ? 0 : GPT_EBAD;
			}
		}
	}
	if (nparts)
		*nparts = used;
	return GPT_ENOTFOUND;
}
