/*
 * RAM copy-on-write overlay for a read-only block device (used by xnu_sweet_emmc.c).
 *
 * iOS wants a writable root (launchd runs `mount -uw /`, HFS replays its journal), but the eMMC driver must
 * never write the phone's flash. Writes land in 4 KiB chunks kept in RAM; reads take the chunk from RAM when
 * it was written and from the card otherwise. Nothing survives a reboot, by design.
 *
 * Portable (no kernel headers): the allocator and the "read the original chunk" function are supplied by
 * the caller, so the same code is unit-tested on the host (cowovl_test.c).
 */
#ifndef COWOVL_H
#define COWOVL_H
#include <stdint.h>
#include <stddef.h>

#define COWOVL_CHUNK 4096u

struct cowovl_ent {
	uint64_t chunk;			/* chunk index + 1; 0 = free slot */
	uint8_t *page;
};

struct cowovl {
	struct cowovl_ent *tab;
	uint32_t cap;			/* power of two, >= 2 * max */
	uint32_t used, max;		/* chunks in use / allowed */
	void *(*alloc)(size_t);		/* zeroed memory or NULL */
	void (*release)(void *, size_t);
};

#define COWOVL_ENOMEM (-1)
#define COWOVL_EFULL  (-2)
#define COWOVL_EREAD  (-3)

int cowovl_init(struct cowovl *o, uint32_t max_chunks, void *(*alloc)(size_t), void (*release)(void *, size_t));

/* overwrite the parts of buf (= device bytes [off, off+len)) that were written before */
void cowovl_patch(const struct cowovl *o, uint64_t off, uint32_t len, uint8_t *buf);

/* base_read(ctx, chunk, page): fetch the original 4 KiB chunk (zero-fill past the end of the device); 0 = ok */
typedef int (*cowovl_base_read)(void *ctx, uint64_t chunk, uint8_t *page);
int cowovl_write(struct cowovl *o, uint64_t off, uint32_t len, const uint8_t *src, cowovl_base_read base_read,
    void *ctx);
#endif
