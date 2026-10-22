@ Shared by the BIOS's source files.

@ Cycles spent to take as long as the reference BIOS: a nop is one cycle here.
.macro wait cycles
	.rept \cycles
	nop
	.endr
.endm

@ The same time as wait, from a loop on reg, which it leaves 0, and the flags: 4k - 1 cycles for k
@ passes, then nops for the rest.
.macro delay reg, cycles
	.if \cycles < 7
	wait   \cycles
	.else
	mov    \reg, #((\cycles + 1) / 4)
.Ldelay\@:
	subs   \reg, \reg, #1
	bne    .Ldelay\@
	wait   (\cycles - 4 * ((\cycles + 1) / 4) + 1)
	.endif
.endm

@ A call that takes as long as the reference's Thumb bl, which comes in two halves.
.macro thumbBl target
	nop
	bl     \target
.endm
