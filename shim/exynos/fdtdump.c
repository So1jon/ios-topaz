/*
 * Print the memory map of the DTB the bootloader hands over in x0: memory@* banks, reserved-memory
 * children and the initrd range. Samsung's S-BOOT patches these at boot, so this is the real map of
 * the device (unlike the unit addresses visible over adb). Read-only; no libc.
 */
#include <stdint.h>
#include "console.h"
#include "probe.h"

#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9
#define MAXDEPTH	10

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static int starts(const char *s, const char *pre)
{
	while (*pre)
		if (*s++ != *pre++)
			return 0;
	return 1;
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void label7(char *dst, const char *name, int plus)
{
	int i = 0;
	if (plus)
		dst[i++] = '+';
	/* skip a leading "memory@" so the interesting part (the address) fits */
	if (starts(name, "memory@"))
		name += 7;
	while (i < 7 && *name && *name != '@')
		dst[i++] = *name++;
	dst[i] = 0;
}

void fdt_dump_memory(uintptr_t fdt)
{
	const uint8_t *h = (const uint8_t *)fdt;
	if (be32(h) != 0xd00dfeedu) {
		con_hex2("FDT", "bad=", be32(h), 0, 0);
		return;
	}
	uint32_t total = be32(h + 4), off_struct = be32(h + 8), off_str = be32(h + 12);
	con_hex2("FDT", "size=", total, "at=", (uint32_t)fdt);

	const char *names[MAXDEPTH] = { 0 };
	int depth = 0, lines = 0;
	const uint8_t *p = h + off_struct;
	const char *strs = (const char *)h + off_str;
	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *nm = (const char *)p;
			int n = 0;
			while (nm[n])
				n++;
			p += (n + 1 + 3) & ~3;
			if (depth < MAXDEPTH)
				names[depth] = nm;
			depth++;
		} else if (tok == FDT_END_NODE) {
			depth--;
		} else if (tok == FDT_PROP) {
			uint32_t len = be32(p), noff = be32(p + 4);
			const uint8_t *d = p + 8;
			const char *pn = strs + noff;
			p += 8 + ((len + 3) & ~3);
			const char *node = (depth >= 1 && depth <= MAXDEPTH) ? names[depth - 1] : "";
			const char *par = (depth >= 2 && depth - 2 < MAXDEPTH) ? names[depth - 2] : "";
			if (lines < 22 && same(pn, "reg") && len >= 12 &&
			    (starts(node, "memory@") || same(par, "reserved-memory"))) {
				char lab[9];
				label7(lab, node, be32(d) != 0);
				con_hex2(lab, "a=", be32(d + 4), "s=", be32(d + len - 4));
				lines++;
			} else if (lines < 22 && same(node, "chosen") && same(pn, "linux,initrd-start")) {
				con_hex2("INITRD", "lo=", be32(d + len - 4), 0, 0);
				lines++;
			}
		} else if (tok == FDT_END || (tok != FDT_NOP && depth < 0)) {
			break;
		}
		if (depth < 0)
			break;
	}
	con_hex2("FDT", "shown=", (uint32_t)lines, 0, 0);
}
