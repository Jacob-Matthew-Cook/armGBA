@ Copyright (c) 2026 Jacob Cook
@
@ This Source Code Form is subject to the terms of the Mozilla Public
@ License, v. 2.0. If a copy of the MPL was not distributed with this
@ file, You can obtain one at http://mozilla.org/MPL/2.0/.
@
@ The sound driver, calls 0x1A to 0x1E, 0x28 and 0x29: a mixer the game gives a work area, whose
@ samples two DMA channels play through Direct Sound A (right) and B (left). The area's layout,
@ settings and timing follow GBATEK and what tests/bioscheck measures of a reference BIOS.

.syntax unified
.arm
.include "macros.inc"

@ The work area. Its address is kept at 0x03007FF0.
.equ AREA_POINTER, 0x03007FF0
.equ IDENT, 0x68736D53                  @ "Smsh", once it's set up
.equ DMA_COUNT, 0x04                    @ frames until the DMA starts again from the buffer's top
.equ REVERB, 0x05
.equ CHANNELS, 0x06
.equ VOLUME, 0x07
.equ FREQUENCY, 0x08                    @ 1 to 12, as SoundDriverMode takes it
.equ FRAMES, 0x0B                       @ frames the buffer holds
.equ SAMPLES, 0x10                      @ samples a frame
.equ RATE, 0x14                         @ samples a second
.equ DIVISOR, 0x18                      @ 2^23 / RATE
.equ MPLAY_MAIN, 0x20                   @ the music player's callback, called with MPLAY_ARG
.equ MPLAY_ARG, 0x24
.equ CALLBACKS, 0x28                    @ six function pointers
.equ CGB_SOUND, 0x28
.equ CHANNEL, 0x50                      @ 12 channels
.equ CHANNEL_SIZE, 0x40
.equ PCM, 0x350                         @ the right samples, then the left
.equ PCM_SIZE, 0x630
.equ AREA_SIZE, PCM + 2 * PCM_SIZE

.text
.global SoundDriverInit, SoundDriverMode, SoundDriverMain, SoundDriverVSync, SoundChannelClear
.global SoundDriverVSyncOff, SoundDriverVSyncOn, setFrequency, soundNothing

