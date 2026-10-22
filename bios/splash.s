@ Copyright (c) 2026 Magnominus
@
@ This Source Code Form is subject to the terms of the Mozilla Public
@ License, v. 2.0. If a copy of the MPL was not distributed with this
@ file, You can obtain one at http://mozilla.org/MPL/2.0/.
@
@ armGBA's splash at power-on. On a white screen each letter of "ARMGBA" rises from the bottom at
@ three times its size, magenta and turning through red, orange, yellow, green and cyan as it reaches
@ its place, with a tick, then goes a little past and settles back blue; then "Open BIOS" comes in black from left to
@ right, and a chime plays, a gold spark flashing on the last letter with its last note (sprite 6,
@ palette bank 6, affine group 6).
@ The letters are affine sprites, 64x64 at 4 bits a pixel, double size, letter k in palette bank k
@ (its 3 coverage levels at 1-3) and affine group k. "Open BIOS" is drawn in mode 4: palette entry 0
@ is the white, band b's 3 levels at FIRST_BAND + 3b. splash-data.inc is written by splash.py.

.syntax unified
.arm

.include "macros.inc"

.section .rodata
.include "splash-data.inc"

.equ FIRST_BAND, 1
.equ ROW_BYTES, SPLASH_WIDTH / 4
.equ BUFFER, 0x02000000
@ The letters' tiles are put together here first, as VRAM takes no single bytes, then copied to the
@ sprite tiles from 512, the first that the bitmap modes leave to sprites.
.equ TILES, 0x02001000
.equ LETTER_TILES, 64 * 32
.equ SPRITE_TILES, 0x06014000
.equ SPARK_TILES, SPRITE_TILES + LETTER_TILES * SPLASH_LETTERS
.equ SPARK_TILE, 512 + 64 * SPLASH_LETTERS
.equ SPARK_START, CHIME + 7
@ Frames, as the reference's logo has them: the first letter from START, each STAGGER after the one
@ before; the chime at the reference's; "Open BIOS" before it, a band every 2 frames; then a hold,
@ which ends the splash after as long as the reference's logo lasts.
.equ START, 33
.equ STAGGER, 5
.equ CHIME, 168
.equ REVEAL, CHIME - 2 * SPLASH_BANDS - 6
.equ HOLD_END, 252
.if REVEAL < START + STAGGER * (SPLASH_LETTERS - 1) + SPLASH_SWEEP
.error "Open BIOS would come before the letters are blue"
.endif
@ Each letter's tick on channel 2: a quarter-duty square from TICK_ENVELOPE's volume, dying away.
.ifndef TICK_ENVELOPE
.equ TICK_ENVELOPE, 0x8140
.endif

.text

@ r11: nonzero for no cartridge. Runs to the end of the hold, before the fade, and leaves r11 1 if a
@ program is to come over the cable: with no cartridge, or with Select and Start held then.
.global splash
splash:
	mov    r9, lr
	mov    r10, #0x04000000
	mov    r0, #0x80
	strh   r0, [r10]
	@ Both palettes white, every sprite hidden, the logo unpacked and its rows made whole again.
	mov    r0, #0x05000000
	add    r1, r0, #0x400
	ldr    r2, =0x7FFF7FFF
0:
	str    r2, [r0], #4
	cmp    r0, r1
	blo    0b
	mov    r0, #0x07000000
	add    r1, r0, #0x400
	mov    r2, #0x0200
	mov    r3, #0
0:
	stmia  r0!, {r2, r3}
	cmp    r0, r1
	blo    0b
	ldr    r0, =splashLogo
	mov    r1, #BUFFER
	bl     unLz77
	ldr    r0, =BUFFER + ROW_BYTES
	ldr    r1, =BUFFER + SPLASH_PACKED
