; armGBA's boot ROM for the original Game Boy and the Pocket: 256 bytes, as their own. The
; cartridge's logo scrolls down into the middle of the screen, a chime plays, and the game starts
; with the machine as Nintendo's leaves it, the logo still in VRAM. Assembled twice: MODEL_POCKET
; for the Pocket, its A $FF.

DEF PRE_SCREEN EQU 2828
DEF HOLD EQU 80
DEF AFTER_FRAME EQU 179
; Strike's steps before each note.
DEF C6_WAIT EQU 27
DEF C7_WAIT EQU 30
; The logo's tiles from 1 and (R) after them, on map rows 8 and 9 from column 4.
DEF LOGO_TILES EQU 24
DEF LOGO_COLUMNS EQU 12
DEF LOGO_MAP EQU $9800 + 8 * 32 + 4

SECTION "boot", ROM0[$0000]
Boot:
	xor a
	ld hl, $9FFF
.clear
	ld [hl-], a
	bit 7, h
	jr nz, .clear
	; Sound on, channel 1 at a 50% duty, from full volume dying away, to both sides: written in
	; Nintendo's order and 2, 5, 2 and 4 cycles apart, as Nintendo's writes them.
	ld c, LOW($FF26)
	ld de, $FF11
	ld hl, $FF25
	ld a, $80
	ldh [c], a
	ld [de], a
	inc e
	ld a, $F3
	ld [de], a
	ld [hl-], a
	ld a, $77
	ld [hl], a
	; The logo, popped from the cartridge's header two bytes at a time, each pixel doubled: a nibble
	; becomes a byte, written to two rows of the first plane, into the tiles from 1, the first byte
	; popped first. c's 1 marks a nibble's end as it shifts out.
	ld sp, $0104
	ld hl, $8010
	ld b, LOGO_TILES
	ld c, 1
.pair
	pop de
.bit
	sla d
	rl e
	rl c
	ld a, c
	rra
	rl c
	jr nc, .bit
	ld a, c
	ld [hl+], a
	inc hl
	ld [hl+], a
	inc hl
	ld c, 1
	ld a, l
	and 15
	jr nz, .bit
	dec b
	jr nz, .pair
	; Then (R), in the first plane too.
	ld de, Registered
.mark
	ld a, [de]
	ld [hl+], a
	inc hl
	inc e
	ld a, e
	cp LOW(Registered + 8)
	jr nz, .mark
	ld sp, $FFFE
	ld hl, LOGO_MAP
	ld a, 1
.row
	ld b, LOGO_COLUMNS
.column
	ld [hl+], a
	inc a
	dec b
	jr nz, .column
	; d is 0: Registered is on the first page.
	ld e, 32 - LOGO_COLUMNS
	add hl, de
	cp LOGO_TILES + 1
	jr nz, .row
	ld [LOGO_MAP + LOGO_COLUMNS], a
	; Waits that make the hand-over Nintendo's, to the cycle: PRE_SCREEN before the screen comes on,
	; AFTER_FRAME after the last frame's VBlank.
	ld bc, PRE_SCREEN
.early
	dec bc
	ld a, b
	or c
	jr nz, .early
	ld a, [hl]
	ld a, [hl]
	nop
	ld c, 97
	ld a, $FC
	ldh [$FF47], a
	ld a, $91
	ldh [$FF40], a
	ld a, 1
	ldh [$FFFF], a
	; Down from 96 lines above, a line every 2 and 3 frames by turns, as Nintendo's is seen to, so the
	; chime comes when Nintendo's does; waiting on VBlank with interrupts off.
.scroll
	dec c
	ld a, c
	ldh [$FF42], a
	cpl
	and 1
	add a, 2
	ld b, a
	call Wait
	or c
	jr nz, .scroll
	; A frame on, C6, then C7 5 frames after, each struck on the cycle Nintendo's is; they share
	; their high byte.
	ld b, 1
	call Wait
	ASSERT HIGH(1923) == HIGH(1985)
	ld hl, $FF13
	ld de, ($80 | HIGH(1923)) << 8 | LOW(1923)
	ld c, C6_WAIT
	call Strike
	ld b, 5
	call Wait
	ld e, LOW(1985)
	ld c, C7_WAIT
	nop
	call Strike
	ld b, HOLD
	call Wait
	ldh [$FFFF], a
	; 5 cycles, which the steps of 6 below can't make.
	ld a, [hl]
	ld a, [hl]
	nop
	ld b, AFTER_FRAME
.late
	ld a, [hl]
	dec b
	jr nz, .late
IF DEF(MODEL_POCKET)
	ld bc, $FFB0
ELSE
	ld bc, $01B0
ENDC
	push bc
	pop af
	; What Nintendo's leaves below the stack.
	ld hl, $FFFC
	ld [hl], $2E
	inc l
	ld [hl], 0
	ld bc, $0013
	ld de, $00D8
	ld hl, $014D
	jr Handover

; The note in e after c steps of 4 cycles: NR13, then NR14 5 cycles on, as Nintendo's.
Strike:
	dec c
	jr nz, Strike
	ld [hl], e
	inc l
	nop
	nop
	ld [hl], d
	dec l
	ret

; b frames. Leaves a 0.
Wait:
	xor a
	ldh [$FF0F], a
	halt
	dec b
	jr nz, Wait
	ret

	ds $FE - @ - 8, 0
Registered:
	db $3C, $42, $B9, $A5, $B9, $A5, $42, $3C
Handover:
	ldh [$FF50], a