@ r0: the work area, cleared (by CpuSet, from a 0 on the stack) and set up for 8 channels at full
@ volume and 13379 Hz. The DMA stops; sound on: Direct Sound A on the right and B on the left at
@ full volume, the FIFOs emptied, and 8-bit output; DMA 1 from the right samples to FIFO A, DMA 2
@ from the left to FIFO B. The DMA and timer 0 start as line 159 begins. r0 is left the area's mark,
@ r1 159, and r3 the way back, which it returns through.
SoundDriverInit:
	stmfd  sp!, {r3, r7, lr}
	mov    r7, r0
	ldr    r1, =0x040000C0
	mov    r0, #0
	strh   r0, [r1, #6]
	strh   r0, [r1, #0x12]
	ldr    r0, =0x04000084
	mov    r2, #0x8F
	strh   r2, [r0]
	ldr    r2, =0xA90E
	strh   r2, [r0, #-2]
	ldrb   r2, [r0, #5]
	mov    r2, r2, lsl #26
	mov    r2, r2, lsr #26
	mov    r3, #0x40
	orr    r2, r2, r3
	mov    r3, #PCM >> 4
	mov    r3, r3, lsl #4
	strb   r2, [r0, #5]
	add    r2, r7, r3
	str    r2, [r1, #-4]
	ldr    r0, =0x040000A0
	mov    r3, #(PCM + PCM_SIZE) >> 7
	mov    r3, r3, lsl #7
	str    r0, [r1]
	add    r0, r7, r3
	str    r0, [r1, #8]
	ldr    r0, =0x040000A4
	ldr    r2, =0x05000000 | (AREA_SIZE / 4)
	str    r0, [r1, #0xC]
	ldr    r0, =AREA_POINTER
	str    r7, [r0]
	mov    r0, #0
	str    r0, [sp]
	mov    r0, sp
	mov    r1, r7
	thumbBl CpuSet
	mov    r0, #8
	strb   r0, [r7, #CHANNELS]
	mov    r0, #15
	strb   r0, [r7, #VOLUME]
	@ The callbacks do nothing, but for the music player's list and note on.
	ldr    r0, =jumpList
	str    r0, [r7, #CALLBACKS + 12]
	ldr    r0, =soundNothing
	str    r0, [r7, #CALLBACKS]
	str    r0, [r7, #CALLBACKS + 4]
	str    r0, [r7, #CALLBACKS + 8]
	str    r0, [r7, #CALLBACKS + 20]
	ldr    r0, =plyNote
	str    r0, [r7, #CALLBACKS + 16]
	mov    r0, #4
	mov    r0, r0, lsl #16
	thumbBl startFrequency
	ldr    r0, =IDENT
	str    r0, [r7]
	ldmfd  sp!, {r3, r7}
	ldmfd  sp!, {r3}
	bx     r3

@ r0: the frequency in bits 16-19, r7: the work area. As the reference does: its settings are worked
@ out from the samples a frame, by DivArm through a veneer; timer 0 stops and takes its new period,
@ the DMA is switched on, and timer 0 starts as line 159 begins. r1 is left 159, r4 kept.
startFrequency:
	stmfd  sp!, {r4, r5, lr}
	ldr    r1, =AREA_POINTER - 0x30
	mov    r3, #0xF
	mov    r3, r3, lsl #16
	and    r0, r0, r3
	@ As long as the reference takes.
	ldr    lr, [r1, #0x30]
	mov    r0, r0, lsr #16
	strb   r0, [r7, #FREQUENCY]
	ldr    r1, =samplesPerFrame
	mov    r0, r0, lsl #1
	@ As long as the reference takes.
	nop
	nop
	ldrh   r0, [r1, r0]
	mov    r1, #PCM_SIZE >> 4
	mov    r1, r1, lsl #4
	mov    r4, r0
	str    r4, [r7, #SAMPLES]
	thumbBl divArmVeneer
	strb   r0, [r7, #FRAMES]
	@ The rate rounded: 597275 / 10000 a sample. The multiply takes the time it does from 597275.
	ldr    r0, =597275
	ldr    r3, =5000
	mul    r0, r4, r0
	add    r1, r0, r3
	mov    r0, r3, lsl #1
	thumbBl divArmVeneer
	mov    r1, #1
	mov    r1, r1, lsl #24
	str    r0, [r7, #RATE]
	thumbBl divArmVeneer
	add    r0, r0, #1
	mov    r0, r0, lsr #1
	str    r0, [r7, #DIVISOR]
	@ Timer 0 stops and takes the period of a sample, from the cycles a frame.
	ldr    r4, =0x04000100
	mov    r0, #0
	strh   r0, [r4, #2]
	ldr    r0, [r7, #SAMPLES]
	ldr    r1, =280896
	thumbBl divArmVeneer
	mov    r1, #1
	mov    r1, r1, lsl #16
	sub    r0, r1, r0
	strh   r0, [r4]
	thumbBl SoundDriverVSyncOn
	mov    r0, #4
	mov    r0, r0, lsl #24
0:
	ldrb   r1, [r0, #6]
	cmp    r1, #159
	beq    0b
1:
	ldrb   r1, [r0, #6]
	cmp    r1, #159
	bne    1b
	mov    r0, #0x80
	strh   r0, [r4, #2]
	ldmfd  sp!, {r4, r5}
	ldmfd  sp!, {r3}
	bx     r3

@ DivArm, through the reference's veneer's time.
divArmVeneer:
	b      0f
0:
	b      DivArm

@ r0: settings, each field changed if it isn't 0: bits 0-7 the reverb (bit 7 dropped), 8-11 the
@ channels (all 12 are stopped), 12-15 the volume, 20-23 the output's bits (8 to 11 for 9 to 6
@ bits), 16-19 the frequency (1 to 12, from 5734 to 42048 Hz), which SoundDriverVSyncOff and
@ startFrequency set as SoundDriverInit does, leaving r0 0x80 and r1 159. The area is marked busy
@ meanwhile. r3 is left the way back, which it returns through.
SoundDriverMode:
	stmfd  sp!, {r4, r5, r7, lr}
	ldr    r1, =AREA_POINTER - 0x30
	ldr    r5, =IDENT
	ldr    r7, [r1, #0x30]
	ldr    r1, [r7]
	cmp    r1, r5
	bne    9f
	add    r1, r1, #1
	str    r1, [r7]
	mov    r1, r0, lsl #24
	movs   r1, r1, lsr #24
	beq    1f
	mov    r1, r1, lsl #25
	mov    r1, r1, lsr #25
	strb   r1, [r7, #REVERB]
1:
	mov    r1, #0xF
	mov    r1, r1, lsl #8
	ands   r1, r1, r0
	beq    2f
	mov    r1, r1, lsr #8
	strb   r1, [r7, #CHANNELS]
	mov    r1, #12
	mov    r3, #0
	add    r2, r7, #7
	add    r2, r2, #CHANNEL - 7
0:
	strb   r3, [r2]
	add    r2, r2, #CHANNEL_SIZE
	subs   r1, r1, #1
	bne    0b
2:
	mov    r1, #0xF
	mov    r1, r1, lsl #12
	ands   r1, r1, r0
	beq    3f
	mov    r1, r1, lsr #12
	strb   r1, [r7, #VOLUME]
3:
	mov    r1, #0xB
	mov    r1, r1, lsl #20
	ands   r1, r1, r0
	beq    4f
	mov    r3, #3
	mov    r3, r3, lsl #20
	ldr    r2, =0x04000080
	and    r1, r1, r3
	ldrb   r3, [r2, #9]
	mov    r1, r1, lsr #14
	mov    r3, r3, lsl #26
	mov    r3, r3, lsr #26
	orr    r1, r1, r3
	strb   r1, [r2, #9]
4:
	mov    r4, #0xF
	mov    r4, r4, lsl #16
	ands   r4, r4, r0
	beq    5f
	thumbBl SoundDriverVSyncOff
	mov    r0, r4
	thumbBl startFrequency
5:
	str    r5, [r7]
9:
	ldmfd  sp!, {r4, r5, r7}
	ldmfd  sp!, {r3}
	bx     r3

@ r0: the work area, r1: the frequency. Its settings are worked out from the samples a frame, and
@ timer 0 stops with its new period while the DMA is switched on; timer 0 starts as line 159 begins,
@ interrupts taken while it waits. r0 is kept, r1 left the CPSR with the interrupt bit set and r2 the
@ samples a frame.
setFrequency:
	stmfd  sp!, {r4-r7, lr}
	mov    r4, r0
	strb   r1, [r4, #FREQUENCY]
	adr    r12, samplesPerFrame
	mov    r1, r1, lsl #1
	ldrh   r5, [r12, r1]
	str    r5, [r4, #SAMPLES]
	@ Frames in the buffer, samples a second, and 2^23 / the rate rounded.
	ldr    r0, =PCM_SIZE
	mov    r1, r5
	bl     divide
	strb   r0, [r4, #FRAMES]
	ldr    r0, =597275
	mul    r0, r5, r0
	ldr    r1, =5000
	add    r0, r0, r1
	ldr    r1, =10000
	bl     divide
	str    r0, [r4, #RATE]
	mov    r1, r0
	mov    r0, #0x1000000
	bl     divide
	add    r0, r0, #1
	mov    r0, r0, asr #1
	str    r0, [r4, #DIVISOR]
	@ Timer 0 stops and takes the period of a sample, from the cycles a frame.
	ldr    r12, =0x04000100
	mov    r0, #0
	strh   r0, [r12, #2]
	ldr    r0, =280896
	ldr    r1, [r4, #SAMPLES]
	bl     divide
	rsb    r1, r0, #0
	@ r12 at the timers, before which are the DMA registers and VCOUNT.
	ldr    r12, =0x04000100
	strh   r1, [r12]
	ldr    r2, =0xB600
	strh   r2, [r12, #0xC6 - 0x100]
	strh   r2, [r12, #0xD2 - 0x100]
	mrs    r7, cpsr
	and    r7, r7, #0xFF
	orr    r7, r7, #0x80
	@ Timer 0 starts on the reference's cycle.
	ldr    r0, =0x04000006
	add    r1, r12, #2
	mov    r3, #0x80
	ldr    r2, =waitStart
	mov    lr, pc
	bx     r2
	mov    r1, r7
	mov    r2, r5
	mov    r0, r4
	ldmfd  sp!, {r4-r7, lr}
	bx     lr

@ Waits for line 159 to begin again, then starts timer 0: r0 VCOUNT's address, r1 TM0CNT_H's, r3
@ its value. Thumb code, polling as often as the reference does.
.thumb
.type waitStart, %function
.thumb_func
waitStart:
1:
	ldrh   r2, [r0]
	cmp    r2, #159
	beq    1b
2:
	ldrh   r2, [r0]
	cmp    r2, #159
	bne    2b
	strh   r3, [r1]
	bx     lr

.arm

@ Each frequency's samples a frame, for frequencies 1 to 12; the others take what a reference BIOS
@ shows for them.
samplesPerFrame:
	.hword 0xF1A1, 96, 132, 176, 224, 264, 304, 352, 448, 528, 608, 672, 704, 31, 31, 31

@ Called right after VBlank: once the buffer has played, the DMA starts again from its top. r0, r1
@ and r3 are left the area, the frames left and its mark, or after a restart 0, 0xB600 and
@ DMA2CNT_H's address.
SoundDriverVSync:
	ldr    r0, =AREA_POINTER
	ldr    r0, [r0]
	ldr    r2, =IDENT
	ldr    r3, [r0]
	cmp    r2, r3
	bne    soundReturn
	ldrb   r1, [r0, #DMA_COUNT]
	subs   r1, r1, #1
	strb   r1, [r0, #DMA_COUNT]
	bgt    soundReturn
	ldrb   r1, [r0, #FRAMES]
	strb   r1, [r0, #DMA_COUNT]
	mov    r0, #0
	mov    r1, #0xB6
	mov    r1, r1, lsl #8
	ldr    r2, =0x040000C6
	ldr    r3, =0x040000D2
	strh   r0, [r2]
	strh   r0, [r3]
	strh   r1, [r2]
	strh   r1, [r3]
	bx     lr

@ Stops all 12 channels. r0 is left past the last, r1 0 and r3 0x170.
SoundChannelClear:
	stmfd  sp!, {r4-r7, lr}
	ldr    r0, =AREA_POINTER
	ldr    r6, =IDENT
	ldr    r7, [r0]
	ldr    r0, [r7]
	cmp    r0, r6
	bne    9f
	@ The area is marked busy meanwhile.
	add    r0, r0, #1
	str    r0, [r7]
	@ As long as the reference takes.
	nop
	mov    r1, #12
	add    r0, r7, #CHANNEL
0:
	mov    r2, #0
	strb   r2, [r0]
	add    r0, r0, #CHANNEL_SIZE
	subs   r1, r1, #1
	cmp    r1, #0
	bgt    0b
	@ The word at 0x1C is looked at, as on the reference.
	ldr    r5, [r7, #0x1C]
	cmp    r5, #0
	beq    1f
1:
	str    r6, [r7]
9:
	ldmfd  sp!, {r4-r7}
	ldmfd  sp!, {r3}
	bx     r3

@ Stops the DMA and clears the samples, for when SoundDriverVSync can't be called each frame. r0 is
@ left the area's mark, r1 past the samples and r3 0x170.
SoundDriverVSyncOff:
	stmfd  sp!, {r3, r7, lr}
	ldr    r0, =AREA_POINTER
	ldr    r3, =IDENT
	ldr    r7, [r0]
	ldr    r0, [r7]
	@ The mark, or the mark of an area busy.
	cmp    r0, r3
	bcc    9f
	add    r3, r3, #1
	cmp    r0, r3
	bhi    9f
	add    r0, r0, #1
	str    r0, [r7]
	mov    r0, #0
	ldr    r1, =0x040000C0
	mov    r3, #PCM >> 4
	strh   r0, [r1, #6]
	strh   r0, [r1, #0x12]
	strb   r0, [r7, #DMA_COUNT]
	mov    r3, r3, lsl #4
	add    r1, r7, r3
	str    r0, [sp]
	mov    r0, sp
	ldr    r2, =0x05000000 | (2 * PCM_SIZE / 4)
	thumbBl CpuSet
	ldr    r0, [r7]
	sub    r0, r0, #1
	str    r0, [r7]
9:
	ldmfd  sp!, {r3, r7}
	ldmfd  sp!, {r3}
	bx     r3

@ Starts the DMA again; SoundDriverVSync restarts it from the buffer's top on the next frame. r0 is
@ left 0x040000C0 and r1 0xB600.
SoundDriverVSyncOn:
	mov    r1, #0x5B
	ldr    r0, =0x040000C0
	mov    r1, r1, lsl #9
	strh   r1, [r0, #6]
	strh   r1, [r0, #0x12]
	bx     lr

@ The mixer, called once a frame: it fills the buffer's section that plays after the current one,
@ the frame after next's. Each sample starts as the reverb (the section's and the next one's old
@ samples, scaled by the reverb), and each channel adds to it without saturating. Before that it
@ calls the music player and the GB channels' callbacks, in System mode with interrupts as the caller
@ had them, r1 and r2 the caller's r8 and r9. r0 and r1 are left r8 and r9, and r3 0x170.
SoundDriverMain:
	ldr    r0, =AREA_POINTER
	ldr    r0, [r0]
	ldr    r2, =IDENT
	ldr    r3, [r0]
	cmp    r2, r3
	@ As long as the reference takes.
	nop
	bxne   lr
	@ The area is marked busy while it's mixed.
	add    r3, r3, #1
	str    r3, [r0]
	stmfd  sp!, {r4-r10}
	sub    sp, sp, #0x20
	mov    r11, r0
	@ The music player's callback with its argument, if there is one, then the GB channels'.
	mov    r1, r8
	mov    r2, r9
	ldr    r3, [r11, #MPLAY_MAIN]
	cmp    r3, #0
	beq    1f
	ldr    r0, [r11, #MPLAY_ARG]
	mov    lr, pc
	bx     r3
1:
	ldr    r3, [r11, #CGB_SOUND]
	mov    r0, r11
	mov    lr, pc
	bx     r3
	@ The section: the buffer's frames less the frames until the DMA restarts, plus one.
	ldrb   r1, [r11, #FRAMES]
	ldrb   r2, [r11, #DMA_COUNT]
	sub    r2, r1, r2
	add    r2, r2, #1
	cmp    r2, r1
	subhs  r2, r2, r1
	ldr    r3, [r11, #SAMPLES]
	add    r0, r11, #PCM
	mla    r0, r2, r3, r0
	@ The section after it, wrapping round to the first.
	add    r4, r0, r3
	add    r2, r2, #1
	cmp    r2, r1
	addhs  r4, r11, #PCM
	ldrb   r5, [r11, #REVERB]
	mov    r6, r3
	add    r9, r0, #PCM_SIZE
	cmp    r5, #0
	bne    1f
	@ Without reverb, the section starts silent.
	mov    r7, #0
	mov    r8, #0
	wait   1
	@ Eight bytes at a time, then four if the frame's samples aren't a multiple of eight.
	tst    r6, #4
	strne  r7, [r0], #4
	strne  r7, [r9], #4
	bics   r6, r6, #4
	beq    2f
0:
	stmia  r0!, {r7, r8}
	stmia  r9!, {r7, r8}
	wait   1
	subs   r6, r6, #8
	bne    0b
	b      2f
1:
	add    r10, r4, #PCM_SIZE
0:
	@ Four samples at a time: r0 and r9 the section's right and left, r4 and r10 the next's.
	.rept  4
	ldrsb  r7, [r0]
	ldrsb  r8, [r9]
	add    r7, r7, r8
	ldrsb  r8, [r4], #1
	add    r7, r7, r8
	ldrsb  r8, [r10], #1
	add    r7, r7, r8
	mul    r8, r7, r5
	movs   r7, r7
	mvnmi  r7, r7
	mulmi  r8, r7, r5
	mov    r8, r8, asr #9
	rsbmi  r8, r8, #0
	strb   r8, [r9], #1
	strb   r8, [r0], #1
	.endr
	wait   5
	subs   r6, r6, #4
	bne    0b
2:
	sub    r0, r0, r3
	@ The channels, as many as the area says.
	ldrb   r10, [r11, #CHANNELS]
	add    r9, r11, #CHANNEL
2:
	subs   r10, r10, #1
	bmi    9f
	ldrb   r1, [r9, #CH_STATUS]
	cmp    r1, #0
	wait   10
	beq    3f
	stmfd  sp!, {r0, r9-r11}
	bl     soundChannel
	ldmfd  sp!, {r0, r9-r11}
3:
	add    r9, r9, #CHANNEL_SIZE
	b      2b
9:
	ldr    r1, =IDENT
	str    r1, [r11]
	add    sp, sp, #0x20
	ldmfd  sp!, {r4-r10}
	mov    r0, r8
	mov    r1, r9
	mov    r3, #0x170
	b      swiReturn

@ The channel at r9 for this frame, into the section at r0, with the area at r11: its envelope,
@ then its samples, each the wave's interpolated between the two around its position and scaled by
@ the channel's levels.
.equ CH_STATUS, 0x00
.equ CH_TYPE, 0x01
.equ CH_RIGHT, 0x02
.equ CH_LEFT, 0x03
.equ CH_ATTACK, 0x04
.equ CH_DECAY, 0x05
.equ CH_SUSTAIN, 0x06
.equ CH_RELEASE, 0x07
.equ CH_ENVELOPE, 0x09
.equ CH_ECHO_VOLUME, 0x0C
.equ CH_ECHO_LENGTH, 0x0D
.equ CH_LEVEL_RIGHT, 0x0A
.equ CH_LEVEL_LEFT, 0x0B
.equ CH_COUNT, 0x18                     @ samples to the wave's end
.equ CH_REMAINDER, 0x1C                 @ in 1/rate of a sample
.equ CH_FREQUENCY, 0x20                 @ samples a second
.equ CH_WAVE, 0x24
.equ CH_POSITION, 0x28
soundChannel:
	ldrb   r1, [r9, #CH_STATUS]
	stmfd  sp!, {lr}
	ldr    r12, [r9, #CH_WAVE]
	tst    r1, #0x80
	beq    1f
	@ Starting: the wave from its top, attacking from 0; bit 4 if it loops. Started and stopped at
	@ once, it goes quiet.
	tst    r1, #0x40
	movne  r1, #0
	strbne r1, [r9, #CH_STATUS]
	ldmfdne sp!, {pc}
	ldr    r2, [r12, #12]
	str    r2, [r9, #CH_COUNT]
	add    r2, r12, #16
	str    r2, [r9, #CH_POSITION]
	mov    r2, #0
	str    r2, [r9, #CH_REMAINDER]
	ldr    r3, [r12]
	tst    r3, #0x40000000
	moveq  r1, #0x03
	movne  r1, #0x13
	ldrb   r2, [r9, #CH_ATTACK]
	b      5f
1:
	ldrb   r2, [r9, #CH_ENVELOPE]
	tst    r1, #0x04
	beq    0f
	@ The echo: the envelope held for its frames, then quiet.
	ldrb   r3, [r9, #CH_ECHO_LENGTH]
	subs   r3, r3, #1
	strb   r3, [r9, #CH_ECHO_LENGTH]
	bhi    7f
	b      8f
0:
	tst    r1, #0x40
	beq    2f
	@ Released: down by the release, until it's down to the echo's volume, which it holds; with no
	@ echo it goes quiet.
	ldrb   r3, [r9, #CH_RELEASE]
	mul    r2, r3, r2
	mov    r2, r2, lsr #8
	ldrb   r3, [r9, #CH_ECHO_VOLUME]
	cmp    r2, r3
	bhi    7f
	cmp    r3, #0
	beq    8f
	orr    r1, r1, #0x04
	mov    r2, r3
	b      7f
8:
	mov    r1, #0
	strb   r1, [r9, #CH_STATUS]
	ldmfd  sp!, {pc}
2:
	and    r3, r1, #3
	cmp    r3, #3
	bne    3f
	@ Attack: up by the attack to the top, then decay.
	ldrb   r3, [r9, #CH_ATTACK]
	add    r2, r2, r3
5:
	cmp    r2, #0xFF
	movhs  r2, #0xFF
	bichs  r1, r1, #1
	b      7f
3:
	cmp    r3, #2
	bne    7f
	@ Decay: down by the decay to the sustain level.
	ldrb   r3, [r9, #CH_DECAY]
	mul    r2, r3, r2
	mov    r2, r2, lsr #8
	ldrb   r3, [r9, #CH_SUSTAIN]
	cmp    r2, r3
	movls  r2, r3
	subls  r1, r1, #1
7:
	strb   r1, [r9, #CH_STATUS]
	strb   r2, [r9, #CH_ENVELOPE]
	@ The levels: the volumes by the envelope, by the master volume.
	ldrb   r3, [r11, #VOLUME]
	add    r3, r3, #1
	ldrb   r7, [r9, #CH_RIGHT]
	mul    r7, r2, r7
	mov    r7, r7, lsr #8
	mul    r7, r3, r7
	mov    r7, r7, lsr #4
	strb   r7, [r9, #CH_LEVEL_RIGHT]
	ldrb   r8, [r9, #CH_LEFT]
	mul    r8, r2, r8
	mov    r8, r8, lsr #8
	mul    r8, r3, r8
	mov    r8, r8, lsr #4
	strb   r8, [r9, #CH_LEVEL_LEFT]
	@ The samples: r1 the remainder, r2 the position, r3 the frequency, r4 the rate, r5 the wave's
	@ end, r6 the loop's length (0 for a wave that doesn't loop), r10 2^23 / the rate.
	ldr    r1, [r9, #CH_REMAINDER]
	ldr    r2, [r9, #CH_POSITION]
	ldr    r3, [r9, #CH_FREQUENCY]
	ldr    r4, [r11, #RATE]
	ldr    r10, [r11, #DIVISOR]
	ldr    r5, [r12, #12]
	ldr    r6, [r12, #8]
	sub    r6, r5, r6
	add    r5, r5, r12
	add    r5, r5, #16
	ldrb   r12, [r9, #CH_STATUS]
	tst    r12, #0x10
	moveq  r6, #0
	ldr    r12, [r11, #SAMPLES]
	ldrb   lr, [r9, #CH_TYPE]
	tst    lr, #8
	bne    soundFixed
	cmp    r3, r4
	bhs    soundFast
	stmfd  sp!, {r9}
1:
	cmp    r1, r4
	bhs    3f
	ldrsb  r11, [r2]
	ldrsb  lr, [r2, #1]
	sub    lr, lr, r11
	mul    lr, r1, lr
	mul    lr, r10, lr
	add    r11, r11, lr, asr #23
	mul    lr, r8, r11
	ldrb   r9, [r0, #PCM_SIZE]
	add    r9, r9, lr, asr #8
	strb   r9, [r0, #PCM_SIZE]
	mul    lr, r7, r11
	ldrb   r9, [r0]
	add    r9, r9, lr, asr #8
	strb   r9, [r0], #1
	add    r1, r1, r3
	subs   r12, r12, #1
	bne    1b
	@ Kept for the next frame.
	ldmfd  sp!, {r9}
	str    r1, [r9, #CH_REMAINDER]
	str    r2, [r9, #CH_POSITION]
	sub    r5, r5, r2
	str    r5, [r9, #CH_COUNT]
	ldmfd  sp!, {pc}
3:
	@ On a sample: past the wave's end, round the loop, or the channel stops as it was.
	sub    r1, r1, r4
	add    r2, r2, #1
	wait   9
	cmp    r2, r5
	blo    1b
	cmp    r6, #0
	subne  r2, r2, r6
	bne    1b
	ldmfd  sp!, {r9}
	mov    r1, #0
	strb   r1, [r9, #CH_STATUS]
	ldmfd  sp!, {pc}

@ soundChannel's samples for a step of a sample or more: each output moves on by r6 whole samples,
@ and r3 is what's left of the step, which adds one more sample as it builds up. The position and
@ remainder are kept as the slower loop keeps them: at the last sample, with the whole step added
@ to the remainder. On the stack: the loop's length, then the channel.
soundFast:
	mov    r11, #0
0:
	sub    r3, r3, r4
	add    r11, r11, #1
	cmp    r3, r4
	bhs    0b
	stmfd  sp!, {r6, r9}
	mov    r6, r11
	wait   0
1:
	cmp    r1, r4
	bhs    3f
2:
	ldrsb  r11, [r2]
	ldrsb  lr, [r2, #1]
	sub    lr, lr, r11
	mul    lr, r1, lr
	mul    lr, r10, lr
	add    r11, r11, lr, asr #23
	mul    lr, r8, r11
	ldrb   r9, [r0, #PCM_SIZE]
	add    r9, r9, lr, asr #8
	strb   r9, [r0, #PCM_SIZE]
	mul    lr, r7, r11
	ldrb   r9, [r0]
	add    r9, r9, lr, asr #8
	strb   r9, [r0], #1
	add    r1, r1, r3
	subs   r12, r12, #1
	beq    5f
	add    r2, r2, r6
	wait   15
	cmp    r2, r5
	blo    1b
	b      4f
3:
	@ The remainder made up a sample, or more at a frame's start.
	sub    r1, r1, r4
	add    r2, r2, #1
	cmp    r2, r5
	blo    1b
4:
	@ Past the wave's end: round the loop, or the channel stops as it was.
	ldr    lr, [sp]
	cmp    lr, #0
	beq    6f
0:
	sub    r2, r2, lr
	cmp    r2, r5
	bhs    0b
	b      1b
5:
	ldmfd  sp!, {r6, r9}
	ldr    lr, [r9, #CH_FREQUENCY]
	sub    lr, lr, r3
	add    r1, r1, lr
	str    r1, [r9, #CH_REMAINDER]
	str    r2, [r9, #CH_POSITION]
	sub    r5, r5, r2
	str    r5, [r9, #CH_COUNT]
	ldmfd  sp!, {pc}
6:
	ldmfd  sp!, {r6, r9}
	mov    r1, #0
	strb   r1, [r9, #CH_STATUS]
	ldmfd  sp!, {pc}

@ soundChannel's samples for a fixed pitch: a sample of the wave for each output, as it is, with no
@ remainder left.
soundFixed:
	stmfd  sp!, {r9}
1:
	ldrsb  r11, [r2], #1
	mul    lr, r8, r11
	ldrb   r9, [r0, #PCM_SIZE]
	add    r9, r9, lr, asr #8
	strb   r9, [r0, #PCM_SIZE]
	mul    lr, r7, r11
	ldrb   r9, [r0]
	add    r9, r9, lr, asr #8
	strb   r9, [r0], #1
	wait   2
	cmp    r2, r5
	bhs    3f
2:
	subs   r12, r12, #1
	bne    1b
	ldmfd  sp!, {r9}
	mov    r1, #0
	str    r1, [r9, #CH_REMAINDER]
	str    r2, [r9, #CH_POSITION]
	sub    r5, r5, r2
	str    r5, [r9, #CH_COUNT]
	ldmfd  sp!, {pc}
3:
	@ Past the wave's end: round the loop, or the channel stops as it was.
	cmp    r6, #0
	subne  r2, r2, r6
	bne    2b
	ldmfd  sp!, {r9}
	mov    r1, #0
	strb   r1, [r9, #CH_STATUS]
	ldmfd  sp!, {pc}

@ Calls before SoundDriverInit, or with the work area's mark gone, do nothing: Main and VSync leave
@ r0 the area and r3 what's in its mark; ChannelClear and VSyncOff leave r0 the mark and r3 the way
@ back; Mode leaves r1 the mark.
soundReturn:
	bx     lr

@ The callbacks a work area starts with, until the mixer's own are in.
.thumb
.type soundNothing, %function
.thumb_func
soundNothing:
	bx     lr
.arm
.ltorg
