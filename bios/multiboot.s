@ Copyright (c) 2026 Magnominus
@
@ This Source Code Form is subject to the terms of the Mozilla Public
@ License, v. 2.0. If a copy of the MPL was not distributed with this
@ file, You can obtain one at http://mozilla.org/MPL/2.0/.
@
@ MultiBoot: a program sent over the link cable to consoles with no cartridge, as GBATEK describes
@ the transfer, checked against what a reference BIOS sends and answers.

.syntax unified
.arm

.include "macros.inc"

@ From r11, at 0x04000100.
.equ SIOMULTI1, 0x22
.equ SIOCNT, 0x28
.equ SIOMLT_SEND, 0x2A
.equ JOYCNT, 0x140
@ The receiver's waits in normal mode, per word of its checksum and per transfer meanwhile.
.equ MB_NORMAL_WORD, 100
.equ MB_NORMAL_TRANSFER, 58

@ The parameters' fields.
.equ MB_HANDSHAKE, 0x14
.equ MB_CLIENT_DATA, 0x19
.equ MB_PALETTE, 0x1C
.equ MB_CLIENT_BIT, 0x1E
.equ MB_SOURCE, 0x20
.equ MB_END, 0x24

.text

@ SWI 0x25. r0: the parameters, after the game's handshake with the consoles, r1: the mode (1 for
@ multiplayer, 0 or 2 for normal mode at 256 KHz or 2 MHz). After 1/16 s the length goes out, and
@ each console answers with a random byte; then the program after its header, each word encrypted
@ with a key run from the palette and the client data, then a checksum the consoles must agree on,
@ which the handshake and their random bytes finish. Normal mode sends the same in 32 bits, a word
@ of the program at a time, with its own checksum and key constants. r0 is left 0, or 1 if a
@ console's checksum differs.
.global MultiBoot
MultiBoot:
	stmfd  sp!, {r4-r11, lr}
	mov    r4, r0
	ldr    r11, =0x04000100
	@ r10: SIOCNT for each transfer.
	cmp    r1, #1
	ldreq  r10, =0x2083
	ldrne  r10, =0x1089
	cmp    r1, #2
	orreq  r10, r10, #2
	@ 1/16 s.
	ldr    r0, =0x40000 + 201
