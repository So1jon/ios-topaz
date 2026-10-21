#pragma once
#include <stdint.h>

void delay_ms(uint64_t ms);
/* Read-only eMMC probe (probe.c): prints on the framebuffer console, writes nothing to storage. */
void probe_run(void);