0:
	ldrb   r2, [r0, #-ROW_BYTES]
	ldrb   r3, [r0]
	eor    r3, r3, r2
	strb   r3, [r0], #1
	cmp    r0, r1
	blo    0b
	@ Each letter into its tiles, centred in its 64x64 cell: 8 tiles a row, 32 bytes a tile, 2
	@ pixels a byte, the left one low.
	mov    r0, #0
	mov    r1, #0
	mov    r2, #0
	mov    r3, #0
	ldr    r4, =TILES
	add    r5, r4, #LETTER_TILES * SPLASH_LETTERS
	bl     clearWords
	mov    r6, #BUFFER
	mov    r8, #0
	ldr    r7, =splashLetters
1:
	ldrb   r10, [r7]
	ldrb   r12, [r7, #1]
	ldr    r3, =TILES
	add    r3, r3, r8, lsl #11
	mov    r4, #0
2:
	mov    r5, r10
3:
	bl     coverage
	cmp    r0, #0
	beq    4f
	rsb    r1, r12, #64
	add    r1, r5, r1, lsr #1
	sub    r1, r1, r10
	add    r2, r4, #SPLASH_CELL_ROW
	mov    lr, r2, lsr #3
	mov    lr, lr, lsl #8
	and    r2, r2, #7
	add    lr, lr, r2, lsl #2
	mov    r2, r1, lsr #3
	add    lr, lr, r2, lsl #5
	and    r2, r1, #7
	add    lr, lr, r2, lsr #1
	tst    r1, #1
	movne  r0, r0, lsl #4
	ldrb   r2, [r3, lr]
	orr    r2, r2, r0
	strb   r2, [r3, lr]
4:
	add    r5, r5, #1
	add    r1, r10, r12
	cmp    r5, r1
	blo    3b
	add    r4, r4, #1
	cmp    r4, #SPLASH_TITLE_HEIGHT
	blo    2b
	add    r7, r7, #4
	add    r8, r8, #1
	cmp    r8, #SPLASH_LETTERS
	blo    1b
	mov    r10, #0x04000000
	ldr    r4, =TILES
	ldr    r5, =SPRITE_TILES
	add    r6, r5, #LETTER_TILES * SPLASH_LETTERS
0:
	ldmia  r4!, {r0-r3}
	stmia  r5!, {r0-r3}
	cmp    r5, r6
	blo    0b
	@ The spark after them, a 4-pointed star: levels 1 to 3 where (|2x - 31| + 1)(|2y - 31| + 1) is
	@ under 72, 48 and 28. A tile row a word, the right pixel first.
	mov    r6, #0
1:
	mov    r3, #0
	mov    r7, #7
2:
	mov    r0, r6, lsr #3
	and    r1, r0, #3
	add    r1, r7, r1, lsl #3
	mov    r0, r0, lsr #2
	and    r2, r6, #7
	add    r2, r2, r0, lsl #3
	mov    r1, r1, lsl #1
	subs   r1, r1, #31
	rsbmi  r1, r1, #0
	add    r1, r1, #1
	mov    r2, r2, lsl #1
	subs   r2, r2, #31
	rsbmi  r2, r2, #0
	add    r2, r2, #1
	mul    r0, r1, r2
	mov    r3, r3, lsl #4
	cmp    r0, #72
	addlo  r3, r3, #1
	cmp    r0, #48
	addlo  r3, r3, #1
	cmp    r0, #28
	addlo  r3, r3, #1
	subs   r7, r7, #1
	bpl    2b
	str    r3, [r5], #4
	add    r6, r6, #1
	cmp    r6, #128
	blo    1b
	@ "Open BIOS" drawn.
	mov    r6, #BUFFER
	mov    r4, #SPLASH_SUBTITLE_Y
	ldr    r7, =0x06000000 + (SPLASH_Y + SPLASH_SUBTITLE_Y) * 240 + SPLASH_X
1:
	mov    r5, #0
2:
	bl     splashIndex
	mov    r3, r0
	add    r5, r5, #1
	bl     splashIndex
	orr    r3, r3, r0, lsl #8
	add    r5, r5, #1
	add    r0, r7, r5
	strh   r3, [r0, #-2]
	cmp    r5, #SPLASH_WIDTH
	blo    2b
	add    r7, r7, #240
	add    r4, r4, #1
	cmp    r4, #SPLASH_HEIGHT
	blo    1b
	@ The buffers cleared now: a program sent over the cable goes where they were.
	mov    r0, #0
	mov    r1, #0
	mov    r2, #0
	mov    r3, #0
	mov    r4, #BUFFER
	ldr    r5, =TILES + LETTER_TILES * SPLASH_LETTERS
	bl     clearWords
	@ Mode 4 with sprites, their tiles in a row.
	ldr    r0, =0x1444
	strh   r0, [r10]
	@ Sound on: channels 1 and 2 on both sides.
	mov    r0, #0x80
	strh   r0, [r10, #0x84]
	ldr    r0, =0x3377
	strh   r0, [r10, #0x80]
	mov    r0, #2
	strh   r0, [r10, #0x82]
	@ The frames.
	mov    r4, #0
3:
	bl     vblankWait
	@ Each letter from STAGGER frames after the one before: its colour, how far it is from its
	@ place and its size, step by step, and its tick as it gets there.
	mov    r5, #0
	ldr    r6, =0x05000202
	mov    r7, #0x07000000
	ldr    r8, =splashLetters
4:
	add    r1, r5, r5, lsl #2
	add    r1, r1, #START
	sub    r0, r4, r1
	cmp    r0, #SPLASH_RISE
	bleq   tick
	@ Hidden until its turn.
	cmp    r0, #0
	movlt  r2, #0x0200
	strhlt r2, [r7]
	blt    8f
	cmp    r0, #SPLASH_SWEEP - 1
	movhi  r0, #SPLASH_SWEEP - 1
	ldr    r1, =splashSweep
	add    r1, r1, r0, lsl #1
	ldrh   r2, [r1]
	bl     levels
	@ Rising, an affine double-size sprite; there, at its size, a plain one 32 further in each
	@ way, which takes a quarter of the drawing time a line can give sprites.
	ldr    r1, =splashOffset
	ldrsb  r2, [r1, r0]
	add    r2, r2, #SPLASH_HOME_Y
	ldrh   r3, [r8, #2]
	cmp    r0, #SPLASH_RISE
	addhs  r2, r2, #32
	addhs  r3, r3, #32
	and    r2, r2, #0xFF
	orrlo  r2, r2, #0x0300
	strh   r2, [r7]
	bic    r3, r3, #0xFE00
	orrlo  r3, r3, r5, lsl #9
	orr    r3, r3, #0xC000
	strh   r3, [r7, #2]
	mov    r2, #512
	add    r2, r2, r5, lsl #6
	orr    r2, r2, r5, lsl #12
	strh   r2, [r7, #4]
	@ Its scale while it rises; settled, it's a plain sprite.
	ldrlo  r1, =splashScale
	addlo  r1, r1, r0, lsl #1
	ldrhlo r2, [r1]
	movlo  r3, #0x07000000
	addlo  r3, r3, r5, lsl #5
	strhlo r2, [r3, #6]
	strhlo r2, [r3, #30]
8:
	add    r6, r6, #32
	add    r7, r7, #8
	add    r8, r8, #4
	add    r5, r5, #1
	cmp    r5, #SPLASH_LETTERS
	blo    4b
	@ "Open BIOS" a band every 2 steps from the left, each band white to black over 3 of its steps.
	subs   r0, r4, #REVEAL
	blt    7f
	mov    r0, r0, lsr #1
	mov    r5, #0
	ldr    r6, =0x05000000 + FIRST_BAND * 2
	ldr    r7, =splashFade
6:
	subs   r1, r0, r5
	movlt  r1, #0
	cmp    r1, #3
	movhi  r1, #3
	add    r2, r7, r1, lsl #1
	ldrh   r2, [r2]
	bl     levels
	add    r6, r6, #6
	add    r5, r5, #1
	cmp    r5, #SPLASH_BANDS
	blo    6b
7:
	sub    r0, r4, #CHIME
	bl     chime
	@ The spark, peaking on the chime's last note: its size and colour frame by frame, then hidden.
	mov    r7, #0x07000000
	subs   r0, r4, #SPARK_START
	cmp    r0, #SPARK_FRAMES
	movhs  r2, #0x0200
	strhhs r2, [r7, #48]
	bhs    9f
	ldr    r2, =SPARK_Y | 0x0300
	strh   r2, [r7, #48]
	ldr    r2, =SPARK_X | 6 << 9 | 0x8000
	strh   r2, [r7, #50]
	ldr    r2, =SPARK_TILE | 6 << 12
	strh   r2, [r7, #52]
	ldr    r1, =sparkScale
	add    r1, r1, r0, lsl #1
	ldrh   r2, [r1]
	strh   r2, [r7, #6 * 32 + 6]
	strh   r2, [r7, #6 * 32 + 30]
	ldr    r1, =sparkColour
	add    r1, r1, r0, lsl #1
	ldr    r6, =0x05000200 + 6 * 32 + 2
	ldrh   r2, [r1]
	bl     levels
9:
	add    r4, r4, #1
	cmp    r4, #HOLD_END
	blo    3b
	@ Select and Start held as it's about to go: a program over the cable even with a cartridge.
	ldr    r0, =0x04000130
	ldrh   r0, [r0]
	tst    r0, #0x0C
	moveq  r11, #1
	cmp    r11, #0
	movne  r11, #1
	bx     r9

@ The fade to white, then everything the splash used as it was: the screen blanked, the blend, the
@ sound, the sprites, VRAM, the palettes, and r0-r10 and r12 0. r11 is kept. Like splash, it keeps its
@ return in a register, so the stack below sp is left as the reference leaves it.
.global splashEnd
splashEnd:
	mov    r12, lr
	mov    r10, #0x04000000
	mov    r0, #0x94
	strh   r0, [r10, #0x50]
	mov    r4, #1
0:
	bl     vblankWait
	strh   r4, [r10, #0x54]
	add    r4, r4, #1
	cmp    r4, #17
	blo    0b
	mov    r0, #0x80
	strh   r0, [r10]
	mov    r0, #0
	strh   r0, [r10, #0x50]
	strh   r0, [r10, #0x54]
	strh   r0, [r10, #0x62]
	strh   r0, [r10, #0x64]
	strh   r0, [r10, #0x68]
	strh   r0, [r10, #0x6C]
	strh   r0, [r10, #0x80]
	strh   r0, [r10, #0x82]
	strh   r0, [r10, #0x84]
	mov    r1, #0
	mov    r2, #0
	mov    r3, #0
	mov    r4, #0x06000000
	add    r5, r4, #240 * 160
	bl     clearWords
	ldr    r4, =SPRITE_TILES
	ldr    r5, =SPARK_TILES + 512
	bl     clearWords
	mov    r4, #0x05000000
	add    r5, r4, #0x400
	bl     clearWords
	mov    r4, #0x07000000
	add    r5, r4, #0x400
	bl     clearWords
	mov    r4, #0
	mov    r5, #0
	mov    r6, #0
	mov    r7, #0
	mov    r8, #0
	mov    r9, #0
	mov    r10, #0
	mov    lr, r12
	mov    r12, #0
	bx     lr

@ Zeroes from r4 to r5 with r0-r3, which are 0, 16 bytes at a time.
clearWords:
	stmia  r4!, {r0-r3}
	cmp    r4, r5
	blo    clearWords
	bx     lr

@ The coverage of pixel (r5, r4) of the logo, unpacked at r6, to r0: 0 to 3. r1 and r2 change.
coverage:
	mov    r1, #ROW_BYTES
	mul    r2, r4, r1
	add    r2, r2, r5, lsr #2
	ldrb   r0, [r6, r2]
	and    r1, r5, #3
	mov    r1, r1, lsl #1
	mov    r0, r0, lsr r1
	and    r0, r0, #3
	bx     lr

@ The palette index for pixel (r5, r4) of "Open BIOS", to r0: 0 for none, else its band's entry
@ for its coverage. r1, r2 and r12 change.
splashIndex:
	mov    r12, lr
	bl     coverage
	cmp    r0, #0
	bxeq   r12
	sub    r1, r5, #SPLASH_SUBTITLE_X
	mov    r1, r1, lsr #2
	add    r1, r1, r1, lsl #1
	add    r0, r0, r1
	add    r0, r0, #FIRST_BAND - 1
	bx     r12

@ r0: frames since the chime started. C6, G6 and C7 on channel 1, 5 frames apart, dying away: the
@ first two from full volume, the last from two thirds and more slowly.
chime:
	cmp    r0, #0
	bne    0f
	ldr    r1, =0xF280
	strh   r1, [r10, #0x62]
	ldr    r1, =0x8000 | 1923
	strh   r1, [r10, #0x64]
	bx     lr
0:
	cmp    r0, #5
	ldreq  r1, =0x8000 | 1964
	strheq r1, [r10, #0x64]
	cmp    r0, #10
	ldreq  r1, =0xA380
	strheq r1, [r10, #0x62]
	ldreq  r1, =0x8000 | 1985
	strheq r1, [r10, #0x64]
	bx     lr

@ Letter r5's tick, a step up the scale from the one before: C5, D5, E5, G5, A5, C6. r0 is kept.
tick:
	ldr    r1, =TICK_ENVELOPE
	strh   r1, [r10, #0x68]
	ldr    r1, =tickNotes
	add    r1, r1, r5, lsl #1
	ldrh   r1, [r1]
	strh   r1, [r10, #0x6C]
	bx     lr
tickNotes:
	.hword 0x8000 | 1798, 0x8000 | 1825, 0x8000 | 1849, 0x8000 | 1881, 0x8000 | 1899, 0x8000 | 1923

@ Colour r2 at the 3 coverage levels, to r6, r6 + 2 and r6 + 4: a third and two thirds of the way
@ from white, then all of it. With t a channel's (31 - c) / 3 rounded, they are 31 - t, c + t and
@ c, which need no carry between channels. r1, r3 and r12 change.
levels:
	adr    r3, thirds
	and    r1, r2, #31
	ldrb   r12, [r3, r1]
	mov    r1, r2, lsr #5
	and    r1, r1, #31
	ldrb   r1, [r3, r1]
	orr    r12, r12, r1, lsl #5
	mov    r1, r2, lsr #10
	and    r1, r1, #31
	ldrb   r1, [r3, r1]
	orr    r12, r12, r1, lsl #10
	mvn    r1, r12
	strh   r1, [r6]
	add    r1, r2, r12
	strh   r1, [r6, #2]
	strh   r2, [r6, #4]
	bx     lr
@ (31 - c) / 3 rounded, for c from 0 to 31.
thirds:
	.byte  10, 10, 10, 9, 9, 9, 8, 8, 8, 7, 7, 7, 6, 6, 6, 5, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 1, 1, 1, 0, 0

@ Waits for the start of the next VBlank. r10 is the I/O.
vblankWait:
	ldrh   r0, [r10, #6]
	cmp    r0, #160
	beq    vblankWait
0:
	ldrh   r0, [r10, #6]
	cmp    r0, #160
	bne    0b
	bx     lr

@ GBA LZ77, from r0 (its header first) to r1. r0-r6 and r12 change.
unLz77:
	ldr    r2, [r0], #4
	add    r2, r1, r2, lsr #8
1:
	ldrb   r3, [r0], #1
	mov    r12, #8
2:
	cmp    r1, r2
	bxhs   lr
	tst    r3, #0x80
	mov    r3, r3, lsl #1
	bne    3f
	ldrb   r4, [r0], #1
	strb   r4, [r1], #1
	b      4f
3:
	ldrb   r4, [r0], #1
	ldrb   r5, [r0], #1
	orr    r5, r5, r4, lsl #8
	mov    r4, r4, lsr #4
	add    r4, r4, #3
	bic    r5, r5, #0xF000
	add    r5, r5, #1
	sub    r5, r1, r5
5:
	ldrb   r6, [r5], #1
	strb   r6, [r1], #1
	subs   r4, r4, #1
	bne    5b
4:
	subs   r12, r12, #1
	bne    2b
	b      1b
.ltorg
