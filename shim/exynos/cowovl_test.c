/* Host test for cowovl.c: compare the overlay against a flat reference model under random I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cowovl.h"

#define DEV (64 * 4096u + 1000)		/* not a multiple of the chunk size on purpose */
static uint8_t base[DEV], ref[DEV];
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void *al(size_t n) { return calloc(1, n); }
static void rl(void *p, size_t n) { (void)n; free(p); }
static int bread(void *ctx, uint64_t c, uint8_t *page)
{
	(void)ctx;
	uint64_t o = c * COWOVL_CHUNK;
	memset(page, 0, COWOVL_CHUNK);
	if (o < DEV)
		memcpy(page, base + o, DEV - o < COWOVL_CHUNK ? DEV - o : COWOVL_CHUNK);
	return 0;
}

int main(void)
{
	struct cowovl o;
	static uint8_t buf[3 * 4096], got[3 * 4096];
	srand(7);
	for (uint32_t i = 0; i < DEV; i++)
		ref[i] = base[i] = (uint8_t)rand();
	CHECK(cowovl_init(&o, 40, al, rl) == 0, "init");
	for (int it = 0; it < 2000; it++) {
		uint32_t len = 1 + rand() % sizeof(buf);
		uint64_t off = (uint64_t)rand() % (DEV - len);
		if (rand() & 1) {
			for (uint32_t i = 0; i < len; i++)
				buf[i] = (uint8_t)rand();
			int rc = cowovl_write(&o, off, len, buf, bread, NULL);
			if (rc == COWOVL_EFULL)
				break;		/* limit reached: later writes would be refused, reference must stay unchanged */
			CHECK(rc == 0, "write");
			memcpy(ref + off, buf, len);
		} else {
			memcpy(got, base + off, len);
			cowovl_patch(&o, off, len, got);
			if (memcmp(got, ref + off, len)) {
				CHECK(0, "read mismatch");
				break;
			}
		}
	}
	CHECK(o.used <= 40, "chunk limit respected");
	memcpy(got, base, 4096);
	cowovl_patch(&o, 0, 4096, got);
	CHECK(!memcmp(got, ref, 4096), "final chunk 0");
	/* the limit: fresh overlay, writes to 3 distinct chunks with max 2 */
	struct cowovl s;
	CHECK(cowovl_init(&s, 2, al, rl) == 0, "init small");
	CHECK(cowovl_write(&s, 0, 4096, buf, bread, NULL) == 0, "w0");
	CHECK(cowovl_write(&s, 4096, 10, buf, bread, NULL) == 0, "w1");
	CHECK(cowovl_write(&s, 8192, 10, buf, bread, NULL) == COWOVL_EFULL, "w2 refused");
	CHECK(cowovl_write(&s, 100, 10, buf, bread, NULL) == 0, "rewrite of an existing chunk still fine when full");
	printf(fails ? "%d FAILED\n" : "cowovl: all checks passed (%u chunks used)\n", fails ? fails : o.used);
	return fails != 0;
}
