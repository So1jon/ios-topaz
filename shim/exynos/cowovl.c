/* RAM copy-on-write overlay, see cowovl.h. Open addressing, linear probing, never deletes. */
#include "cowovl.h"
#include <string.h>

static uint32_t slot_of(const struct cowovl *o, uint64_t chunk)
{
	uint64_t h = chunk * 0x9E3779B97F4A7C15ull;
	return (uint32_t)(h >> 32) & (o->cap - 1);
}

static struct cowovl_ent *find(const struct cowovl *o, uint64_t chunk)
{
	for (uint32_t i = slot_of(o, chunk), n = 0; n < o->cap; i = (i + 1) & (o->cap - 1), n++) {
		if (o->tab[i].chunk == chunk + 1)
			return &o->tab[i];
		if (o->tab[i].chunk == 0)
			return NULL;
	}
	return NULL;
}

int cowovl_init(struct cowovl *o, uint32_t max_chunks, void *(*alloc)(size_t), void (*release)(void *, size_t))
{
	uint32_t cap = 16;
	while (cap < 2 * max_chunks)
		cap <<= 1;
	memset(o, 0, sizeof(*o));
	o->alloc = alloc;
	o->release = release;
	o->tab = alloc((size_t)cap * sizeof(struct cowovl_ent));
	if (!o->tab)
		return COWOVL_ENOMEM;
	o->cap = cap;
	o->max = max_chunks;
	return 0;
}

void cowovl_patch(const struct cowovl *o, uint64_t off, uint32_t len, uint8_t *buf)
{
	if (!o->tab || !o->used)
		return;
	for (uint64_t c = off / COWOVL_CHUNK; c * COWOVL_CHUNK < off + len; c++) {
		const struct cowovl_ent *e = find(o, c);
		if (!e)
			continue;
		uint64_t lo = c * COWOVL_CHUNK > off ? c * COWOVL_CHUNK : off;
		uint64_t hi = (c + 1) * COWOVL_CHUNK < off + len ? (c + 1) * COWOVL_CHUNK : off + len;
		memcpy(buf + (lo - off), e->page + (lo - c * COWOVL_CHUNK), (size_t)(hi - lo));
	}
}

int cowovl_write(struct cowovl *o, uint64_t off, uint32_t len, const uint8_t *src, cowovl_base_read base_read,
    void *ctx)
{
	for (uint64_t c = off / COWOVL_CHUNK; c * COWOVL_CHUNK < off + len; c++) {
		uint64_t lo = c * COWOVL_CHUNK > off ? c * COWOVL_CHUNK : off;
		uint64_t hi = (c + 1) * COWOVL_CHUNK < off + len ? (c + 1) * COWOVL_CHUNK : off + len;
		struct cowovl_ent *e = find(o, c);
		if (!e) {
			if (o->used >= o->max)
				return COWOVL_EFULL;
			uint8_t *page = o->alloc(COWOVL_CHUNK);
			if (!page)
				return COWOVL_ENOMEM;
			if (hi - lo != COWOVL_CHUNK && base_read(ctx, c, page)) {
				o->release(page, COWOVL_CHUNK);
				return COWOVL_EREAD;
			}
			uint32_t i = slot_of(o, c);
			while (o->tab[i].chunk)
				i = (i + 1) & (o->cap - 1);
			o->tab[i].chunk = c + 1;
			o->tab[i].page = page;
			o->used++;
			e = &o->tab[i];
		}
		memcpy(e->page + (lo - c * COWOVL_CHUNK), src + (lo - off), (size_t)(hi - lo));
	}
	return 0;
}
