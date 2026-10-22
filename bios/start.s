@ Copyright (c) 2013-2014 Jeffrey Pfau
@ Copyright (c) 2026 Magnominus
@
@ This Source Code Form is subject to the terms of the Mozilla Public
@ License, v. 2.0. If a copy of the MPL was not distributed with this
@ file, You can obtain one at http://mozilla.org/MPL/2.0/.
@
@ The BIOS, all in ARM code: the exception vectors, the SWI and interrupt entries, reset, and the
@ calls. The entries, waits and copies are adapted from mGBA's src/gba/hle-bios.s; the rest follows
@ GBATEK and what tests/bioscheck measures of a reference BIOS's results and timing. A word after
@ each way out of the BIOS is the opcode a game reads back from BIOS memory after it, as the CPU has
@ prefetched it by then.

.syntax unified
.arm

.include "macros.inc"

@ The calls 0x00 to 0x2A; larger numbers never return, with interrupts still taken.
.equ SWI_COUNT, 0x2B
@ Iterations of the wait before the game starts, from the start of line 126.
.equ BOOT_LINE_WAIT, 204
.section .vectors, "ax"

.global vectors
vectors:
	b resetTrampoline
	b undefBase
	b swiBase
	b pabtBase
	b dabtBase
	nop
	b irqBase
	b fiqBase

@ Z set if the source at r0, r12 bytes long (up to 32 MiB), is refused: none of it, or a start or
@ end with bits 25-27 clear, as the BIOS's addresses have. r12 is left the end. It returns through
@ link.
.macro sourceCheck link
	cmp    r12, #0
	beq    1f
	bic    r12, r12, #0xFE000000
	add    r12, r12, r0
	tst    r12, #0x0E000000
	tstne  r0, #0x0E000000
1:
	bx     \link
.endm

checkSource:
	sourceCheck lr

@ HALTCNT is written with 0 from r2, here and in IntrWait, as an interrupt sees on the reference;
@ swiReturn puts the caller's r2 back.
Halt:
	mov    r2, #0
	@ As long as the reference takes.
	b      0f
