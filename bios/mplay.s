@ Copyright (c) 2026 Jacob Cook
@
@ This Source Code Form is subject to the terms of the Mozilla Public
@ License, v. 2.0. If a copy of the MPL was not distributed with this
@ file, You can obtain one at http://mozilla.org/MPL/2.0/.
@
@ The music player's functions that SoundDriverGetJumpList hands out, in Thumb code: the sequence
@ commands 0xB1 to 0xCD, which a game's own player calls with r0 its player and r1 the track, and
@ the helpers after them. They follow the public MusicPlayer2000 song format and what
@ tests/bioscheck measures of a reference BIOS's results and timing.

.syntax unified

.include "macros.inc"

@ A track.
.equ TR_FLAGS, 0x00
.equ TR_PATTERN_LEVEL, 0x02
.equ TR_REPEATS, 0x03
.equ TR_GATE, 0x04
.equ TR_KEY, 0x05
.equ TR_VELOCITY, 0x06
.equ TR_KEY_M, 0x08
.equ TR_PITCH_M, 0x09
.equ TR_KEY_SHIFT, 0x0A
.equ TR_KEY_SHIFT_X, 0x0B
.equ TR_TUNE, 0x0C
.equ TR_PITCH_X, 0x0D
.equ TR_BEND, 0x0E
.equ TR_BEND_RANGE, 0x0F
.equ TR_VOLUME_RIGHT, 0x10
.equ TR_VOLUME_LEFT, 0x11
.equ TR_VOLUME, 0x12
.equ TR_VOLUME_X, 0x13
.equ TR_PAN, 0x14
.equ TR_PAN_X, 0x15
.equ TR_MOD_LEVEL, 0x16
.equ TR_MOD, 0x17
.equ TR_MOD_TYPE, 0x18
.equ TR_LFO_SPEED, 0x19
.equ TR_LFO_SPEED_C, 0x1A
.equ TR_LFO_DELAY, 0x1B
.equ TR_LFO_DELAY_C, 0x1C
.equ TR_PRIORITY, 0x1D
.equ TR_ECHO_VOLUME, 0x1E
.equ TR_ECHO_LENGTH, 0x1F
.equ TR_CHANNEL, 0x20
.equ TR_TONE, 0x24
.equ TR_COMMAND, 0x40
.equ TR_PATTERNS, 0x44

@ The sound work area's pointer, and its callback for turning a GB channel off.
.equ AREA_POINTER, 0x03007FF0
.equ AREA_CHANNELS, 0x06
.equ AREA_CHANNEL, 0x50
.equ AREA_CGB_CHANNELS, 0x1C
.equ MIDI_KEY_TO_CGB_FREQ, 0x30
.equ CGB_OSC_OFF, 0x2C
.equ EXT_VOL_PIT, 0x3C

@ A player.
.equ PL_TRACK_COUNT, 0x08
.equ PL_PRIORITY, 0x09
.equ PL_TEMPO, 0x1C
.equ PL_TEMPO_SCALE, 0x1E
.equ PL_TEMPO_STEP, 0x20
.equ PL_FADE_INTERVAL, 0x24
.equ PL_FADE_COUNT, 0x26
.equ PL_FADE_VOLUME, 0x28
.equ PL_TRACKS, 0x2C
.equ PL_TONES, 0x30

@ A channel's links to its track and the track's other channels.
.equ CH_STATUS, 0x00
.equ CH_TYPE, 0x01
.equ CH_RIGHT, 0x02
.equ CH_LEFT, 0x03
.equ CH_ATTACK, 0x04
.equ CH_NOTE, 0x08
.equ CH_ECHO_VOLUME, 0x0C
.equ CH_ECHO_LENGTH, 0x0D
.equ CH_GATE, 0x10
.equ CH_VELOCITY, 0x12
.equ CH_PRIORITY, 0x13
.equ CH_RHYTHM_PAN, 0x14
.equ CH_CGB_LENGTH, 0x1E
.equ CH_CGB_SWEEP, 0x1F
.equ CH_FREQUENCY, 0x20
.equ CH_WAVE, 0x24
.equ CH_KEY, 0x11
.equ CH_TRACK, 0x2C
.equ CH_PREVIOUS, 0x30
.equ CH_NEXT, 0x34
.equ CHANNEL_SIZE, 0x40

.text
.arm

