#pragma once
#include <stdint.h>

void delay_ms(uint64_t ms);
/* Read-only eMMC probe (probe.c): prints on the framebuffer console, writes nothing to storage. */
void probe_run(uint64_t dtb);
/* fdtdump.c: memory banks and reserved-memory of the DTB the bootloader passed in x0 */
void fdt_dump_memory(uintptr_t fdt);