0:
	subs   r0, r0, #1
	bne    0b
	@ The length in words, less 0x34; the answers give each console's random byte, or 0xFF for
	@ one that isn't there. The checksum's finish: the handshake, then those bytes.
	ldr    r5, [r4, #MB_SOURCE]
	ldr    r6, [r4, #MB_END]
	sub    r0, r6, r5
	mov    r0, r0, lsr #2
	sub    r0, r0, #0x34
	bl     multiSend
	ldrb   r7, [r4, #MB_HANDSHAKE]
	ldrb   r8, [r4, #MB_CLIENT_BIT]
	mov    r9, #8
	mov    r12, #2
	add    r3, r11, #SIOMULTI1
1:
	ldrh   r0, [r3], #2
	tst    r8, r12
	moveq  r0, #0xFF
	and    r0, r0, #0xFF
	orr    r7, r7, r0, lsl r9
	add    r9, r9, #8
	mov    r12, r12, lsl #1
	cmp    r9, #32
	blo    1b
	str    r7, [sp, #-4]!
	@ The key: the palette, then each console's client data.
	ldrb   r7, [r4, #MB_PALETTE]
	ldrb   r0, [r4, #MB_CLIENT_DATA]
	orr    r7, r7, r0, lsl #8
	ldrb   r0, [r4, #MB_CLIENT_DATA + 1]
	orr    r7, r7, r0, lsl #16
	ldrb   r0, [r4, #MB_CLIENT_DATA + 2]
	orr    r7, r7, r0, lsl #24
	@ r5 the next word, r6 the end, r7 the key, r8 the checksum, r9 its polynomial, r12 the
	@ word's position from the image's top as 0xFE000000 less it. Each word is worked out while
	@ the one before it goes, as the reference does.
	tst    r10, #0x2000
	ldrne  r8, =0xFFF8
	ldrne  r9, =0xA517
	ldreq  r8, =0xC387
	ldreq  r9, =0xC37B
	ldr    r12, =0xFE000000 - 0xC0
	bl     nextWord
	mov    r0, #60
0:
	subs   r0, r0, #1
	bne    0b
	cmp    r2, #0
	beq    4f
2:
	mov    r0, r1
	tst    r10, #0x2000
	beq    3f
	mov    r0, r1, lsl #16
	mov    r0, r0, lsr #16
	bl     multiStart
	bl     multiWait
	mov    r0, r1, lsr #16
3:
	bl     multiStart
	bl     nextWord
	bl     multiWait
	cmp    r2, #0
	bne    2b
4:
	@ The end, sent until every console is ready for the checksum.
	mov    r0, #0x65
	bl     multiStart
	bl     multiWaitEnd
	mov    r0, #5
0:
	subs   r0, r0, #1
	bne    0b
	b      5f
4:
	mov    r0, #0x65
	bl     multiSend
5:
	bl     allAnswered
	bne    4b
	mov    r0, #0x66
	bl     multiSend
	@ As long as the reference takes to work out the checksum.
	mov    r0, #65
0:
	subs   r0, r0, #1
	bne    0b
	ldr    r0, [sp], #4
	eor    r8, r8, r0
	mov    r2, #32
0:
	movs   r8, r8, lsr #1
	eorcs  r8, r8, r9
	subs   r2, r2, #1
	bne    0b
	mov    r0, r8, lsl #16
	mov    r0, r0, lsr #16
	bl     multiStart
0:
	ldrh   r0, [r11, #SIOCNT]
	tst    r0, #0x80
	bne    0b
	@ Each console there answers with its own checksum, looked at as late as on the reference.
	mov    r0, #16
1:
	subs   r0, r0, #1
	bne    1b
	ldrb   r1, [r4, #MB_CLIENT_BIT]
	mov    r2, #2
	add    r3, r11, #SIOMULTI1
	mov    r0, #0
6:
	tst    r1, r2
	ldrh   r12, [r3], #2
	beq    7f
	eor    r12, r12, r8
	mov    r12, r12, lsl #16
	cmp    r12, #0
	movne  r0, #1
7:
	mov    r2, r2, lsl #1
	cmp    r2, #16
	blo    6b
	ands   r1, r10, #0x2000
	movne  r1, #1
	andeq  r1, r10, #2
	mov    r2, #0
	mov    r3, #0
	ldmfd  sp!, {r4-r11, lr}
	b      swiReturn

@ Sends r0 as the parent and waits for the transfer to end, and as long after it as the reference
@ waits. As on the reference, r0 goes to SIODATA32 and its lower half to SIOMLT_SEND, whichever the
@ mode. r10 is SIOCNT to start with, r11 the I/O; only r0 changes.
multiSend:
	stmfd  sp!, {lr}
	bl     multiStart
	bl     multiWaitEnd
	ldmfd  sp!, {pc}
multiStart:
	str    r0, [r11, #0x20]
	strh   r0, [r11, #SIOMLT_SEND]
	strh   r10, [r11, #SIOCNT]
	bx     lr
multiWait:
	ldrh   r0, [r11, #SIOCNT]
	tst    r0, #0x80
	bne    multiWait
	mov    r0, #164
0:
	subs   r0, r0, #1
	bne    0b
	nop
	bx     lr
@ The same, with what's done between transfers after the program's taking less time.
multiWaitEnd:
	ldrh   r0, [r11, #SIOCNT]
	tst    r0, #0x80
	bne    multiWaitEnd
	mov    r0, #145
0:
	subs   r0, r0, #1
	bne    0b
	bx     lr

@ The next word of the program, encrypted, to r1, with r2 1; or r2 0 at the end. r5 the next word,
@ r6 the end, r7 the key, r8 the checksum, r9 its polynomial, r12 the position.
nextWord:
	cmp    r5, r6
	movhs  r2, #0
	bxhs   lr
	ldr    r1, [r5], #4
	eor    r8, r8, r1
	mov    r2, #32
0:
	movs   r8, r8, lsr #1
	eorcs  r8, r8, r9
	subs   r2, r2, #1
	bne    0b
	ldr    r2, =0x6F646573
	mul    r7, r2, r7
	add    r7, r7, #1
	eor    r1, r1, r12
	eor    r1, r1, r7
	tst    r10, #0x2000
	ldrne  r2, =0x6465646F
	ldreq  r2, =0x43202F2F
	eor    r1, r1, r2
	sub    r12, r12, #4
	mov    r2, #1
	bx     lr

@ Whether every console there answered 0x75: Z set if so. r4 the parameters, r11 the I/O.
allAnswered:
	ldrb   r1, [r4, #MB_CLIENT_BIT]
	mov    r2, #2
	add    r3, r11, #SIOMULTI1
	mov    r12, #0
0:
	ldrh   r0, [r3], #2
	tst    r1, r2
	eorne  r0, r0, #0x75
	orrne  r12, r12, r0
	mov    r2, r2, lsl #1
	cmp    r2, #16
	blo    0b
	cmp    r12, #0
	bx     lr
.ltorg

@ The BIOS's side of a transfer, with no cartridge: it listens in turn, as the reference does, in
@ multiplayer mode for 31 frames, normal mode for 7 and JOY bus mode for 7, until a parent's 0x6200
@ comes. Then, as a child in that mode, it answers the game's handshake, takes the header to
@ 0x02000000 and the program after it, decrypting each word, then swaps checksums, and starts
@ again if they differ. Returns once a program has come. r9 is set in multiplayer mode and clear in
@ normal mode, r11 the I/O. Normal mode carries the same values as multiplayer mode in 32 bits: the
@ answers go in the upper half, the parent's last word staying in the lower, and the program comes
@ a word at a time.
.global multibootReceive
multibootReceive:
	stmfd  sp!, {lr}
	ldr    r11, =0x04000100
	@ In JOY bus mode a GameCube's commands raise the serial interrupt, their flags cleared first.
	mov    r0, #0x47
	strh   r0, [r11, #JOYCNT - 0x100]
1:
	adr    r4, listenModes
2:
	ldrh   r0, [r4, #6]
	strh   r0, [r11, #0x34]
	ldrh   r0, [r4]
	cmp    r0, #0
	beq    3f
	strh   r0, [r11, #SIOCNT]
	mov    r0, #0
	strh   r0, [r11, #SIOMLT_SEND]
	str    r0, [r11, #0x20]
	mov    r5, #1
	bl     listen
	ldrh   r0, [r4, #2]
	strh   r0, [r11, #SIOCNT]
3:
	ldrh   r5, [r4, #4]
	bl     listen
	add    r4, r4, #8
	adr    r0, listenEnd
	cmp    r4, r0
	bne    2b
	b      1b
	@ Heard: 0x6200, answered 0x72 and this console's bit, until 0x61xx starts the header.
heard:
	ldr    r1, =0x6200
	cmp    r0, r1
	orreq  r1, r10, #0x7200
	bleq   reply
	beq    5f
	ldrh   r1, [r11, #SIOMLT_SEND]
	cmp    r1, #0
	bleq   reply
	beq    5f
	and    r1, r0, #0xFF00
	cmp    r1, #0x6100
	bne    1b
	@ The header, 0x60 halfwords, each answered with how many are left.
	mov    r5, #0x02000000
	mov    r6, #0x60
6:
	orr    r1, r10, r6, lsl #8
	bl     reply
	bl     receive
	strh   r0, [r5], #2
	subs   r6, r6, #1
	bne    6b
	orr    r1, r10, r6, lsl #8
	bl     reply
	@ 0x62xx, answered 0x72 and the bit, until the palette, 0x63pp, answered with the client data.
7:
	bl     receive
	and    r1, r0, #0xFF00
	cmp    r1, #0x6200
	orreq  r1, r10, #0x7200
	bleq   reply
	beq    7b
	cmp    r1, #0x6300
	bne    1b
	and    r7, r0, #0xFF
	ldr    r1, =0x7387
	cmp    r9, #0
	addeq  r1, r1, #1
	bl     reply
	@ The palette again until the handshake, 0x64hh: the key from the palette and the consoles'
	@ client data, and this console's random byte.
8:
	bl     receive
	and    r1, r0, #0xFF00
	cmp    r1, #0x6300
	beq    8b
	cmp    r1, #0x6400
	bne    1b
	and    r8, r0, #0xFF
	mov    r0, r7
	mov    r1, #0x88
	bl     consoleBytes
	mov    r7, r0
	ldr    r1, =0x73EE
	cmp    r9, #0
	ldreq  r1, =0x734F
	bl     reply
	@ The length, and the checksum's finish from the handshake and the consoles' random bytes.
	bl     receive
	add    r0, r0, #0x34
	add    r6, r5, r0, lsl #2
	mov    r0, r8
	mov    r1, #0x4F
	bl     consoleBytes
	stmfd  sp!, {r0}
	@ The program: each word answered with where it goes, as two halves in multiplayer mode, then
	@ decrypted.
	ldr    r12, =0xFE000000 - 0xC0
9:
	sub    r3, r5, #0x02000000
	mov    r1, r3
	bl     reply
	bl     receive
	ldr    r4, [r11, #0x20]
	cmp    r9, #0
	beq    0f
	mov    r4, r0
	add    r1, r3, #2
	bl     reply
	bl     receive
	orr    r4, r4, r0, lsl #16
0:
	ldr    r1, =0x6F646573
	mul    r2, r7, r1
	add    r7, r2, #1
	eor    r4, r4, r12
	eor    r4, r4, r7
	cmp    r9, #0
	ldrne  r1, =0x6465646F
	ldreq  r1, =0x43202F2F
	eor    r4, r4, r1
	str    r4, [r5], #4
	sub    r12, r12, #4
	cmp    r5, r6
	blo    9b
	@ The end: the offset reached, then the checksum over the program, answered 0x74 meanwhile.
	sub    r1, r5, #0x02000000
	bl     reply
	bl     receive
	mov    r1, #0x74
	bl     reply
	bl     rearm
	cmp    r9, #0
	ldrne  r8, =0xFFF8
	ldrne  r10, =0xA517
	ldreq  r8, =0xC387
	ldreq  r10, =0xC37B
	ldr    r5, =0x020000C0
	ldmfd  sp!, {r4}
0:
	cmp    r5, r6
	ldrlo  r0, [r5], #4
	movhs  r0, r4
	addhs  r5, r5, #4
	eor    r8, r8, r0
	mov    r2, #32
3:
	movs   r8, r8, lsr #1
	eorcs  r8, r8, r10
	wait   2
	subs   r2, r2, #1
	bne    3b
	@ In normal mode the transfers meanwhile are each answered again, and each word takes as long
	@ as on the reference.
	cmp    r9, #0
	moveq  r3, #MB_NORMAL_WORD
	movne  r3, #0
3:
	bl     service
	subs   r3, r3, #1
	bpl    3b
	cmp    r5, r6
	bls    0b
	mov    r1, #0x75
	bl     reply
0:
	bl     receive
	cmp    r0, #0x65
	beq    0b
	cmp    r0, #0x66
	bne    1b
	mov    r1, r8
	bl     reply
	bl     receive
	mov    r1, r8, lsl #16
	cmp    r0, r1, lsr #16
	bne    1b
	@ Done: 0xFF on the cable, and in the header the boot mode, 3 for multiplayer or 2 for normal,
	@ and this console's number.
	mov    r1, #0xFF
	bl     reply
	mov    r2, #0x02000000
	cmp    r9, #0
	movne  r1, #3
	moveq  r1, #2
	strb   r1, [r2, #0xC4]
	mov    r1, r10, lsr #1
	cmp    r1, #4
	moveq  r1, #3
	strb   r1, [r2, #0xC5]
	ldmfd  sp!, {pc}
5:
	bl     receive
	b      heard

@ SIOCNT as each listening mode is set up and a frame later (none for JOY bus), the frames it lasts
@ after that, and RCNT.
listenModes:
	.hword 0x2003, 0x6003, 30, 0x0000
	.hword 0x1008, 0x5088, 6, 0x0000
	.hword 0x0000, 0x0000, 7, 0xC000
listenEnd:

@ Waits r5 frames, to the start of line 160, unless a transfer ends first: then it takes the
@ parent's halfword to r0 and goes on at heard.
listen:
	mov    r12, #0x04000000
0:
	ldrh   r0, [r12, #6]
	cmp    r0, #160
	beq    0b
listening:
	add    r0, r12, #0x200
	ldrh   r0, [r0, #2]
	tst    r0, #0x80
	bne    2f
	ldrh   r0, [r12, #6]
	cmp    r0, #160
	bne    listening
	subs   r5, r5, #1
	bne    0b
	bx     lr
2:
	ldrh   r9, [r11, #0x34]
	tst    r9, #0x4000
	ldrne  r0, =joyHeard + 1
	bxne   r0
	ldrh   r9, [r11, #SIOCNT]
	and    r9, r9, #0x2000
	ldr    lr, =heard
	b      took

@ JOY bus mode: a GameCube's command has come. A reset starts its MultiBoot, as Nintendo's GameCube
@ SDK sends it: the session key goes in JOY_TRANS for it to read, then comes its key word, the
@ header plain and the rest encrypted, each word answered by flipping JOYSTAT's general flag 0x10,
@ then the length and checksum, which JOY_TRANS answers with 0. Until a reset it waits in JOY bus
@ mode. A wrong checksum isn't taken for the end, and the GameCube starts over with a reset. r4 is
@ the checksum, r5 where the next word goes, r6 the key, r7 JOYCNT and r3 IF.
.thumb
joyHeard:
	ldr    r7, =0x04000000 + JOYCNT
	ldr    r3, =0x04000202
	@ No session: the key in JOY_TRANS and JOYSTAT's flags clear, until a reset.
joyIdle:
	movs   r0, #0
	bl     joyKey
0:
	bl     joyWrite
	b      0b
joyReset:
	movs   r0, #0x10
	bl     joyKey
	@ The key word, which holds the length again.
	bl     joyWrite
	movs   r0, #0x20
	strh   r0, [r7, #0x18]
	movs   r5, #2
	lsls   r5, r5, #24
	movs   r4, #0xAD
	lsls   r4, r4, #5
0:
	bl     joyWrite
	ldrh   r1, [r7, #0x18]
	movs   r2, #0x10
	eors   r1, r2
	strh   r1, [r7, #0x18]
	lsls   r1, r5, #8
	lsrs   r1, r1, #8
	cmp    r1, #0xC0
	blo    2f
	ldr    r2, =0x6177614B
	muls   r6, r2, r6
	adds   r6, r6, #1
	eors   r0, r6
	ldr    r2, =0x20796220
	eors   r0, r2
	negs   r2, r5
	eors   r0, r2
	@ The end: the length above the checksum so far.
	lsls   r2, r1, #16
	orrs   r2, r4
	cmp    r0, r2
	beq    3f
	stmia  r5!, {r0}
	movs   r2, #32
	ldr    r3, =0xA1C1
4:
	movs   r1, r4
	eors   r1, r0
	lsrs   r4, r4, #1
	lsrs   r1, r1, #1
	bcc    5f
	eors   r4, r3
5:
	lsrs   r0, r0, #1
	subs   r2, r2, #1
	bne    4b
	ldr    r3, =0x04000202
	b      0b
2:
	stmia  r5!, {r0}
	b      0b
	@ Done: JOY_TRANS 0, JOYSTAT's flags cleared, and in the header boot mode 1.
3:
	movs   r0, #0
	str    r0, [r7, #0x14]
	strh   r0, [r7, #0x18]
	movs   r1, #1
	movs   r2, #2
	lsls   r2, r2, #24
	adds   r2, r2, #0xC4
	strb   r1, [r2]
	pop    {r0}
	bx     r0

@ The session key in JOY_TRANS, one the reference has given, and JOYSTAT's flags r0; r6 is left
@ the key the encryption starts from.
joyKey:
	ldr    r6, =0xB2A4CFC3
	str    r6, [r7, #0x14]
	strh   r0, [r7, #0x18]
	ldr    r0, =0x6F646573
	eors   r6, r0
	bx     lr

@ Waits for the GameCube's next write and takes its word to r0. A reset starts over, and with
@ nothing for about 12 frames the session is dropped, as on the reference.
joyWrite:
	movs   r0, #3
	lsls   r0, r0, #17
0:
	subs   r0, r0, #1
	beq    joyIdle
	ldrh   r1, [r3]
	lsls   r1, r1, #25
	bcc    0b
	movs   r1, #0x80
	strh   r1, [r3]
	ldrh   r1, [r7]
	strh   r1, [r7]
	lsrs   r2, r1, #1
	bcs    joyReset
	lsrs   r2, r1, #2
	bcc    joyWrite
	ldr    r0, [r7, #0x10]
	bx     lr
.ltorg
.arm

@ Answers with r1 in the next transfer: written to both SIOMLT_SEND and SIODATA32's upper half, as
@ the reference does.
reply:
	strh   r1, [r11, #SIOMLT_SEND]
	strh   r1, [r11, #0x22]
	bx     lr

@ r0's low byte with each console's low byte from its last answer above it, or 0xFF for one that
@ isn't there; in normal mode the one console is this one, whose byte is r1.
consoleBytes:
	cmp    r9, #0
	orreq  r0, r0, r1, lsl #8
	orreq  r0, r0, #0xFF000000
	orreq  r0, r0, #0x00FF0000
	bxeq   lr
	mov    r2, #8
	add    r3, r11, #SIOMULTI1
0:
	ldrb   r1, [r3], #2
	orr    r0, r0, r1, lsl r2
	add    r2, r2, #8
	cmp    r2, #32
	blo    0b
	bx     lr

@ In normal mode, a transfer that has ended is answered again with SIOMLT_SEND's answer, after as
@ long as the reference takes over one.
service:
	cmp    r9, #0
	bxne   lr
	ldr    r0, =0x04000202
	ldrh   r1, [r0]
	tst    r1, #0x80
	bxeq   lr
	mov    r1, #0x80
	strh   r1, [r0]
	mov    r1, #MB_NORMAL_TRANSFER
0:
	subs   r1, r1, #1
	bne    0b
@ In normal mode the answer goes in again and the transfer is started again on the parent's clock.
rearm:
	cmp    r9, #0
	bxne   lr
	ldrh   r0, [r11, #SIOMLT_SEND]
	strh   r0, [r11, #0x22]
	ldr    r0, =0x5088
	strh   r0, [r11, #SIOCNT]
	bx     lr

@ Waits for a transfer to end and takes the parent's halfword to r0; r10 is left this console's bit
@ (always 2 in normal mode). In normal mode, where each transfer leaves the parent's word in place of
@ the answer, the answer kept in SIOMLT_SEND goes back in and the transfer is started again on the
@ parent's clock.
receive:
	cmp    r9, #0
	ldrheq r0, [r11, #SIOMLT_SEND]
	strheq r0, [r11, #0x22]
	ldreq  r0, =0x5088
	strheq r0, [r11, #SIOCNT]
	ldr    r0, =0x04000202
0:
	ldrh   r1, [r0]
	tst    r1, #0x80
	beq    0b
took:
	ldr    r0, =0x04000202
	mov    r1, #0x80
	strh   r1, [r0]
	ldrh   r0, [r11, #SIOCNT]
	mov    r0, r0, lsr #4
	and    r0, r0, #3
	mov    r10, #1
	mov    r10, r10, lsl r0
	cmp    r9, #0
	moveq  r10, #2
	ldrh   r0, [r11, #0x20]
	bx     lr
.ltorg