@ SWI 0x2A: the list of functions to r0, each word as long as the reference takes over it: it calls
@ out for each. r1 is left 0, r2 past the list, r3 its last word.
.global SoundDriverGetJumpList
SoundDriverGetJumpList:
	mov    r12, lr
	mov    r1, #36
	ldr    r2, =jumpList
0:
	ldr    r3, [r2]
	thumbBl 1f
	b      2f
1:
	stmfd  sp!, {lr}
	nop
	nop
	b      3f
3:
	nop
	nop
	nop
	ldmfd  sp!, {lr}
	bx     lr
2:
	str    r3, [r0], #4
	add    r2, r2, #4
	subs   r1, r1, #1
	bne    0b
	bx     r12
.ltorg

.thumb

@ Reads the command's next byte to r3, r2 left where it was; the caller's lr is kept in r12.
.macro nextByte
	mov    r12, lr
	bl     readByte
.endm

.macro func name
	.type \name, %function
	.thumb_func
\name:
.endm

@ 0xB1, and the commands with no use: the track stops, and its channels are let go.
func plyFine
	nop
	push   {r4, r5, lr}
	movs   r5, r1
	ldr    r4, [r5, #TR_CHANNEL]
	cmp    r4, #0
	beq    2f
1:
	ldrb   r1, [r4, #CH_STATUS]
	movs   r0, #0xC7
	tst    r1, r0
	beq    3f
	movs   r0, #0x40
	orrs   r1, r0
	strb   r1, [r4, #CH_STATUS]
3:
	movs   r0, r4
	bl     clearChain
	ldr    r4, [r4, #CH_NEXT]
	cmp    r4, #0
	bne    1b
2:
	strb   r4, [r5, #TR_FLAGS]
	pop    {r4, r5}
	pop    {r0}
	bx     r0

@ 0xB2: the command goes on at the pointer after it.
func plyGoto
	push   {lr}
gotoHere:
	movs   r0, #4
0:
	subs   r0, #1
	bne    0b
	nop
	ldr    r2, [r1, #TR_COMMAND]
	ldrb   r3, [r2, #3]
	lsls   r0, r3, #24
	ldrb   r3, [r2, #2]
	lsls   r3, r3, #16
	orrs   r0, r3
	ldrb   r3, [r2, #1]
	lsls   r3, r3, #8
	orrs   r0, r3
	ldrb   r3, [r2]
	orrs   r0, r3
	str    r0, [r1, #TR_COMMAND]
	pop    {r0}
	bx     r0

@ 0xB3: goes to a pattern, up to three deep, to come back after it.
func plyPattern
	ldrb   r2, [r1, #TR_PATTERN_LEVEL]
	cmp    r2, #3
	bhs    9f
	lsls   r2, r2, #2
	adds   r3, r1, r2
	ldr    r2, [r1, #TR_COMMAND]
	adds   r2, #4
	str    r2, [r3, #TR_PATTERNS]
	ldrb   r2, [r1, #TR_PATTERN_LEVEL]
	adds   r2, #1
	strb   r2, [r1, #TR_PATTERN_LEVEL]
	b      plyGoto
9:
	b      plyFine

@ 0xB4: back from a pattern.
func plyPatternEnd
	ldrb   r2, [r1, #TR_PATTERN_LEVEL]
	cmp    r2, #0
	beq    1f
	subs   r2, #1
	strb   r2, [r1, #TR_PATTERN_LEVEL]
	lsls   r3, r2, #2
	adds   r3, r1
	ldr    r2, [r3, #TR_PATTERNS]
	str    r2, [r1, #TR_COMMAND]
1:
	bx     lr

@ 0xB5: repeats from the pointer the byte's number of times, or for ever if it's 0.
func plyRepeat
	push   {lr}
	ldr    r2, [r1, #TR_COMMAND]
	ldrb   r3, [r2]
	cmp    r3, #0
	bne    1f
	adds   r2, #1
	str    r2, [r1, #TR_COMMAND]
	b      gotoHere
1:
	ldrb   r3, [r1, #TR_REPEATS]
	adds   r3, #1
	strb   r3, [r1, #TR_REPEATS]
	mov    r12, r3
	bl     readByte
	cmp    r12, r3
	bhs    2f
	b      gotoHere
2:
	movs   r3, #0
	strb   r3, [r1, #TR_REPEATS]
	adds   r2, #5
	str    r2, [r1, #TR_COMMAND]
	pop    {r0}
	bx     r0

@ 0xBA
func plyPriority
	nextByte
	strb   r3, [r1, #TR_PRIORITY]
	bx     r12

@ 0xBB: the tempo, in beats a minute over 2.
func plyTempo
	nextByte
	lsls   r3, r3, #1
	strh   r3, [r0, #PL_TEMPO]
	ldrh   r2, [r0, #PL_TEMPO_SCALE]
	muls   r3, r2
	lsrs   r3, r3, #8
	strh   r3, [r0, #PL_TEMPO_STEP]
	bx     r12

@ 0xBC
func plyKeyShift
	nextByte
	strb   r3, [r1, #TR_KEY_SHIFT]
	ldrb   r3, [r1, #TR_FLAGS]
	movs   r2, #0xC
	orrs   r3, r2
	strb   r3, [r1, #TR_FLAGS]
	bx     r12

@ 0xBD: the instrument, from the player's tones.
func plyVoice
	nextByte
	movs   r2, r3
	movs   r3, #7
0:
	subs   r3, #1
	bne    0b
	movs   r3, r2
	lsls   r2, r3, #1
	adds   r2, r3
	lsls   r2, r2, #2
	ldr    r3, [r0, #PL_TONES]
	adds   r2, r3
	ldr    r3, [r2]
	str    r3, [r1, #TR_TONE]
	ldr    r3, [r2, #4]
	str    r3, [r1, #TR_TONE + 4]
	ldr    r3, [r2, #8]
	str    r3, [r1, #TR_TONE + 8]
	bx     r12

@ 0xBE
func plyVolume
	nextByte
	strb   r3, [r1, #TR_VOLUME]
	ldrb   r3, [r1, #TR_FLAGS]
	movs   r2, #3
	orrs   r3, r2
	strb   r3, [r1, #TR_FLAGS]
	bx     r12

@ 0xBF
func plyPan
	nextByte
	subs   r3, #0x40
	strb   r3, [r1, #TR_PAN]
	ldrb   r3, [r1, #TR_FLAGS]
	movs   r2, #3
	orrs   r3, r2
	strb   r3, [r1, #TR_FLAGS]
	bx     r12

@ 0xC0
func plyBend
	nextByte
	subs   r3, #0x40
	strb   r3, [r1, #TR_BEND]
	ldrb   r3, [r1, #TR_FLAGS]
	movs   r2, #0xC
	orrs   r3, r2
	strb   r3, [r1, #TR_FLAGS]
	bx     r12

@ 0xC1
func plyBendRange
	nextByte
	strb   r3, [r1, #TR_BEND_RANGE]
	ldrb   r3, [r1, #TR_FLAGS]
	movs   r2, #0xC
	orrs   r3, r2
	strb   r3, [r1, #TR_FLAGS]
	bx     r12

@ 0xC8
func plyTune
	nextByte
	subs   r3, #0x40
	strb   r3, [r1, #TR_TUNE]
	ldrb   r3, [r1, #TR_FLAGS]
	movs   r2, #0xC
	orrs   r3, r2
	strb   r3, [r1, #TR_FLAGS]
	bx     r12

@ 0xC2: the LFO's speed; at 0 the modulation stops.
func plyLfoSpeed
	nextByte
	strb   r3, [r1, #TR_LFO_SPEED]
	cmp    r3, #0
	bne    1f
	strb   r3, [r1, #TR_MOD_LEVEL]
1:
	bx     r12

@ 0xC3
func plyLfoDelay
	nextByte
	strb   r3, [r1, #TR_LFO_DELAY]
	bx     r12

@ 0xC4: the modulation's depth; at 0 it stops.
func plyMod
	nextByte
	strb   r3, [r1, #TR_MOD]
	cmp    r3, #0
	bne    1f
	strb   r3, [r1, #TR_MOD_LEVEL]
1:
	bx     r12

@ 0xC5: what the modulation changes: pitch, volume or pan.
func plyModType
	nextByte
	ldrb   r0, [r1, #TR_MOD_TYPE]
	wait   2
	strb   r3, [r1, #TR_MOD_TYPE]
	ldrb   r3, [r1, #TR_FLAGS]
	movs   r2, #0xF
	orrs   r3, r2
	strb   r3, [r1, #TR_FLAGS]
	bx     r12

@ 0xCC: a byte to a sound register.
func plyPort
	ldr    r2, [r1, #TR_COMMAND]
	adds   r3, r2, #1
	str    r3, [r1, #TR_COMMAND]
	ldrb   r3, [r2]
	ldr    r0, =0x04000060
	adds   r0, r3
	ldr    r2, [r1, #TR_COMMAND]
	adds   r3, r2, #1
	str    r3, [r1, #TR_COMMAND]
	ldrb   r3, [r2]
	strb   r3, [r0]
	wait   5
	bx     lr

@ The end of a tied note: its key if one follows, or the last one; the first of the track's
@ channels playing it is released.
func plyEndTie
	push   {lr}
	wait   4
	ldr    r2, [r1, #TR_COMMAND]
	ldrb   r3, [r2]
	cmp    r3, #0x80
	bhs    1f
	strb   r3, [r1, #TR_KEY]
	adds   r2, #1
	str    r2, [r1, #TR_COMMAND]
	b      2f
1:
	ldrb   r3, [r1, #TR_KEY]
2:
	ldr    r1, [r1, #TR_CHANNEL]
	cmp    r1, #0
	beq    5f
3:
	ldrb   r2, [r1, #CH_STATUS]
	movs   r0, #0x83
	tst    r2, r0
	beq    4f
	ldrb   r0, [r1, #CH_KEY]
	cmp    r0, r3
	bne    4f
	movs   r0, #0x40
	tst    r2, r0
	bne    5f
	orrs   r2, r0
	strb   r2, [r1, #CH_STATUS]
	b      5f
4:
	ldr    r1, [r1, #CH_NEXT]
	cmp    r1, #0
	bne    3b
5:
	pop    {r0}
	bx     r0

func readByte
	ldr    r2, [r1, #TR_COMMAND]
	adds   r3, r2, #1
	str    r3, [r1, #TR_COMMAND]
	ldrb   r3, [r2]
	wait   12
	bx     lr

@ Stops the track at r1 if it's playing: its channels go quiet, the GB ones by the sound area's
@ callback, and are let go.
func trackStop
	push   {r4-r6, lr}
	movs   r5, r1
	ldrb   r1, [r5, #TR_FLAGS]
	movs   r0, #0x80
	tst    r1, r0
	beq    9f
	ldr    r4, [r5, #TR_CHANNEL]
	cmp    r4, #0
	beq    8f
	movs   r6, #0
1:
	ldrb   r0, [r4, #CH_STATUS]
	cmp    r0, #0
	beq    2f
	ldrb   r0, [r4, #CH_TYPE]
	movs   r3, #7
	ands   r0, r3
	beq    3f
	ldr    r3, =AREA_POINTER
	ldr    r3, [r3]
	ldr    r3, [r3, #CGB_OSC_OFF]
	bl     callR3
3:
	strb   r6, [r4, #CH_STATUS]
	str    r6, [r4, #CH_TRACK]
2:
	ldr    r4, [r4, #CH_NEXT]
	cmp    r4, #0
	bne    1b
8:
	str    r4, [r5, #TR_CHANNEL]
9:
	pop    {r4-r6}
	pop    {r0}
	bx     r0

@ A fade out's step, once a frame for the player at r0: every interval's frames the volume drops a
@ sixteenth and the playing tracks take it; when none is left, the tracks stop.
func fadeOutBody
	push   {r4-r6, lr}
	wait   2
	movs   r4, r0
	ldrh   r0, [r4, #PL_FADE_INTERVAL]
	cmp    r0, #0
	beq    9f
	wait   2
	ldrh   r1, [r4, #PL_FADE_COUNT]
	subs   r1, #1
	strh   r1, [r4, #PL_FADE_COUNT]
	bne    9f
	ldrh   r1, [r4, #PL_FADE_VOLUME]
	subs   r1, #16
	strh   r1, [r4, #PL_FADE_VOLUME]
	lsls   r1, r1, #16
	asrs   r1, r1, #16
	beq    5f
	bmi    5f
	wait   6
	strh   r0, [r4, #PL_FADE_COUNT]
	ldrb   r1, [r4, #PL_TRACK_COUNT]
	ldr    r0, [r4, #PL_TRACKS]
	cmp    r1, #0
	beq    9f
1:
	wait   4
	ldrb   r2, [r0, #TR_FLAGS]
	movs   r3, #0x80
	tst    r2, r3
	beq    2f
	ldrh   r3, [r4, #PL_FADE_VOLUME]
	lsrs   r3, r3, #2
	strb   r3, [r0, #TR_VOLUME_X]
	movs   r3, #3
	orrs   r2, r3
	strb   r2, [r0, #TR_FLAGS]
2:
	adds   r0, #0x50
	subs   r1, #1
	bgt    1b
9:
	pop    {r4-r6}
	pop    {r3}
	bx     r3
5:
	ldrb   r5, [r4, #PL_TRACK_COUNT]
	ldr    r6, [r4, #PL_TRACKS]
	cmp    r5, #0
	beq    9b
6:
	wait   1
	movs   r0, r4
	movs   r1, r6
	bl     trackStop
	movs   r3, #0
	strb   r3, [r6, #TR_FLAGS]
	adds   r6, #0x50
	subs   r5, #1
	bgt    6b
	b      9b

@ The track at r1's volumes and pitch from its settings, where its flags say they changed, then the
@ sound area's callback for them, with r0 the player.
func trkVolPitSet
	push   {r4, r5, lr}
	movs   r4, r0
	movs   r5, r1
	ldrb   r2, [r1, #TR_FLAGS]
	lsrs   r3, r2, #1
	bcc    3f
	nop
	@ The volume: the track's by its fade's, plus the modulation if that's on the volume.
	ldrb   r3, [r1, #TR_VOLUME]
	ldrb   r2, [r1, #TR_VOLUME_X]
	muls   r3, r2
	lsrs   r3, r3, #5
	ldrb   r2, [r1, #TR_MOD_TYPE]
	cmp    r2, #1
	bne    1f
	movs   r2, #TR_MOD_LEVEL
	ldrsb  r2, [r1, r2]
	adds   r3, r2
1:
	@ The pan, with the modulation if that's on the pan, from -128 to 127.
	movs   r2, #TR_PAN
	ldrsb  r2, [r1, r2]
	lsls   r2, r2, #1
	movs   r0, #TR_PAN_X
	ldrsb  r0, [r1, r0]
	adds   r2, r0
	ldrb   r0, [r1, #TR_MOD_TYPE]
	cmp    r0, #2
	bne    2f
	movs   r0, #TR_MOD_LEVEL
	ldrsb  r0, [r1, r0]
	adds   r2, r0
2:
	movs   r0, #0x80
	negs   r0, r0
	cmp    r2, r0
	bge    4f
	movs   r2, r0
	b      5f
4:
	cmp    r2, #0x7F
	ble    5f
	movs   r2, #0x7F
5:
	movs   r0, r2
	adds   r0, #0x80
	muls   r0, r3
	lsrs   r0, r0, #8
	strb   r0, [r1, #TR_VOLUME_RIGHT]
	movs   r0, #0x7F
	subs   r0, r2
	muls   r0, r3
	lsrs   r0, r0, #8
	strb   r0, [r1, #TR_VOLUME_LEFT]
	ldrb   r2, [r1, #TR_FLAGS]
3:
	@ The pitch in 1/256 semitones: the key shifts, the bend over its range, the tune, the fine
	@ pitch, and the modulation if that's on the pitch.
	movs   r3, #4
	tst    r2, r3
	beq    6f
	movs   r3, #TR_BEND
	ldrsb  r3, [r1, r3]
	ldrb   r2, [r1, #TR_BEND_RANGE]
	muls   r3, r2
	lsls   r3, r3, #2
	movs   r2, #TR_TUNE
	ldrsb  r2, [r1, r2]
	lsls   r2, r2, #2
	adds   r3, r2
	movs   r2, #TR_KEY_SHIFT
	ldrsb  r2, [r1, r2]
	lsls   r2, r2, #8
	adds   r3, r2
	movs   r2, #TR_KEY_SHIFT_X
	ldrsb  r2, [r1, r2]
	lsls   r2, r2, #8
	adds   r3, r2
	ldrb   r2, [r1, #TR_PITCH_X]
	adds   r3, r2
	ldrb   r2, [r1, #TR_MOD_TYPE]
	cmp    r2, #0
	bne    7f
	movs   r2, #TR_MOD_LEVEL
	ldrsb  r2, [r1, r2]
	lsls   r2, r2, #4
	adds   r3, r2
7:
	asrs   r2, r3, #8
	strb   r2, [r1, #TR_KEY_M]
	strb   r3, [r1, #TR_PITCH_M]
6:
	movs   r0, r4
	ldr    r2, =AREA_POINTER
	ldr    r2, [r2]
	ldr    r2, [r2, #EXT_VOL_PIT]
	bl     callR2
	ldrb   r0, [r5, #TR_FLAGS]
	movs   r3, #5
	bics   r0, r3
	strb   r0, [r5, #TR_FLAGS]
	pop    {r4, r5}
	pop    {r3}
	bx     r3

@ The sound's sample rate from r0's bits 16 to 19, as SoundDriverMode sets it. r0 is left 0x80,
@ r1 the CPSR with interrupts off and r2 the samples a frame.
func sampleFreqSet
	push   {lr}
	lsls   r1, r0, #12
	lsrs   r1, r1, #28
	ldr    r0, =AREA_POINTER
	ldr    r0, [r0]
	ldr    r2, =setFrequency
	bl     callR2
	movs   r0, #0x80
	pop    {r3}
	bx     r3

func callR2
	bx     r2

func callR3
	bx     r3

@ A note on, which the sound area points to: r0 the length's index, r1 the player, r2 the track. The
@ key, velocity and extra length follow in the command if they're below 0x80. The track's tone, or
@ its key's from a drum kit or key split, gets a channel that's free, or else the one stopping or
@ of the lowest priority, which joins the track's channels and starts the note.
func plyNote
	push   {r4-r7, lr}
	mov    r4, r8
	mov    r5, r9
	mov    r6, r10
	mov    r7, r11
	push   {r4-r7}
	movs   r5, r1
	movs   r4, r2
	ldr    r3, =clockTable
	ldrb   r3, [r3, r0]
	strb   r3, [r4, #TR_GATE]
	ldr    r2, [r4, #TR_COMMAND]
	ldrb   r1, [r2]
	cmp    r1, #0x80
	bhs    1f
	wait   5
	strb   r1, [r4, #TR_KEY]
	adds   r2, #1
	ldrb   r1, [r2]
	cmp    r1, #0x80
	bhs    1f
	wait   5
	strb   r1, [r4, #TR_VELOCITY]
	adds   r2, #1
	ldrb   r1, [r2]
	cmp    r1, #0x80
	bhs    1f
	wait   5
	adds   r3, r1
	strb   r3, [r4, #TR_GATE]
	adds   r2, #1
1:
	str    r2, [r4, #TR_COMMAND]
	@ The priority, the track's and its player's, up to 255.
	ldrb   r0, [r4, #TR_PRIORITY]
	ldrb   r1, [r5, #PL_PRIORITY]
	adds   r0, r1
	cmp    r0, #0xFF
	bls    2f
	movs   r0, #0xFF
2:
	mov    r8, r0
	@ The tone, r7, the key, r9, and a drum's pan, r10.
	ldrb   r0, [r4, #TR_KEY]
	mov    r9, r0
	movs   r0, #0
	mov    r10, r0
	movs   r7, r4
	adds   r7, #TR_TONE
	ldrb   r1, [r7]
	movs   r0, #0xC0
	tst    r0, r1
	beq    3f
	mov    r0, r9
	movs   r2, #0x40
	tst    r2, r1
	beq    4f
	ldr    r2, [r7, #8]
	ldrb   r0, [r2, r0]
4:
	lsls   r1, r0, #1
	adds   r1, r0
	lsls   r1, r1, #2
	ldr    r0, [r7, #4]
	adds   r7, r0, r1
	ldrb   r1, [r7]
	movs   r0, #0xC0
	tst    r0, r1
	beq    5f
	b      noteDone
5:
	movs   r0, #TR_TONE
	ldrb   r0, [r4, r0]
	movs   r2, #0x80
	tst    r0, r2
	beq    3f
	ldrb   r0, [r7, #3]
	tst    r0, r2
	beq    6f
	subs   r0, #0xC0
	lsls   r0, r0, #1
	mov    r10, r0
6:
	ldrb   r0, [r7, #1]
	mov    r9, r0
3:
	@ A GB tone's channel is its own, unless that's playing with a higher priority, or the same
	@ from a track before this one.
	mov    r11, r7
	ldrb   r1, [r7]
	movs   r0, #7
	ands   r1, r0
	beq    14f
	ldr    r0, =AREA_POINTER
	ldr    r0, [r0]
	ldr    r0, [r0, #AREA_CGB_CHANNELS]
	cmp    r0, #0
	beq    16f
	subs   r1, #1
	lsls   r1, r1, #6
	adds   r0, r1
	ldrb   r1, [r0, #CH_STATUS]
	movs   r2, #0xC7
	tst    r1, r2
	beq    noteChannel
	movs   r2, #0x40
	tst    r1, r2
	bne    noteChannel
	ldrb   r1, [r0, #CH_PRIORITY]
	cmp    r1, r8
	bhi    16f
	bcc    noteChannel
	ldr    r1, [r0, #CH_TRACK]
	cmp    r1, r4
	bhs    noteChannel
16:
	b      noteDone
14:
	@ A channel: the first free one, else the first stopping one or the lowest priority's, the
	@ latest of those of the same priority from the lowest track. The tone waits in r11. It takes
	@ as long to start as the reference.
	push   {r5}
	ldr    r0, =AREA_POINTER
	ldr    r0, [r0]
	ldrb   r5, [r0, #AREA_CHANNELS]
	adds   r0, #AREA_CHANNEL
	movs   r2, #0
	mov    r3, r8
	mov    r12, r4
	movs   r6, #0
7:
	ldrb   r7, [r0, #CH_STATUS]
	movs   r1, #0xC7
	tst    r1, r7
	beq    13f
	movs   r1, #0x40
	tst    r1, r7
	beq    8f
	cmp    r2, #0
	bne    9f
	adds   r2, #1
	ldrb   r3, [r0, #CH_PRIORITY]
	ldr    r7, [r0, #CH_TRACK]
	mov    r12, r7
	b      10f
8:
	cmp    r2, #0
	bne    11f
9:
	ldrb   r7, [r0, #CH_PRIORITY]
	cmp    r7, r3
	bhs    12f
	movs   r3, r7
	ldr    r7, [r0, #CH_TRACK]
	mov    r12, r7
	b      10f
12:
	bhi    11f
	ldr    r7, [r0, #CH_TRACK]
	cmp    r7, r12
	bhi    11f
	mov    r12, r7
10:
	movs   r6, r0
11:
	adds   r0, #CHANNEL_SIZE
	subs   r5, #1
	bgt    7b
	pop    {r5}
	movs   r0, r6
	cmp    r0, #0
	bne    noteChannel
	b      noteDone
13:
	pop    {r5}
noteChannel:
	movs   r6, r0
	@ The channel joins the front of the track's list.
	bl     clearChain
	movs   r0, #0
	str    r0, [r6, #CH_PREVIOUS]
	ldr    r0, [r4, #TR_CHANNEL]
	str    r0, [r6, #CH_NEXT]
	cmp    r0, #0
	beq    1f
	str    r6, [r0, #CH_PREVIOUS]
1:
	str    r6, [r4, #TR_CHANNEL]
	str    r4, [r6, #CH_TRACK]
	@ The modulation starts again after its delay.
	ldrb   r0, [r4, #TR_LFO_DELAY]
	strb   r0, [r4, #TR_LFO_DELAY_C]
	cmp    r0, #0
	beq    1f
	movs   r0, #0
	strb   r0, [r4, #TR_LFO_SPEED_C]
	strb   r0, [r4, #TR_MOD_LEVEL]
1:
	movs   r0, r5
	movs   r1, r4
	bl     trkVolPitSet
	@ The channel takes the note and the tone.
	mov    r7, r11
	ldrb   r0, [r4, #TR_GATE]
	strb   r0, [r6, #CH_GATE]
	ldrb   r0, [r4, #TR_KEY]
	strb   r0, [r6, #CH_KEY]
	ldrb   r0, [r4, #TR_VELOCITY]
	strb   r0, [r6, #CH_VELOCITY]
	mov    r0, r8
	strb   r0, [r6, #CH_PRIORITY]
	mov    r0, r9
	strb   r0, [r6, #CH_NOTE]
	b      noteTone
noteDone:
	pop    {r4-r7}
	mov    r8, r4
	mov    r9, r5
	mov    r10, r6
	mov    r11, r7
	pop    {r4-r7}
	pop    {r0}
	bx     r0
.ltorg

@ The rest of a note on, with r4 the track, r5 the player, r6 the channel, r7 the tone.
noteTone:
	ldrb   r0, [r7]
	strb   r0, [r6, #CH_TYPE]
	ldr    r0, [r7, #4]
	str    r0, [r6, #CH_WAVE]
	ldr    r0, [r7, #8]
	str    r0, [r6, #CH_ATTACK]
	ldrb   r0, [r4, #TR_ECHO_VOLUME]
	strb   r0, [r6, #CH_ECHO_VOLUME]
	ldrb   r0, [r4, #TR_ECHO_LENGTH]
	strb   r0, [r6, #CH_ECHO_LENGTH]
	@ A drum's channel keeps its pan, and the volumes are the velocity by the track's.
	movs   r0, #TR_TONE
	ldrb   r0, [r4, r0]
	lsls   r0, r0, #24
	bcc    1f
	mov    r0, r10
	strb   r0, [r6, #CH_RHYTHM_PAN]
1:
	ldrb   r1, [r6, #CH_VELOCITY]
	ldrb   r0, [r4, #TR_VOLUME_RIGHT]
	muls   r0, r1
	lsrs   r0, r0, #7
	strb   r0, [r6, #CH_RIGHT]
	ldrb   r0, [r4, #TR_VOLUME_LEFT]
	muls   r0, r1
	lsrs   r0, r0, #7
	strb   r0, [r6, #CH_LEFT]
	@ Its frequency, from the key and the track's pitch; a GB channel's from the sound area's
	@ callback, with its length, and its sweep unless the tone's top bit says there's none.
	ldrb   r1, [r6, #CH_NOTE]
	movs   r0, #TR_KEY_M
	ldrsb  r0, [r4, r0]
	adds   r1, r0
	bpl    1f
	movs   r1, #0
1:
	ldrb   r2, [r4, #TR_PITCH_M]
	ldrb   r0, [r7]
	lsls   r0, r0, #29
	beq    2f
	lsrs   r0, r0, #29
	ldrb   r3, [r7, #2]
	strb   r3, [r6, #CH_CGB_LENGTH]
	ldrb   r3, [r7, #3]
	lsls   r3, r3, #24
	bmi    3f
	lsrs   r3, r3, #24
	strb   r3, [r6, #CH_CGB_SWEEP]
3:
	ldr    r3, =AREA_POINTER
	ldr    r3, [r3]
	ldr    r3, [r3, #MIDI_KEY_TO_CGB_FREQ]
	bl     callR3
	str    r0, [r6, #CH_FREQUENCY]
	b      7f
2:
	ldr    r0, [r6, #CH_WAVE]
	ldr    r3, =midiKeyToFreq
	bl     callR3
	str    r0, [r6, #CH_FREQUENCY]
7:
	movs   r0, #0x80
	strb   r0, [r6, #CH_STATUS]
	ldrb   r0, [r4, #TR_FLAGS]
	movs   r1, #0xF
	bics   r0, r1
	strb   r0, [r4, #TR_FLAGS]
	b      noteDone

@ Each length's ticks.
clockTable:
	.byte  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24
	.byte  28, 30, 32, 36, 40, 42, 44, 48, 52, 54, 56, 60, 64, 66, 68, 72, 76, 78, 80, 84, 88, 90
	.byte  92, 96
	.balign 4

@ Takes the channel at r0 out of its track's list of channels.
func clearChain
	ldr    r3, [r0, #CH_TRACK]
	cmp    r3, #0
	beq    3f
	ldr    r2, [r0, #CH_PREVIOUS]
	ldr    r1, [r0, #CH_NEXT]
	cmp    r2, #0
	beq    1f
	str    r1, [r2, #CH_NEXT]
	b      2f
1:
	str    r1, [r3, #TR_CHANNEL]
2:
	cmp    r1, #0
	beq    4f
	str    r2, [r1, #CH_PREVIOUS]
4:
	movs   r1, #0
	str    r1, [r0, #CH_TRACK]
3:
	bx     lr

@ Clears the 0x40 bytes at r0.
func clear64
	movs   r1, #0
	movs   r2, #0
	movs   r3, #0
	stmia  r0!, {r1-r3}
	stmia  r0!, {r1-r3}
	stmia  r0!, {r1-r3}
	stmia  r0!, {r1-r3}
	stmia  r0!, {r1-r3}
	str    r1, [r0]
	adds   r0, #4
	bx     lr

func plyNothing
	bx     lr

.ltorg
.align 2
.global jumpList, plyNote
jumpList:
	.word  plyFine, plyGoto, plyPattern, plyPatternEnd, plyRepeat, plyFine, plyFine, plyFine
	.word  plyFine, plyPriority, plyTempo, plyKeyShift, plyVoice, plyVolume, plyPan, plyBend
	.word  plyBendRange, plyLfoSpeed, plyLfoDelay, plyMod, plyModType, plyFine, plyFine, plyTune
	.word  plyFine, plyFine, plyFine, plyPort, plyFine, plyEndTie, sampleFreqSet, trackStop
	.word  fadeOutBody, trkVolPitSet, clearChain, clear64
