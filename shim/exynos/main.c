/*
 * Exynos 7870 shim: print a few facts on the bootloader's framebuffer, then
 * hand over to the XNU payload appended to the Image (loader.c). The MMU stays
 * off. No GIC, PSCI or UFS code here: XNU brings the other CPUs up itself.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

/* clang may turn loops into libc calls; the freestanding image has none, so provide them */
void *memcpy(void *d, const void *s, unsigned long n)
{
	volatile uint8_t *dd = d;
	const volatile uint8_t *ss = s;
	while (n--)
		*dd++ = *ss++;
	return d;
}

void *memset(void *d, int c, unsigned long n)
{
	volatile uint8_t *dd = d;
	while (n--)
		*dd++ = (uint8_t)c;
	return d;
}

/* The stock Samsung bootloader leaves CNTFRQ_EL0 unset (see Linux exynos7870.dtsi): assume 26 MHz. */
static void delay_ms(uint64_t ms)
{
	uint64_t f = rsr("cntfrq_el0");
	if (!f)
		f = 26000000;
	uint64_t end = rsr("cntpct_el0") + f / 1000 * ms;
	while (rsr("cntpct_el0") < end)
		;
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(RED);
	con_str("EXC", "shim exception");
	con_hex("SLOT", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SP", (uint64_t)regs);
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	con_clear(BLUE);
	con_str("SHIM", "exynos7870 xnu loader");
	con_hex2("ENTRY", "el=", (uint32_t)entry_el, "dtb=", (uint32_t)dtb);
	con_hex2("MIDR", "el1=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2("TIMER", "frq=", (uint32_t)rsr("cntfrq_el0"), "sctlr=", (uint32_t)rsr("sctlr_el1"));
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_str("HALT", "see message above");
	delay_ms(600000);
	for (;;)
		__asm__ volatile("wfe");
}
