@ The test ROM's header and entry. It starts with the BIOS intro skipped, in System mode with the
@ stack set up, so it only has to reach main.
.syntax unified
.arm
.section .header, "ax"
.global _start
_start:
	b      entry
	.fill  156, 1, 0            @ No logo: the test runs with the intro skipped.
	.ascii "BIOSCHECK   "       @ Title.
	.ascii "BCHK"               @ Game code.
	.ascii "01"                 @ Maker.
	.byte  0x96, 0x00, 0x00
	.fill  7, 1, 0
	.byte  0x00, 0x00           @ Version, header checksum (not checked with the intro skipped).
	.byte  0x00, 0x00
entry:
	ldr    r0, =main
	bx     r0
.ltorg

@ The game's interrupt handler, as games install it at 0x03007FFC: it acknowledges the interrupts
@ taken, adds them to the flags IntrWait reads at 0x03007FF8, and logs each with the cycle counter
@ (timers 2 and 3), the scanline and the System mode stack pointer.
.text
.global irqHandler
irqHandler:
	mov    r0, #0x04000000
	add    r2, r0, #0x200
	ldr    r1, [r2]
	and    r1, r1, r1, lsr #16
	strh   r1, [r2, #2]
	tst    r1, #0x08                @ Timer 0 is a one-shot.
	movne  r12, #0
	strhne r12, [r2, #-0xFE]
	ldr    r2, =0x03007FF8
	ldrh   r3, [r2]
	orr    r3, r3, r1
	strh   r3, [r2]
	ldr    r2, =0x0201FFEC          @ The log's end.
	ldr    r3, [r2]
	ldr    r12, =0x0201F000         @ The log is full there.
	cmp    r3, #0
	cmpne  r3, r12
	bhs    1f
	str    r1, [r3], #4
0:
	add    r2, r0, #0x100
	ldrh   r12, [r2, #0x0C]
	ldrh   r1, [r2, #0x08]
	ldrh   r2, [r2, #0x0C]
	cmp    r12, r2
	bne    0b
	orr    r1, r1, r12, lsl #16
	str    r1, [r3], #4
	ldrh   r1, [r0, #0x006]
	@ With the low 16 bits of the System mode stack pointer, to show how deep the BIOS's stack is.
	mrs    r2, cpsr
	orr    r12, r2, #0x1F
	msr    cpsr_c, r12
	mov    r12, sp
	msr    cpsr_c, r2
	orr    r1, r1, r12, lsl #16
	str    r1, [r3], #4
	ldr    r2, =0x0201FFEC
	str    r3, [r2]
1:
	bx     lr
.ltorg

@ RegisterRamReset with r0 = its flags and the stack at r1; r0-r3 after it go to the array at r2.
.global resetWithStack
.type resetWithStack, %function
resetWithStack:
	stmfd  sp!, {r4, r5, lr}
	mov    r4, sp
	mov    r5, r2
	mov    sp, r1
	swi    0x010000
	mov    sp, r4
	stmia  r5, {r0-r3}
	ldmfd  sp!, {r4, r5, lr}
	bx     lr

@ SWI r0 from ARM code, with r0-r11 pointing into free EWRAM and lr known; r0-r12, lr, sp and the
@ CPSR after it go to the array at r1.
.global swiProbe
.type swiProbe, %function
swiProbe:
	stmfd  sp!, {r4-r11, lr}
	ldr    r2, =0x0201FFE0
	str    r1, [r2]
	str    sp, [r2, #4]
	adr    r12, swiProbeTable
	add    r12, r12, r0, lsl #3
	ldr    lr, =0x02030E00
	adr    r0, swiProbeInputs
	ldmia  r0, {r0-r11}
	bx     r12
@ SWI r1 with r0-r11 from the block at r0, which r0-r12, lr, sp and the CPSR after it replace.
.global swiRegs
.type swiRegs, %function
swiRegs:
	stmfd  sp!, {r4-r11, lr}
	ldr    r2, =0x0201FFE0
	str    r0, [r2]
	str    sp, [r2, #4]
	adr    r12, swiProbeTable
	add    r12, r12, r1, lsl #3
	ldr    lr, =0x02030E00
	ldmia  r0, {r0-r11}
	bx     r12
swiProbeAfter:
	stmfd  sp!, {r0-r12, lr}
	add    r0, sp, #56
	mrs    r1, cpsr
	ldr    r2, =0x0201FFE0
	ldr    r3, [r2]
	ldr    r4, [r2, #4]
	mov    r5, #14
0:
	ldmfd  sp!, {r6}
	str    r6, [r3], #4
	subs   r5, r5, #1
	bne    0b
	stmia  r3, {r0, r1}
	mov    sp, r4
	ldmfd  sp!, {r4-r11, lr}
	bx     lr
swiProbeInputs:
	.word  0x02030000, 0x02030100, 0x02030200, 0x02030300, 0x02030400, 0x02030500
	.word  0x02030600, 0x02030700, 0x02030800, 0x02030900, 0x02030A00, 0x02030B00
.ltorg
swiProbeTable:
	.set   number, 0
	.rept  256
	swi    number << 16
	b      swiProbeAfter
	.set   number, number + 1
	.endr

@ A callback for the BIOS to call: r0-r3, lr, the stack pointer, the CPSR, the sound work area's
@ first word and the time go to the log at 0x0201B000, 0x24 bytes each, counted at 0x0201AFFC.
.global probeCallback
.type probeCallback, %function
probeCallback:
	stmfd  sp!, {r4-r7}
	ldr    r4, =0x0201AFFC
	ldr    r5, [r4]
	add    r6, r5, #1
	str    r6, [r4]
	mov    r6, #0x24
	mul    r7, r5, r6
	add    r7, r7, r4
	add    r7, r7, #4
	stmia  r7!, {r0-r3, lr}
	add    r6, sp, #16
	mrs    r5, cpsr
	stmia  r7!, {r5, r6}
	ldr    r6, =0x03007FF0
	ldr    r6, [r6]
	ldr    r6, [r6]
	ldr    r4, =0x04000100
	ldrh   r5, [r4, #0x08]
	ldrh   r4, [r4, #0x0C]
	orr    r5, r5, r4, lsl #16
	stmia  r7!, {r5, r6}
	ldmfd  sp!, {r4-r7}
	bx     lr
.ltorg

@ Calls the function at block[4] (Thumb if odd) with r0-r3 from block[0..3], and puts r0-r3 back.
.global callWith
.type callWith, %function
callWith:
	stmfd  sp!, {r4, lr}
	mov    r4, r0
	ldr    r12, [r4, #16]
	ldmia  r4, {r0-r3}
	mov    lr, pc
	bx     r12
	stmia  r4, {r0-r3}
	ldmfd  sp!, {r4, lr}
	bx     lr

@ An interrupt handler that acknowledges what's pending and logs it with VCOUNT, the sound work
@ area's first 16 bytes, and timer 0 with DMA 1's control above it, 0x18 bytes each, to the log at
@ 0x0201B000 counted at 0x0201AFFC.
.global irqProbe
.type irqProbe, %function
irqProbe:
	stmfd  sp!, {r4-r7}
	mov    r0, #0x04000000
	add    r2, r0, #0x200
	ldr    r1, [r2]
	and    r1, r1, r1, lsr #16
	strh   r1, [r2, #2]
	ldr    r4, =0x0201AFFC
	ldr    r5, [r4]
	add    r6, r5, #1
	str    r6, [r4]
	mov    r6, #0x18
	mul    r7, r5, r6
	add    r7, r7, r4
	add    r7, r7, #4
	ldrh   r2, [r0, #6]
	orr    r1, r1, r2, lsl #16
	ldr    r6, =0x03007FF0
	ldr    r6, [r6]
	ldmia  r6, {r2, r3, r5, r6}
	stmia  r7!, {r1-r3, r5, r6}
	ldr    r2, =0x04000100
	ldrh   r2, [r2]
	ldr    r3, =0x040000C4
	ldr    r3, [r3]
	orr    r2, r2, r3
	str    r2, [r7]
	ldmfd  sp!, {r4-r7}
	bx     lr
.ltorg