0:
	mov    r12, #0x04000000
	strb   r2, [r12, #0x301]
	bx     lr

NopCall:
	bx     lr

Hang:
	b      Hang

divVeneer:
	adr    r3, Div
	bx     r3

cpuFastSetVeneer:
	adr    r3, CpuFastSet
	bx     r3

@ Reset goes through here, so that the first vector reads as the reference's does to a call that
@ reads the BIOS's first word (a sound call with no area set).
.org 0x68
resetTrampoline:
	b      resetBase

@ As CpuSet, in words and in blocks of eight, which go through r2-r9; lr holds the end.
CpuFastSet:
	stmfd  sp!, {r4-r10, lr}
	mov    r10, r2, lsl #11
	mov    r12, r10, lsr #9
	bl     checkSource
	beq    2f
	add    lr, r1, r10, lsr #9
	@ The carry is bit 24, fill.
	movs   r2, r2, lsr #25
	bcc    0f
	ldr    r2, [r0]
	mov    r9, r2
	mov    r8, r2
	mov    r7, r2
	mov    r6, r2
	mov    r5, r2
	mov    r4, r2
	mov    r3, r2
1:
	cmp    r1, lr
	stmialt r1!, {r2-r9}
	blt    1b
	b      2f
0:
	cmp    r1, lr
	ldmialt r0!, {r2-r9}
	stmialt r1!, {r2-r9}
	blt    0b
2:
	ldmfd  sp!, {r4-r10, lr}
	bx     lr

@ The game's interrupt handler is at 0x03007FFC, which 0x03FFFFFC mirrors. This, the SWI entry and
@ the way back are where the reference has them, so the addresses left in lr and on the stacks are
@ the reference's, and so is what a game reads from the BIOS after them.
.org 0x128
irqBase:
	stmfd  sp!, {r0-r3, r12, lr}
	mov    r0, #0x04000000
	add    lr, pc, #0
	ldr    pc, [r0, #-4]
	ldmfd  sp!, {r0-r3, r12, lr}
	subs   pc, lr, #4

.org 0x140
@ A SWI runs its call in System mode, with interrupts as the caller had them. r2 and lr are kept.
@ Numbers past 0x61 read on past the table, as on the reference.
swiBase:
	stmfd  sp!, {r11, r12, lr}
	ldrb   r12, [lr, #-2]
	adr    r11, swiTable
	ldr    r12, [r11, r12, lsl #2]
	mrs    r11, spsr
	stmfd  sp!, {r11}
	and    r11, #0x80
	orr    r11, #0x1F
	msr    cpsr_c, r11
	stmfd  sp!, {r2, lr}
	adr    lr, swiReturn
	bx     r12

.org 0x170
.global swiReturn
swiReturn:
	ldmfd  sp!, {r2, lr}
	mov    r12, #0xD3
	msr    cpsr_c, r12
	ldmfd  sp!, {r11}
	msr    spsr_fsxc, r11
	ldmfd  sp!, {r11, r12, lr}
	movs   pc, lr
	.word 0
	.word 0xE3A02004

swiTable:
	.word SoftReset                   @ 0x00
	.word RegisterRamReset            @ 0x01
	.word Halt                        @ 0x02
	.word Stop                        @ 0x03
	.word IntrWait                    @ 0x04
	.word VBlankIntrWait              @ 0x05
	.word Div                         @ 0x06
	.word DivArm                      @ 0x07
	.word Sqrt                        @ 0x08
	.word ArcTan                      @ 0x09
	.word ArcTan2                     @ 0x0A
	.word CpuSet                      @ 0x0B
	.word CpuFastSet                  @ 0x0C
	.word GetBiosChecksum             @ 0x0D
	.word BgAffineSet                 @ 0x0E
	.word ObjAffineSet                @ 0x0F
	.word BitUnPack                   @ 0x10
	.word Lz77UnCompWram              @ 0x11
	.word Lz77UnCompVram              @ 0x12
	.word HuffUnComp                  @ 0x13
	.word RlUnCompWram                @ 0x14
	.word RlUnCompVram                @ 0x15
	.word Diff8BitUnFilterWram        @ 0x16
	.word Diff8BitUnFilterVram        @ 0x17
	.word Diff16BitUnFilter           @ 0x18
	.word SoundBias                   @ 0x19
	.word SoundDriverInit             @ 0x1A
	.word SoundDriverMode             @ 0x1B
	.word SoundDriverMain             @ 0x1C
	.word SoundDriverVSync            @ 0x1D
	.word SoundChannelClear           @ 0x1E
	.word MidiKey2Freq                @ 0x1F
	.word NopCall                     @ 0x20 MusicPlayerOpen
	.word NopCall                     @ 0x21 MusicPlayerStart
	.word NopCall                     @ 0x22 MusicPlayerStop
	.word NopCall                     @ 0x23 MusicPlayerContinue
	.word NopCall                     @ 0x24 MusicPlayerFadeOut
	.word MultiBoot                   @ 0x25
	.word HardReset                   @ 0x26
	.word CustomHalt                  @ 0x27
	.word SoundDriverVSyncOff         @ 0x28
	.word SoundDriverVSyncOn          @ 0x29
	.word SoundDriverGetJumpList      @ 0x2A
	.rept  0x61 - SWI_COUNT
	.word Hang                        @ 0x2B on
	.endr
	@ On the reference, 0x61 goes to the end of a 16 MiB cartridge, as debugging cartridges expect.
	.word 0x0AFFFFFC                  @ 0x61

@ DivArm: r1 / r0, as Div.
.global DivArm
DivArm:
	mov    r12, r0
	mov    r0, r1
	mov    r1, r12
@ Div: r0 / r1, rounded toward zero. r0 is left the quotient, r1 the remainder, with r0's sign, and r3
@ the quotient's magnitude. The divisor is doubled while it is below half the dividend, then halved
@ back, a quotient bit each time. A divisor of 0 never returns unless the dividend is -1, 0 or 1. The
@ signs go into r2: bit 31 the quotient's, the rest the dividend's; the divisor's in r3 is shifted
@ out by the first quotient bit.
Div:
	ands   r3, r1, #0x80000000
	rsbmi  r1, r1, #0
	eors   r2, r3, r0, asr #32
	rsbcs  r0, r0, #0
	mov    r12, r1
1:
	cmp    r12, r0, lsr #1
	movls  r12, r12, lsl #1
	blo    1b
2:
	cmp    r0, r12
	adc    r3, r3, r3
	subcs  r0, r0, r12
	teq    r1, r12
	movne  r12, r12, lsr #1
	bne    2b
	mov    r1, r0
	mov    r0, r3
	movs   r2, r2, lsl #1
	rsbcs  r0, r0, #0
	rsbmi  r1, r1, #0
	bx     lr

.text

@ r0: what to clear: bit 7 the other registers, 5 the serial registers, 6 the sound registers, 0
@ EWRAM, 3 VRAM, 4 OAM, 2 palette, 1 IWRAM up to its last 512 bytes, in that order, as the
@ reference clears them: each with CpuFastSet filling from a zero kept on the stack, through
@ ramRegion. The screen is blanked first. Clearing the registers starts with IE to IME, so that
@ interrupts stop there. As on the reference, the serial part writes RCNT and JOYCNT after where
@ its clear left r1, which without bit 5 is 0x04000114 and SIODATA's low byte. r0 is left the zero's
@ address (0x100 if only the registers were cleared), r1 IWRAM's start or end, and r3 the way back.
RegisterRamReset:
	stmfd  sp!, {r4-r7, lr}
	sub    sp, sp, #4
	mov    r7, r0
	@ Loaded, not moved, as the reference loads them.
	ldr    r5, fillWords
	mov    r4, #4
	mov    r4, r4, lsl #24
	mov    r3, #0
	str    r3, [sp]
	mov    r1, #0x80
	strh   r1, [r4]
	mov    r6, #0x80
	tst    r7, r6
	beq    1f
	mov    r1, #0x200
	add    r1, r1, r4
	mov    r2, #8
	thumbBl ramRegion
	@ The interrupts flagged cleared (r1 is past IME), then the video registers and DMA, the affine
	@ matrices set to the identity, and the timers and KEYCNT.
	mvn    r0, #0
	strh   r0, [r1, #-0x1E]
	mov    r0, #0
	mov    r1, #0
	mov    r2, #0
	@ 0x04 to 0x5F and 0xB0 to 0x10F, unused ones too, as the reference clears them.
	add    r12, r4, #0x04
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	stmia  r12, {r0-r2}
	add    r12, r4, #0xB0
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	stmia  r12!, {r0-r3}
	mov    r0, #0x100
	strh   r0, [r4, #0x20]
	strh   r0, [r4, #0x26]
	strh   r0, [r4, #0x30]
	strh   r0, [r4, #0x36]
	@ The timers stop when the reference's do, lr 0 after the wait.
	mov    lr, #53
0:
	subs   lr, lr, #1
	bne    0b
	wait   3
	stmia  r12, {r1-r3, lr}
	strh   r3, [r12, #0x32]
	@ As long as the reference takes.
	mov    r12, #8
0:
	subs   r12, r12, #1
	bne    0b
1:
	mov    r6, #0x20
	ldr    r1, =0x04000110
	mov    r2, #8
	thumbBl ramRegion
	mov    r2, r6, lsl #10
	strh   r2, [r1, #4]
	add    r1, r1, #0x10
	mov    r2, #7
	strb   r2, [r1]
	thumbBl ramRegion
	mov    r6, #0x40
	tst    r7, r6
	beq    1f
	@ Sound off, which clears its registers, and the bias's level kept.
	mov    r1, #0x80
	add    r1, r1, r4
	@ As long as the reference takes.
	b      0f
0:
	strh   r3, [r1, #4]
	strh   r3, [r1, #2]
	strh   r3, [r1]
	ldrh   r0, [r1, #8]
	mov    r0, r0, lsl #22
	mov    r0, r0, lsr #22
	strh   r0, [r1, #8]
	@ Both banks of wave RAM: bit 6 of SOUND3CNT_L picks the one the CPU sees.
	sub    r1, r1, #0x10
	strh   r6, [r1]
	add    r1, r1, #0x20
	mov    r2, #8
	thumbBl ramRegion
	sub    r1, r1, #0x40
	strh   r3, [r1]
	add    r1, r1, #0x20
	mov    r2, #8
	thumbBl ramRegion
	@ Sound off again, as the reference does, after as long as it takes.
	nop
	mov    r1, #0x80
	add    r1, r1, r4
	strh   r3, [r1, #4]
1:
	mov    r6, #1
	mov    r1, r6, lsl #25
	mov    r2, r6, lsl #16
	thumbBl ramRegion
	mov    r6, #8
	mov    r1, #6
	mov    r1, r1, lsl #24
	mov    r2, #0x6000
	thumbBl ramRegion
	mov    r6, #0x10
	mov    r1, #7
	mov    r1, r1, lsl #24
	mov    r2, #0x100
	thumbBl ramRegion
	mov    r6, #4
	mov    r1, #5
	mov    r1, r1, lsl #24
	mov    r2, #0x100
	thumbBl ramRegion
	mov    r6, #2
	mov    r1, #3
	mov    r1, r1, lsl #24
	ldr    r2, iwramWords
	thumbBl ramRegion
	add    sp, sp, #4
	ldmfd  sp!, {r4-r7}
	ldmfd  sp!, {r3}
	bx     r3

@ If r7 has the bit in r6, r2 words at r1 filled from the zero at sp, by CpuFastSet with r5's fill
@ bit, which returns to the caller.
ramRegion:
	tst    r7, r6
	bne    0f
	bx     lr
0:
	mov    r0, sp
	orr    r2, r2, r5
	b      cpuFastSetVeneer
.ltorg
fillWords:
	.word  0x85000000
iwramWords:
	.word  0x1F80

@ Z clear if an interrupt in r1 is flagged: r0 is left those, which are cleared.
intrCheck:
	mov    r12, #0x04000000
	strb   r3, [r12, #0x208]
	ldrh   r11, [r12, #-8]
	ands   r0, r11, r1
	eorne  r11, r0, r11
	strhne r11, [r12, #-8]
	strb   r4, [r12, #0x208]
	bx     lr

Stop:
	mov    r11, #0x04000000
	mov    r2, #0x80
	strb   r2, [r11, #0x301]
	bx     lr

@ r2: the value for HALTCNT.
CustomHalt:
	mov    r11, #0x04000000
	strb   r2, [r11, #0x301]
	bx     lr

VBlankIntrWait:
	mov    r1, #1
	mov    r0, #1
@ r0: return at once if an interrupt in r1 was already flagged, r1: the interrupts to wait for.
@ The game's handler flags the interrupts it took at 0x03007FF8, mirrored at 0x03FFFFF8; only the
@ ones waited for are cleared, and returned in r0. Each pass halts, then checks the flags with IME
@ off, turning it on after, whatever the game left it. With r0 0 the first halt goes through the
@ address the SWI entry left in r12, into the BIOS, where it does nothing, as on the reference;
@ otherwise the flags are checked once first, and cleared. r3 is left 0, r4 is 1 meanwhile.
IntrWait:
	stmfd  sp!, {r4, lr}
	mov    r4, #1
	mov    r3, #0
	teq    r0, #0
	blne   intrCheck
0:
	strb   r3, [r12, #0x301]
	bl     intrCheck
	beq    0b
	ldmfd  sp!, {r4, lr}
	bx     lr

@ r0 = the square root of r0, rounded down, by Newton's method from a power of two at or above it,
@ each step dividing as Div does: r1 the estimate, r12 the shifted divisor, r3 the quotient, r4 the
@ estimate before, r2 the number. r1 is left the last estimate tried, and r3 r0 divided by the one
@ before.
Sqrt:
	stmfd  sp!, {r4}
	mov    r2, r0
	mov    r1, #1
1:
	cmp    r1, r0
	movlo  r1, r1, lsl #1
	movlo  r0, r0, lsr #1
	blo    1b
2:
	mov    r3, #0
	mov    r0, r2
	mov    r12, r1
	mov    r4, r1
3:
	cmp    r12, r0, lsr #1
	movls  r12, r12, lsl #1
	blo    3b
4:
	cmp    r0, r12
	adc    r3, r3, r3
	subcs  r0, r0, r12
	teq    r1, r12
	movne  r12, r12, lsr #1
	bne    4b
	add    r1, r3, r1
	mov    r1, r1, lsr #1
	cmp    r1, r4
	blo    2b
	mov    r0, r4
	ldmfd  sp!, {r4}
	bx     lr

@ r0 = the arc tangent of r0, a 1.14 fixed-point tangent from -1 to 1, as 0x4000 to the quarter turn,
@ by mGBA's polynomial in a = -(r0^2 >> 14). r1 is left a, and r3 the polynomial; r2 is kept, r12 not. The
@ multiplies' operands are in the order whose timing matches the reference's.
arcTanVeneer:
	adr    r3, ArcTan
	bx     r3
ArcTan:
	mul    r1, r0, r0
	mov    r1, r1, asr #14
	rsb    r1, r1, #0
	mov    r12, #0xA9
	mul    r3, r1, r12
	mov    r12, r3, asr #14
	add    r3, r12, #0x390
	mul    r12, r1, r3
	mov    r3, r12, asr #14
	add    r3, r3, #0x1C
	add    r3, r3, #0x900
	mul    r12, r1, r3
	mov    r3, r12, asr #14
	add    r3, r3, #0xB6
	add    r3, r3, #0xF00
	mul    r12, r1, r3
	mov    r3, r12, asr #14
	add    r3, r3, #0xAA
	add    r3, r3, #0x1600
	mul    r12, r1, r3
	mov    r3, r12, asr #14
	add    r3, r3, #0x81
	add    r3, r3, #0x2000
	mul    r12, r1, r3
	mov    r3, r12, asr #14
	add    r3, r3, #0x51
	add    r3, r3, #0x3600
	mul    r12, r1, r3
	mov    r3, r12, asr #14
	add    r3, r3, #0xF9
	add    r3, r3, #0xA200
	mul    r0, r3, r0
	mov    r0, r0, asr #16
	bx     lr

@ r0 = the angle of the point (r0, r1), 0x10000 to the full turn: ArcTan of the smaller coordinate
@ over the larger, turned to its octant and not cut to 16 bits, so that just below the x axis is
@ 0x10000. r2-r7 hold x and y shifted up 14, their negations, 0x4000 and 0x8000. Div and ArcTan are
@ called through veneers, as the reference's are. r1 is left as ArcTan leaves it, and r3 the way
@ back, which it returns through.
ArcTan2:
	stmfd  sp!, {r4-r7, lr}
	cmp    r1, #0
	bne    1f
	cmp    r0, #0
	blt    0f
	mov    r0, #0
	b      9f
0:
	mov    r0, #0x80
	mov    r0, r0, lsl #8
	b      9f
1:
	cmp    r0, #0
	bne    2f
	cmp    r1, #0
	blt    0f
	mov    r0, #0x40
	mov    r0, r0, lsl #8
	b      9f
0:
	mov    r0, #0xC0
	mov    r0, r0, lsl #8
	b      9f
2:
	mov    r2, r0
	mov    r2, r2, lsl #14
	mov    r3, r1
	mov    r3, r3, lsl #14
	rsb    r4, r0, #0
	rsb    r5, r1, #0
	mov    r6, #0x40
	mov    r6, r6, lsl #8
	mov    r7, r6, lsl #1
	cmp    r1, #0
	blt    5f
	cmp    r0, #0
	blt    3f
	cmp    r0, r1
	blt    4f
	@ East: y / x.
	mov    r1, r0
	mov    r0, r3
	thumbBl divVeneer
	thumbBl arcTanVeneer
	b      9f
3:
	cmp    r4, r1
	blt    4f
6:
	@ West: y / x, half a turn on.
	mov    r1, r0
	mov    r0, r3
	thumbBl divVeneer
	thumbBl arcTanVeneer
	add    r0, r0, r7
	b      9f
4:
	@ North: x / y, back from a quarter turn.
	mov    r0, r2
	thumbBl divVeneer
	thumbBl arcTanVeneer
	sub    r0, r6, r0
	b      9f
5:
	cmp    r0, #0
	bge    7f
	cmp    r4, r5
	bgt    6b
8:
	@ South: x / y, back from three quarters.
	mov    r0, r2
	thumbBl divVeneer
	thumbBl arcTanVeneer
	add    r6, r6, r7
	sub    r0, r6, r0
	b      9f
7:
	cmp    r0, r5
	blt    8b
	@ East below the axis: y / x, a whole turn on.
	mov    r1, r0
	mov    r0, r3
	thumbBl divVeneer
	thumbBl arcTanVeneer
	mov    r7, r7, lsl #1
	add    r0, r0, r7
9:
	ldmfd  sp!, {r4-r7}
	ldmfd  sp!, {r3}
	bx     r3

@ r0 / r1 for ArcTan2 and the sound's settings, rounded toward zero, as Div divides. r0-r3 and r12
@ are changed.
.global divide
divide:
	eor    r12, r0, r1
	cmp    r0, #0
	rsblt  r0, r0, #0
	cmp    r1, #0
	rsblt  r1, r1, #0
	mov    r2, r1
1:
	cmp    r1, r0, lsr #1
	movls  r1, r1, lsl #1
	blo    1b
	mov    r3, #0
2:
	cmp    r0, r1
	subcs  r0, r0, r1
	adc    r3, r3, r3
	teq    r1, r2
	movne  r1, r1, lsr #1
	bne    2b
	eor    r0, r3, r12, asr #31
	sub    r0, r0, r12, asr #31
	bx     lr

@ r0: source, 20 bytes each: the 24.8 centre in the original, the 16-bit centre on screen, 8.8
@ scales x and y, and the angle; r1: destination, 16 bytes each: the matrix, then the 24.8 start
@ point; r2: count. The angle's top byte (r8) picks the cosine (r12) and sine (r11) from the
@ table, whose address r11 holds first. The multiplies' operands are in the order that sets their
@ time as the reference's. Both pointers are left past what was read and written, and r3 the last matrix's
@ first entry, which like the others isn't cut to 16 bits for the start point.
BgAffineSet:
	stmfd  sp!, {r4-r11}
1:
	subs   r2, r2, #1
	blt    2f
	ldrh   r8, [r0, #16]
	mov    r8, r8, lsr #8
	adr    r11, sine
	add    r7, r8, #64
	and    r7, r7, #0xFF
	mov    r7, r7, lsl #1
	ldrsh  r12, [r11, r7]
	mov    r7, r8, lsl #1
	ldrsh  r11, [r11, r7]
	ldrsh  r9, [r0, #12]
	ldrsh  r10, [r0, #14]
	mul    r7, r12, r9
	mov    r3, r7, asr #14
	mul    r7, r11, r9
	mov    r4, r7, asr #14
	mul    r7, r11, r10
	mov    r5, r7, asr #14
	mul    r7, r12, r10
	mov    r6, r7, asr #14
	@ The start point: the centre in the original less the matrix times the centre on screen.
	ldmia  r0!, {r9, r10, r11}
	mov    r12, r11, lsl #16
	mov    r12, r12, asr #16
	mov    r11, r11, asr #16
	rsb    r7, r12, #0
	mla    r9, r3, r7, r9
	mla    r7, r4, r11, r9
	str    r7, [r1, #8]
	rsb    r7, r12, #0
	mla    r10, r5, r7, r10
	rsb    r7, r11, #0
	mla    r7, r6, r7, r10
	str    r7, [r1, #12]
	strh   r3, [r1]
	rsb    r4, r4, #0
	strh   r4, [r1, #2]
	strh   r5, [r1, #4]
	strh   r6, [r1, #6]
	add    r0, r0, #8
	add    r1, r1, #16
	b      1b
2:
	ldmfd  sp!, {r4-r11}
	bx     lr

@ r0: source, 8 bytes each: 8.8 scales x and y, and the angle; r1: destination; r2: count; r3: the
@ distance between the matrix's entries, 2 packed or 8 for OAM. As BgAffineSet. Both pointers are
@ left past what was read and written.
ObjAffineSet:
	stmfd  sp!, {r8-r11}
1:
	subs   r2, r2, #1
	blt    2f
	ldrh   r10, [r0, #4]
	mov    r10, r10, lsr #8
	adr    r11, sine
	add    r9, r10, #64
	and    r9, r9, #0xFF
	mov    r9, r9, lsl #1
	ldrsh  r12, [r11, r9]
	mov    r9, r10, lsl #1
	ldrsh  r11, [r11, r9]
	ldrsh  r10, [r0]
	ldrsh  r8, [r0, #2]
	mul    r9, r12, r10
	mov    r9, r9, asr #14
	strh   r9, [r1], r3
	mul    r9, r11, r10
	mov    r9, r9, asr #14
	rsb    r9, r9, #0
	strh   r9, [r1], r3
	mul    r9, r11, r8
	mov    r9, r9, asr #14
	strh   r9, [r1], r3
	mul    r9, r12, r8
	mov    r9, r9, asr #14
	strh   r9, [r1], r3
	add    r0, r0, #8
	b      1b
2:
	ldmfd  sp!, {r8-r11}
	bx     lr

@ The sines of the 256 angles in 1.14 fixed point, as ObjAffineSet returns them on a GBA; measured by
@ tests/bioscheck (sine.py), not computed.
sine:
	.hword 0, 402, 803, 1205, 1605, 2005, 2404, 2801, 3196, 3589, 3980, 4369, 4756, 5139, 5519, 5896
	.hword 6269, 6639, 7005, 7366, 7723, 8075, 8423, 8765, 9102, 9434, 9759, 10079, 10393, 10701, 11002, 11297
	.hword 11585, 11866, 12139, 12406, 12665, 12916, 13159, 13395, 13622, 13842, 14053, 14255, 14449, 14634, 14810, 14978
	.hword 15136, 15286, 15426, 15557, 15678, 15790, 15892, 15985, 16069, 16142, 16206, 16260, 16305, 16339, 16364, 16379
	.hword 16384, 16379, 16364, 16339, 16305, 16260, 16206, 16142, 16069, 15985, 15892, 15790, 15678, 15557, 15426, 15286
	.hword 15136, 14978, 14810, 14634, 14449, 14255, 14053, 13842, 13622, 13395, 13159, 12916, 12665, 12406, 12139, 11866
	.hword 11585, 11297, 11002, 10701, 10393, 10079, 9759, 9434, 9102, 8765, 8423, 8075, 7723, 7366, 7005, 6639
	.hword 6269, 5896, 5519, 5139, 4756, 4369, 3980, 3589, 3196, 2801, 2404, 2005, 1605, 1205, 803, 402
	.hword 0, -402, -803, -1205, -1605, -2005, -2404, -2801, -3196, -3589, -3980, -4369, -4756, -5139, -5519, -5896
	.hword -6269, -6639, -7005, -7366, -7723, -8075, -8423, -8765, -9102, -9434, -9759, -10079, -10393, -10701, -11002, -11297
	.hword -11585, -11866, -12139, -12406, -12665, -12916, -13159, -13395, -13622, -13842, -14053, -14255, -14449, -14634, -14810, -14978
	.hword -15136, -15286, -15426, -15557, -15678, -15790, -15892, -15985, -16069, -16142, -16206, -16260, -16305, -16339, -16364, -16379
	.hword -16384, -16379, -16364, -16339, -16305, -16260, -16206, -16142, -16069, -15985, -15892, -15790, -15678, -15557, -15426, -15286
	.hword -15136, -14978, -14810, -14634, -14449, -14255, -14053, -13842, -13622, -13395, -13159, -12916, -12665, -12406, -12139, -11866
	.hword -11585, -11297, -11002, -10701, -10393, -10079, -9759, -9434, -9102, -8765, -8423, -8075, -7723, -7366, -7005, -6639
	.hword -6269, -5896, -5519, -5139, -4756, -4369, -3980, -3589, -3196, -2801, -2404, -2005, -1605, -1205, -803, -402

@ r0: source, r1: destination, r2: the unpacking's settings: the source length in bytes, the source
@ and destination widths in bits, and a bias added to each unit (bit 31: to zero units as well).
@ Units are taken from each byte while its 8 bits aren't used up, under the mask 0xFF >> (8 - width)
@ in r9 shifted along, and a word gathered in lr is written once 32 or more bits are filled. The bias
@ is kept on the stack and loaded for each unit, as on the reference. No width is refused: 0 bits
@ in never returns. Both pointers are left past what was read and written, r2 the destination width,
@ and r3 the bits filled but not written.
BitUnPack:
	stmfd  sp!, {r4-r11, lr}
	@ As long as the reference takes.
	nop
	ldrh   r10, [r2]
	mov    r12, r10
	bl     checkSource
	beq    9f
	ldrb   r4, [r2, #2]
	rsb    r7, r4, #8
	mov    lr, #0
	ldr    r8, [r2, #4]
	mov    r11, r8, lsr #31
	ldr    r8, [r2, #4]
	mov    r8, r8, lsl #1
	mov    r8, r8, lsr #1
	str    r8, [sp, #-4]!
	ldrb   r2, [r2, #3]
	mov    r3, #0
1:
	subs   r10, r10, #1
	bmi    8f
	mov    r8, #0xFF
	mov    r9, r8, lsr r7
	ldrb   r5, [r0], #1
	mov    r6, #0
2:
	cmp    r6, #8
	bge    1b
	and    r8, r5, r9
	mov    r12, r8, lsr r6
	@ The bias goes on unless both the unit and bit 31 are 0.
	cmn    r12, r11
	beq    3f
	ldr    r8, [sp]
	add    r12, r12, r8
3:
	orr    lr, lr, r12, lsl r3
	add    r3, r3, r2
	cmp    r3, #32
	blt    4f
	str    lr, [r1], #4
	mov    lr, #0
	mov    r3, #0
4:
	mov    r9, r9, lsl r4
	add    r6, r6, r4
	b      2b
8:
	add    sp, sp, #4
	ldmfd  sp!, {r4-r11, lr}
	bx     lr
9:
	@ As long as the reference takes.
	nop
	ldmfd  sp!, {r4-r11, lr}
	bx     lr

@ The decompressors and filters check their source with checkSource, past its header, as long as
@ their output; Huffman as 32 MiB. A source refused returns with r0 past the header.

@ Run-length: r0 the source, whose header gives the size; r1 the destination. Each block's first
@ byte, with bit 7 set, starts a run of bits 0-6 + 3 copies of the next byte, or else bits 0-6 + 1
@ bytes as they are. r7 is what's left, r2 the block's length. r1 is left past what was written,
@ r0 past the source read, and r3 the way back, which it returns through.
RlUnCompWram:
	stmfd  sp!, {r4-r7, lr}
	ldmia  r0!, {r3}
	mov    r7, r3, lsr #8
	mov    r4, r7
	@ As long as the reference takes.
	nop
	b      0f
0:
	adr    r3, 0f
	mov    r12, r4
	b      checkSourceR3
0:
	beq    9f
1:
	cmp    r7, #0
	ble    9f
	ldrb   r4, [r0]
	add    r0, r0, #1
	@ The carry is bit 7, a run.
	movs   r2, r4, lsl #25
	mov    r2, r2, lsr #25
	mov    r3, #0
	bcs    3f
	add    r2, r2, #1
	sub    r7, r7, r2
2:
	ldrb   r3, [r0]
	strb   r3, [r1]
	add    r0, r0, #1
	add    r1, r1, #1
	subs   r2, r2, #1
	bne    2b
	b      1b
3:
	add    r2, r2, #3
	sub    r7, r7, r2
	ldrb   r5, [r0]
	add    r0, r0, #1
4:
	strb   r5, [r1]
	add    r1, r1, #1
	subs   r2, r2, #1
	bne    4b
	b      1b
9:
	ldmfd  sp!, {r4-r7}
	ldmfd  sp!, {r3}
	bx     r3

@ As RlUnCompWram, into VRAM a halfword at a time: r7 gathers the bytes, r4 is where the next goes
@ (0 or 8). The block's first byte and a run's byte are kept on the stack and loaded from there, as
@ on the reference. An odd last byte isn't written: r1 is left after the last whole halfword.
RlUnCompVram:
	stmfd  sp!, {r4-r7, lr}
	sub    sp, sp, #8
	mov    r7, #0
	ldmia  r0!, {r3}
	mov    r5, r3, lsr #8
	mov    r4, r5
	@ As long as the reference takes.
	nop
	b      0f
0:
	adr    r3, 0f
	mov    r12, r4
	b      checkSourceR3
0:
	beq    9f
	mov    r4, #0
1:
	cmp    r5, #0
	ble    9f
	ldrb   r3, [r0]
	str    r3, [sp]
	add    r0, r0, #1
	@ As long as the reference takes.
	b      0f
0:
	@ The carry is bit 7, a run.
	movs   r2, r3, lsl #25
	mov    r2, r2, lsr #25
	ldr    r6, [sp]
	mov    r3, #0
	bcs    3f
	add    r2, r2, #1
	sub    r5, r5, r2
2:
	ldrb   r6, [r0]
	mov    r6, r6, lsl r4
	orr    r7, r7, r6
	add    r0, r0, #1
	mov    r3, #8
	eors   r4, r4, r3
	bne    0f
	strh   r7, [r1]
	add    r1, r1, #2
	mov    r7, #0
0:
	subs   r2, r2, #1
	bne    2b
	b      1b
3:
	add    r2, r2, #3
	sub    r5, r5, r2
	ldrb   r6, [r0]
	str    r6, [sp, #4]
	add    r0, r0, #1
4:
	ldr    r6, [sp, #4]
	mov    r6, r6, lsl r4
	orr    r7, r7, r6
	mov    r3, #8
	eors   r4, r4, r3
	bne    0f
	strh   r7, [r1]
	add    r1, r1, #2
	mov    r7, #0
0:
	subs   r2, r2, #1
	bne    4b
	b      1b
9:
	add    sp, sp, #8
	ldmfd  sp!, {r4-r7}
	ldmfd  sp!, {r3}
	bx     r3

@ LZ77: r0 the source, whose header gives the size; r1 the destination. A flag byte tells for the next
@ 8 blocks whether each is a byte as it is (0) or a copy (1) of 3 to 18 bytes from up to 4096 bytes
@ back, which runs its whole length even past the size. r0 is left past the source read, r1 past
@ what was written (in VRAM, after the last whole halfword), and r3 0, or in VRAM a byte left
@ waiting for its partner.
Lz77UnCompWram:
	stmfd  sp!, {r4-r6, lr}
	ldr    r12, [r0], #4
	mov    r4, r12, lsr #8
	mov    r12, r4
	bl     checkSource
	beq    9f
	@ r4 what's left, lr the flags, r6 how many are left in it.
0:
	cmp    r4, #0
	ble    9f
	ldrb   lr, [r0], #1
	mov    r6, #8
1:
	subs   r6, r6, #1
	bmi    0b
	tst    lr, #0x80
	bne    3f
	ldrb   r2, [r0], #1
	strb   r2, [r1], #1
	subs   r4, r4, #1
	b      2f
3:
	@ The copy's first byte is read again for the displacement, as the reference reads it; r5 is
	@ the displacement + 1, r3 the length.
	ldrb   r12, [r0], #1
	mov    r2, #3
	add    r3, r2, r12, lsr #4
	ldrb   r2, [r0, #-1]
	and    r12, r2, #0xF
	mov    r5, r12, lsl #8
	ldrb   r2, [r0], #1
	orr    r12, r2, r5
	add    r5, r12, #1
	subs   r4, r4, r3
4:
	ldrb   r12, [r1, -r5]
	strb   r12, [r1], #1
	subs   r3, r3, #1
	bgt    4b
2:
	cmp    r4, #0
	mov    lr, lr, lsl #1
	bgt    1b
	b      0b
9:
	ldmfd  sp!, {r4-r6, lr}
	bx     lr

@ LZ77 into VRAM, a halfword at a time: r3 gathers the bytes, r2 is where the next goes in it (0 or
@ 8), r5 what's left, r6 the flags and r9 how many are left in it. A copied byte is read from VRAM
@ as a halfword (so one not yet written reads as VRAM has it): r7 is the displacement + 1, and lr the
@ byte's place in the halfword it's read from, which changes each byte.
Lz77UnCompVram:
	stmfd  sp!, {r4-r10, lr}
	mov    r3, #0
	ldr    r10, [r0], #4
	mov    r5, r10, lsr #8
	mov    r2, #0
	mov    r12, r5
	bl     checkSource
	beq    9f
0:
	cmp    r5, #0
	ble    9f
	ldrb   r6, [r0], #1
	mov    r9, #8
1:
	subs   r9, r9, #1
	bmi    0b
	tst    r6, #0x80
	bne    3f
	ldrb   r4, [r0], #1
	orr    r3, r3, r4, lsl r2
	subs   r5, r5, #1
	eors   r2, r2, #8
	strheq r3, [r1], #2
	moveq  r3, #0
	b      2f
3:
	@ The copy's first byte is read again for the displacement, as the reference reads it.
	ldrb   r4, [r0], #1
	mov    r10, #3
	add    r8, r10, r4, lsr #4
	ldrb   r4, [r0, #-1]
	and    r10, r4, #0xF
	mov    r7, r10, lsl #8
	ldrb   r4, [r0], #1
	orr    r10, r4, r7
	add    r7, r10, #1
	rsb    r10, r2, #8
	and    r4, r7, #1
	eor    lr, r10, r4, lsl #3
	subs   r5, r5, r8
4:
	eor    lr, lr, #8
	rsb    r10, r2, #8
	add    r10, r7, r10, lsr #3
	mov    r10, r10, lsr #1
	mov    r10, r10, lsl #1
	ldrh   r4, [r1, -r10]
	mov    r10, #0xFF
	and    r10, r4, r10, lsl lr
	mov    r10, r10, lsr lr
	orr    r3, r3, r10, lsl r2
	eors   r2, r2, #8
	strheq r3, [r1], #2
	moveq  r3, #0
	subs   r8, r8, #1
	bgt    4b
2:
	cmp    r5, #0
	mov    r6, r6, lsl #1
	bgt    1b
	b      0b
9:
	ldmfd  sp!, {r4-r10, lr}
	bx     lr

@ Huffman: r0 the source: a header with the bits per symbol and the size, the tree's size in
@ halfwords less 1 and its nodes, then the stream in words, from the top bit. A node's bits 0-5 give
@ its children's pair, 2 bytes on from it after rounding down, and bits 7 and 6 whether the left and
@ right child is a symbol. r1 is the destination, written in words of 4 + (bits & 7) symbols, each
@ shifting the word down and going in at its top. The source is checked as 32 MiB long. r8 is the
@ node, r4 the root, r9 the stream's word with r11 its bits left, lr the size left, r12 the symbols in
@ the word, whose count is kept on the stack and loaded each time, as on the reference. r0 is left
@ past the stream read, r1 past what was written, and r3 the last word.
HuffUnComp:
	stmfd  sp!, {r4-r11, lr}
	@ As long as the reference takes.
	nop
	mov    r12, #0x02000000
	bl     checkSource
	beq    9f
	add    r8, r0, #4
	add    r4, r0, #5
	ldrb   r6, [r0]
	and    r7, r6, #0xF
	mov    r3, #0
	mov    r12, #0
	and    r6, r7, #7
	add    r2, r6, #4
	str    r2, [sp, #-4]!
	ldr    r6, [r0]
	mov    lr, r6, lsr #8
	ldrb   r6, [r0, #4]
	add    r6, r6, #1
	add    r0, r8, r6, lsl #1
	mov    r8, r4
0:
	cmp    lr, #0
	ble    8f
	mov    r11, #32
	ldr    r9, [r0], #4
1:
	subs   r11, r11, #1
	bmi    0b
	@ r5 the bit, r10 the node shifted by it so that bit 7 says whether the child is a symbol.
	mov    r6, #1
	and    r5, r6, r9, lsr #31
	ldrb   r10, [r8]
	mov    r10, r10, lsl r5
	mov    r6, r8, lsr #1
	mov    r6, r6, lsl #1
	ldrb   r2, [r8]
	and    r2, r2, #0x3F
	add    r2, r2, #1
	add    r6, r6, r2, lsl #1
	add    r8, r6, r5
	tst    r10, #0x80
	beq    2f
	mov    r3, r3, lsr r7
	ldrb   r6, [r8]
	rsb    r2, r7, #32
	orr    r3, r3, r6, lsl r2
	mov    r8, r4
	add    r12, r12, #1
	ldr    r2, [sp]
	cmp    r12, r2
	streq  r3, [r1], #4
	subeq  lr, lr, #4
	moveq  r12, #0
2:
	cmp    lr, #0
	mov    r9, r9, lsl #1
	bgt    1b
	b      0b
8:
	add    sp, sp, #4
	ldmfd  sp!, {r4-r11, lr}
	bx     lr
9:
	@ As long as the reference takes.
	nop
	ldmfd  sp!, {r4-r11, lr}
	bx     lr

@ The Diff filters: each unit is the sum of the units so far. r0 is the source, whose header gives the
@ size in bytes; r1 the destination. Both are left past what was read and written, the sum in r2
@ (beyond 16 bits for Diff16bitUnFilter, which leaves r3 the last difference and returns through
@ r2; the others return through r3, which they leave the way back).
Diff8BitUnFilterWram:
	stmfd  sp!, {r4, lr}
	ldmia  r0!, {r4}
	mov    r4, r4, lsr #8
	@ As long as the reference takes.
	nop
	b      0f
0:
	adr    r3, 0f
	mov    r12, r4
	b      checkSourceR3
0:
	beq    9f
	ldrb   r2, [r0]
	add    r0, r0, #1
	strb   r2, [r1]
	add    r1, r1, #1
1:
	subs   r4, r4, #1
	ble    9f
	ldrb   r3, [r0]
	add    r2, r2, r3
	add    r0, r0, #1
	strb   r2, [r1]
	add    r1, r1, #1
	b      1b
9:
	ldmfd  sp!, {r4}
	ldmfd  sp!, {r3}
	bx     r3

@ Into VRAM a halfword at a time: r7 is the sum, r2 gathers the pair, r4 is where the next byte
@ goes (0 or 8). An odd last byte isn't written.
Diff8BitUnFilterVram:
	stmfd  sp!, {r4-r7, lr}
	ldmia  r0!, {r3}
	mov    r5, r3, lsr #8
	mov    r4, r5
	@ As long as the reference takes.
	nop
	b      0f
0:
	adr    r3, 0f
	mov    r12, r4
	b      checkSourceR3
0:
	beq    9f
	mov    r4, #8
	ldrb   r7, [r0]
	add    r0, r0, #1
	mov    r2, r7
1:
	subs   r5, r5, #1
	ble    9f
	ldrb   r3, [r0]
	add    r7, r7, r3
	add    r0, r0, #1
	mov    r6, r7, lsl #24
	mov    r6, r6, lsr #24
	mov    r6, r6, lsl r4
	orr    r2, r2, r6
	mov    r3, #8
	eors   r4, r4, r3
	bne    1b
	strh   r2, [r1]
	add    r1, r1, #2
	mov    r2, #0
	b      1b
9:
	ldmfd  sp!, {r4-r7}
	ldmfd  sp!, {r3}
	bx     r3

Diff16BitUnFilter:
	stmfd  sp!, {r4, lr}
	ldmia  r0!, {r4}
	mov    r4, r4, lsr #8
	@ As long as the reference takes.
	nop
	b      0f
0:
	adr    r3, 0f
	mov    r12, r4
	b      checkSourceR3
0:
	beq    9f
	ldrh   r2, [r0]
	add    r0, r0, #2
	strh   r2, [r1]
	add    r1, r1, #2
1:
	subs   r4, r4, #2
	ble    9f
	ldrh   r3, [r0]
	add    r2, r2, r3
	add    r0, r0, #2
	strh   r2, [r1]
	add    r1, r1, #2
	b      1b
9:
	ldmfd  sp!, {r4}
	ldmfd  sp!, {r2}
	bx     r2

@ r0: source, r1: destination, r2: count in bits 0-20, bit 24 fill, bit 26 words. Halfwords go
@ through r5 as an offset; words move the pointers, aligned as ldm and stm align them. The source
@ is checked as count words long, whatever the width. r3 is left the way back, which it returns
@ through.
.global CpuSet
CpuSet:
	stmfd  sp!, {r4, r5, lr}
	mov    r4, r2, lsl #11
	mov    r4, r4, lsr #9
	@ As long as the reference takes.
	nop
	b      0f
0:
	adr    r3, 0f
	mov    r12, r4
	b      checkSourceR3
0:
	beq    4f
	mov    r5, #0
	@ The carry is bit 26, words, then bit 24, fill.
	movs   r3, r2, lsr #27
	bcc    2f
	add    r5, r1, r4
	movs   r3, r2, lsr #25
	bcc    1f
	ldmia  r0!, {r3}
0:
	cmp    r1, r5
	bge    4f
	stmia  r1!, {r3}
	b      0b
1:
	cmp    r1, r5
	bge    4f
	ldmia  r0!, {r3}
	stmia  r1!, {r3}
	b      1b
2:
	mov    r4, r4, lsr #1
	movs   r3, r2, lsr #25
	bcc    3f
	ldrh   r3, [r0]
0:
	cmp    r5, r4
	bge    4f
	strh   r3, [r1, r5]
	add    r5, r5, #2
	b      0b
3:
	cmp    r5, r4
	bge    4f
	ldrh   r3, [r0, r5]
	strh   r3, [r1, r5]
	add    r5, r5, #2
	b      3b
4:
	ldmfd  sp!, {r4, r5}
	ldmfd  sp!, {r3}
	bx     r3

checkSourceR3:
	sourceCheck r3

@ r0: 0 to bring the sound bias level down to 0, or anything else to bring it up to 0x200 (a level
@ above that stays). It moves 2 a step, each step starting over and ending in a delay. r1 is left
@ the level, r3 the register's address.
SoundBias:
	mov    r1, #2
	mov    r1, r1, lsl #8
	mov    r12, r1
	ldr    r3, =0x04000088
	ldrh   r2, [r3]
	@ As long as the reference takes.
	b      0f
0:
	mov    r1, r2, lsl #22
	mov    r1, r1, lsr #22
	cmp    r0, #0
	beq    2f
	cmp    r1, r12
	bge    3f
	add    r2, r2, #2
	b      1f
2:
	cmp    r1, #0
	beq    3f
	sub    r2, r2, #2
1:
	strh   r2, [r3]
	mov    r2, #8
0:
	subs   r2, r2, #1
	bpl    0b
	b      SoundBias
3:
	bx     lr

@ r0: wave data, with its sample rate at +4, r1: a MIDI key, r2: 256ths of the way to the next key.
@ r0 is left the rate scaled by 2^((key + fine - 180) / 12), and r1 the scale. Keys above 178 count
@ as 178 and 255/256. As the reference does: each key's scale is its note's from the top octave
@ shifted down by its octave (r4 this key's, r1 the next's), and the two products go through a
@ veneer to a umull, the first giving the fine part of the way between them.
MidiKey2Freq:
	stmfd  sp!, {r4-r7, lr}
	mov    r2, r2, lsl #24
	mov    r7, r0
	cmp    r1, #178
	bls    1f
	@ Loaded, not moved, as the reference loads them.
	ldr    r2, fineTop
	mov    r1, #178
1:
	ldr    r0, keyScalesAddress
	ldrb   r3, [r0, r1]
	mov    r4, r3, lsl #28
	mov    r4, r4, lsr #28
	mov    r4, r4, lsl #2
	mov    r5, r0
	adr    r5, topOctave
	ldr    r4, [r5, r4]
	mov    r6, r3, lsr #4
	mov    r4, r4, lsr r6
	add    r0, r0, r1
	ldrb   r0, [r0, #1]
	mov    r1, r0, lsl #28
	mov    r1, r1, lsr #28
	mov    r1, r1, lsl #2
	ldr    r1, [r5, r1]
	mov    r0, r0, lsr #4
	mov    r1, r1, lsr r0
	sub    r0, r1, r4
	mov    r1, r2
	thumbBl umulVeneer
	add    r1, r4, r0
	ldr    r0, [r7, #4]
	thumbBl umulVeneer
	ldmfd  sp!, {r4-r7}
	ldmfd  sp!, {r3}
	bx     r3
fineTop:
	.word  0xFF000000
keyScalesAddress:
	.word  keyScales

@ r0 = the top word of r0 * r1.
umulVeneer:
	adr    r2, 0f
	bx     r2
0:
	umull  r2, r3, r0, r1
	mov    r0, r3
	bx     lr

@ The same for the BIOS's own use: r0 the frequency, r1-r3 and r12 changed.
.global midiKeyToFreq
midiKeyToFreq:
	stmfd  sp!, {r4}
	ldr    r0, [r0, #4]
	cmp    r1, #178
	movhi  r1, #178
	movhi  r2, #255
	adr    r12, keyScales
	add    r12, r12, r1
	ldrb   r3, [r12]
	ldrb   r4, [r12, #1]
	adr    r12, topOctave
	and    r1, r3, #0x0F
	ldr    r1, [r12, r1, lsl #2]
	mov    r3, r3, lsr #4
	mov    r1, r1, lsr r3
	and    r3, r4, #0x0F
	ldr    r3, [r12, r3, lsl #2]
	mov    r4, r4, lsr #4
	mov    r3, r3, lsr r4
	sub    r3, r3, r1
	mov    r4, r2, lsl #24
	umull  r12, r2, r3, r4
	add    r1, r1, r2
	umull  r12, r2, r0, r1
	mov    r0, r2
	ldmfd  sp!, {r4}
	bx     lr
.ltorg

@ 2^31 * 2^(n/12), the scales of the top octave's notes.
topOctave:
	.word 0x80000000, 0x879C7C97, 0x8FACD61E, 0x9837F052, 0xA14517CC, 0xAADC0848
	.word 0xB504F334, 0xBFC886BB, 0xCB2FF52A, 0xD744FCCB, 0xE411F03A, 0xF1A1BF39

@ Each key's note, and in the top 4 bits how far its octave is below the top one.
keyScales:
	.set   key, 0
	.rept  180
	.byte  ((14 - key / 12) << 4) | (key % 12)
	.set   key, key + 1
	.endr
	.balign 4

@ The checksum of Nintendo's BIOS, which games compare. Interrupts are off from the msr until the
@ call returns, as on the reference.
GetBiosChecksum:
	mov    r0, #0
	mov    r3, #0
	mov    r12, #0xDF
	ldr    r2, [r3], #4
	msr    cpsr_c, r12
	@ As long as the reference takes to add up its 4096 words, with r3 left past the last.
1:
	add    r3, r3, #4
	cmp    r3, #0x4000
	wait   5
	blo    1b
	wait   1
	ldr    r0, =0xBAAE187F
	mov    r1, #1
	bx     lr

.ltorg

@ Power-on: the stacks, then the game, or a multiboot image in EWRAM without a cartridge. The game
@ starts as the reference leaves it: in System mode with the flags clear, the supervisor and
@ interrupt modes' SPSRs set to User mode, and JOY_TRANS written first, which sets JOYSTAT's bit 3
@ for a GameCube to see.
resetBase:
	mov    r0, #0x04000000
	mov    r1, #0
	str    r1, [r0, #0x154]
	msr    cpsr_c, #0xD3
	ldr    sp, =0x03007FE0
	msr    spsr_fsxc, #0x10
	msr    cpsr_c, #0xD2
	ldr    sp, =0x03007FA0
	msr    spsr_fsxc, #0x10
	msr    cpsr_c, #0xDF
	ldr    sp, =0x03007F00
	@ With no cartridge and no program in EWRAM, or with Select and Start held as the splash ends,
	@ a program comes over the cable before it fades; r11 is left 1 if one came.
	mov    r0, #0x8000000
	ldrb   r1, [r0, #3]
	ldr    r2, =0x20000C0
	ldr    r2, [r2]
	orrs   r1, r1, r2
	moveq  r11, #1
	movne  r11, #0
	bl     splash
	cmp    r11, #0
	beq    2f
	bl     multibootReceive
	mov    r11, #1
2:
	bl     splashEnd
	mov    r0, #0x04000000
	mov    r1, #1
	strb   r1, [r0, #0x300]
	add    r1, r0, #0x100
	@ JOYCNT's flags cleared: a program sent from a GameCube waits for them.
	mov    r2, #7
	strh   r2, [r1, #0x40]
	@ A program from a GameCube, boot mode 1, starts at 0xE0; one over the cable at 0xC0.
	ldr    lr, =0x20000C0
	ldrb   r2, [lr, #4]
	cmp    r2, #1
	addeq  lr, lr, #0x20
	cmp    r11, #1
	moveq  r1, #0
	beq    1f
	mov    lr, #0x8000003
	ldrb   r1, [lr], #-3
	cmp    r1, #0
	movne  r1, #0
	bne    1f
	ldr    lr, =0x20000C0
	ldr    r1, [lr]
	cmp    r1, #0
	mov    r1, #0
	bne    1f
	sub    lr, #0xC0
1:
	@ The game starts on line 126, 847 cycles into it, as on the reference.
	mov    r0, #0x04000000
2:
	ldrh   r2, [r0, #6]
	cmp    r2, #125
	bne    2b
3:
	ldrh   r2, [r0, #6]
	cmp    r2, #126
	bne    3b
	mov    r2, #BOOT_LINE_WAIT
4:
	subs   r2, r2, #1
	bne    4b
	wait   2
	mov    r0, #0
	msr    cpsr_fsxc, #0x1F
	bx     lr
	.word 0
	.word 0xE129F000

.ltorg

@ The other exceptions go to a debugging handler in the cartridge, if it has one.
undefBase:
pabtBase:
dabtBase:
fiqBase:
	ldr    sp, =0x03007FF0
	stmdb  sp!, {r12, lr}
	mrs    r12, spsr
	mrs    lr, cpsr
	stmdb  sp!, {r12, lr}
	mov    lr, #0x08000000
	ldrb   r12, [lr, #0x9C]
	cmp    r12, #0xA5
	bne    1f
	ldrb   r12, [lr, #0xB4]
	tst    r12, #0x80
	adr    lr, 1f
	ldrne  pc, =0x09FE2000
	ldreq  pc, =0x09FFC000
1:
	ldr    sp, =0x03007FF0
	ldr    r12, [sp, #-0x10]
	msr    spsr_fsxc, r12
	ldmdb  sp!, {r12, lr}
	subs   pc, lr, #4
	.word 0
	.word 0x03A0E004

@ Restarts the game, or the multiboot image if the byte at 0x03007FFA is set, with the stacks and
@ the top 512 bytes of IWRAM cleared.
SoftReset:
	msr    spsr_fsxc, #0
	mov    lr, #0
	ldr    sp, =0x03007F00
	msr    cpsr_c, #0x92
	msr    spsr_fsxc, #0
	mov    lr, #0
	ldr    sp, =0x03007FA0
	msr    cpsr_c, #0x93
	msr    spsr_fsxc, #0
	mov    lr, #0
	ldr    sp, =0x03007FE0
	mov    r0, #0x04000000
	sub    r1, r0, #0x200
	ldrb   r0, [r0, #-6]
	mov    r2, #0
	mov    r3, #0
	mov    r4, #0
	mov    r5, #0
	mov    r6, #0
	mov    r7, #0
	mov    r8, #0
	mov    r9, #0
	mov    r10, #0
	mov    r11, #0
	mov    r12, #0
1:
	stmia  r1!, {r2, r3, r4, r5, r6, r7, r8, r9}
	cmp    r1, #0x04000000
	bne    1b
	cmp    r0, #0
	mov    r0, #0
	mov    r1, #0
	moveq  lr, #0x08000000
	movne  lr, #0x02000000
	movs   pc, lr
	.word 0
	.word 0xE129F000

@ The power-on sequence again.
HardReset:
	msr    cpsr_c, #0xD3
	b      resetBase

.ltorg
