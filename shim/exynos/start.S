/*
 * Exynos 7870 boot shim entry (Galaxy A6 experiment). Samsung S-BOOT loads
 * this file as an arm64 Image kernel inside an Android boot image and enters
 * it with the MMU and caches off, x0 = DTB pointer. The entry exception level
 * is not verified for the A6 (EL1 or EL2): both are handled. XNU is always
 * started at EL1 (its EL2 path touches GICv3 ICC_SRE_EL2, which does not exist
 * on a GICv2 machine).
 *
 * Position independent: it runs wherever S-BOOT puts it. x19 = DTB, x20 = the
 * exception level at entry (CurrentEL value, EL << 2).
 */
	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end			/* image_size (linked at 0) */
	.quad	0xa			/* flags: LE, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 (PE offset) */

	.text
	.globl _start
_start:
	msr	daifset, #0xf
	mov	x19, x0
	mrs	x20, CurrentEL
	cmp	x20, #(2 << 2)
	b.ne	el1_entry

	/* EL2 -> EL1 (AArch64), the same setup as Linux head.S for a GICv2 machine */
	mov	x0, #(1 << 31)		/* HCR_EL2.RW: EL1 is AArch64 */
	msr	hcr_el2, x0
	mrs	x0, cnthctl_el2
	orr	x0, x0, #3		/* EL1 may use the physical counter and timer */
	msr	cnthctl_el2, x0
	msr	cntvoff_el2, xzr
	mrs	x0, midr_el1
	mrs	x1, mpidr_el1
	msr	vpidr_el2, x0
	msr	vmpidr_el2, x1
	mov	x0, #0x33ff		/* CPTR_EL2: no FP/SIMD trapping */
	msr	cptr_el2, x0
	msr	hstr_el2, xzr
	mov	x0, #0x0800		/* SCTLR_EL1 RES1 bits, MMU and caches off */
	movk	x0, #0x30d0, lsl #16
	msr	sctlr_el1, x0
	mov	x0, #0x3c5		/* SPSR_EL2: DAIF masked, EL1h */
	msr	spsr_el2, x0
	adr	x0, el1_entry
	msr	elr_el2, x0
	eret

el1_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adr	x1, _stack_top
	mov	sp, x1
	adr	x1, vectors
	msr	vbar_el1, x1
	isb
	/* zero .bss */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b
2:	mov	x0, x19
	lsr	x1, x20, #2
	bl	shim_main
3:	wfe
	b	3b

/*
 * Exception vectors. Each slot stashes x29/x30, loads its slot number and
 * jumps to the common path, which saves x0-x28 and calls exc_handler(slot,
 * regs). The handler prints on the framebuffer and parks the CPU.
 */
.macro vent slot
	.balign	0x80
	stp	x29, x30, [sp, #-16]!
	mov	x29, #\slot
	b	exc_common
.endm

	.balign	0x800
	.globl vectors
vectors:
	vent 0
	vent 1
	vent 2
	vent 3
	vent 4
	vent 5
	vent 6
	vent 7
	vent 8
	vent 9
	vent 10
	vent 11
	vent 12
	vent 13
	vent 14
	vent 15

exc_common:
	sub	sp, sp, #(30 * 8)
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	str	x28, [sp, #224]
	mov	x0, x29
	mov	x1, sp
	bl	exc_handler
4:	wfe
	b	4b
