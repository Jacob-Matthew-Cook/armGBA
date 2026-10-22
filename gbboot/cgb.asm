; armGBA's boot ROM for the Game Boy Color, and with MODEL_AGB the Game Boy Advance: 2,048 bytes at
; $0000-$00FF and $0200-$08FF, as their own. "ARMGBC" appears in place from left to right, each
; column coming in yellow, red, magenta and green, then blue; a chime plays, and the game starts with
; the machine as Nintendo's leaves it, an original Game Boy game in the colours Nintendo's gives it.

INCLUDE "cgbdata.inc"

; Variables in HRAM.
DEF FRAME EQU $FF80
DEF COMBO EQU $FF81
DEF SUM EQU $FF82
; The hand-over's extra wait for the way Nintendo's takes with this cartridge: EXTRA_N for Delay
; (7 cycles each), EXTRA_S where to enter the 7-nop slide.
DEF EXTRA_N EQU $FF83
DEF EXTRA_S EQU $FF84
; Its part after the screen comes on, which a title in the table moves.
DEF LATE_N EQU $FF85
DEF LATE_S EQU $FF86
; Extra cycles, beyond the plain cartridge's 1: licensee $33 others 0, Nintendo's games 1091
; ($33 with "01": 1099), or for a title in the table its own.
DEF PLAIN_N EQU 1
DEF PLAIN_S EQU 6
DEF OTHER_33_N EQU 1
DEF OTHER_33_S EQU 7
DEF NINTENDO_N EQU 156
DEF NINTENDO_S EQU 1
DEF NINTENDO_33_N EQU 158
DEF NINTENDO_33_S EQU 7
DEF BG_TILE EQU 1
DEF TITLE_MAP EQU $9800 + TITLE_Y / 8 * 32 + TITLE_X / 8
; The cartridge's logo at its own size, then (R), where Nintendo's shows it.
DEF LOGO_TILE EQU BG_TILE + TITLE_COLUMNS * TITLE_ROWS / 8
DEF LOGO EQU $8000 + LOGO_TILE * 16
DEF LOGO_MAP EQU $9800 + 13 * 32 + 7
DEF LOGO_FRAME EQU 27
; As Nintendo's logo is seen to: a title column every STEP frames from frame REVEAL, each WAVE
; colours of STAGE frames before its blue (palette WAVE + 1), and the chime when Nintendo's plays. END
; is the frame the hand-over waits are timed from.
DEF REVEAL EQU 49
DEF STEP EQU 2
DEF STAGE EQU 6
DEF CHIME EQU 83
; Delay's steps before each note is struck: the frames' work before it isn't the same length.
DEF C6_WAIT EQU 802
DEF C7_WAIT EQU 820
DEF END EQU 162
IF DEF(MODEL_AGB)
DEF PRE_COLOUR EQU 16083
DEF POST_COLOUR EQU 2437
; A frame of the wait before the screen comes on moved after it, to leave room for a title's wait.
DEF PRE_MONO EQU 18536 - 2509
DEF POST_MONO EQU 2509
ELSE
DEF PRE_COLOUR EQU 16083
DEF POST_COLOUR EQU 2437
DEF PRE_MONO EQU 18535
DEF POST_MONO EQU 1
ENDC

SECTION "low", ROM0[$0000]
Boot:
	ld sp, $FFFE
	jp Main

; Routines called from more than one place sit on the rst addresses, where a call is a byte.
	ds $08 - @, 0
; hl += a, taking the same time whether or not it crosses a page, so a title's wait doesn't depend
; on where the tables are.
At:
	add a, l
	ld l, a
	adc a, h
	sub l
	ld h, a
	ret

	ds $10 - @, 0
; A frame, waiting on VBlank with interrupts off. Leaves a 0.
WaitFrame:
	ld b, 1
.wait
	xor a
	ldh [$FF0F], a
	halt
	dec b
	jr nz, .wait
	ret

; bc times 4 cycles and a few. Leaves a 0.
Delay:
	dec bc
	ld a, b
	or c
	jr nz, Delay
	ret

	ASSERT @ <= $20
	ds $20 - @, 0
; (R) to the tile at hl, in the first plane.
Mark:
	ld de, Registered
.loop
	ld a, [de]
	ld [hl+], a
	inc hl
	inc e
	ld a, e
	cp LOW(Registered + 8)
	jr nz, .loop
	ret

	ds $30 - @, 0
; b bytes from hl to palette port c (BCPD or OCPD).
Palette:
	ld a, [hl+]
	ldh [c], a
	dec b
	jr nz, Palette
	ret

	ds $38 - @, 0
; All BG palettes white; WRAM's bank register as Nintendo's leaves it.
WhiteBG:
	ld a, $F8
	ldh [$FF70], a
	ld a, $80
	ldh [$FF68], a
	ld bc, 32 << 8 | LOW($FF69)
.loop
	ld a, $FF
	ldh [c], a
	ld a, $7F
	ldh [c], a
	dec b
	jr nz, .loop
	ret

; Both VRAM banks and OAM cleared. Leaves a 0, VRAM bank 0.
ClearAll:
	ld a, 1
	ldh [$FF4F], a
	call .vram
	ldh [$FF4F], a
	call .vram
	ld hl, $FE00
	ld b, $A0
.oam
	ld [hl+], a
	dec b
	jr nz, .oam
	ret
.vram
	ld hl, $8000
	xor a
.loop
	ld [hl+], a
	bit 5, h
	jr z, .loop
	ret

; One direction alone, with or without A or B, picks one of 12 colour sets.
Buttons:
	ld a, $20
	call .read
	ret z
	ld c, a
	dec a
	and c
	ret nz
	ld b, -1
.direction
	inc b
	srl c
	jr nz, .direction
	ld a, $10
	call .read
	ld c, a
	ld a, b
	add a, a
	add a, a
	add a, c
	ld e, a
	ld d, 0
	ld hl, ButtonCombos
	add hl, de
	ld a, [hl]
	ldh [COMBO], a
	ret
.read
	ldh [$FF00], a
	ldh a, [$FF00]
	ldh a, [$FF00]
	cpl
	and $0F
	ret

; What Nintendo's leaves below the stack, from $FFF2, then popped into bc, de, hl and af at the
; hand-over; b has the title's sum added.
IF DEF(MODEL_AGB)
DEF LAST_CALL EQU $F8
ELSE
DEF LAST_CALL EQU $F9
ENDC
MonoRegisters:
	db $71, $02, $4D, $01, $00, $FF, $0D, $00, $F5, $05, LAST_CALL, $00
IF DEF(MODEL_AGB)
	dw $0000, $0008, $007C, $1100
ELSE
	dw $0000, $0008, $007C, $1180
ENDC
ColourRegisters:
	db $71, $02, $4D, $01, $C1, $FF, $0D, $00, $D3, $05, LAST_CALL, $00
IF DEF(MODEL_AGB)
	dw $0000, $FF56, $000D, $1100
ELSE
	dw $0000, $FF56, $000D, $1180
ENDC
; The two titles (sums 67 and 88) with which Nintendo's leaves the logo on the map as the original
; Game Boy's does, rows 8 and 9 from column 4 with (R) after the first; they hand over with hl $991A
; and other bytes below the stack: LogoRegisters, which go in place of the registers' address on the
; stack. Keeps a and de.
LogoMap:
	push af
	ld hl, sp + 4
	ld [hl], LOW(LogoRegisters)
	inc hl
	ld [hl], HIGH(LogoRegisters)
	; A cycle, so that its time is even.
	nop
	ld hl, $9904
	ld a, 1
.tile
	ld [hl+], a
	inc a
	cp 13
	jr nz, .next
	ld [hl], $19
	ld l, $24
.next
	cp 25
	jr nz, .tile
	pop af
	ret
LogoRegisters:
	db $71, $02, $4D, $01, $00, $FF, $0D, $00, $03, $06, LAST_CALL, $00
IF DEF(MODEL_AGB)
	dw $0000, $0008, $991A, $1100
ELSE
	dw $0000, $0008, $991A, $1180
ENDC
	ds $FE - @, 0
Handover:
	ldh [$FF50], a

SECTION "main", ROM0[$0200]
Main:
	; Sound on, at once as Nintendo's: channel 1 at a 50% duty for the chime, from full volume dying
	; away, to both sides, its registers written on the cycles Nintendo's writes them.
	ld b, 3
.soundWait
	dec b
	jr nz, .soundWait
	nop
	nop
	nop
	ld a, $80
	ldh [$FF26], a
	ldh [$FF11], a
	ld a, $F3
	ldh [$FF12], a
	ldh [$FF25], a
	ld a, $77
	ldh [$FF24], a
	call ClearAll
	ld hl, Variables
	ld bc, 7 << 8 | LOW(FRAME)
.variables
	ld a, [hl+]
	ldh [c], a
	inc c
	dec b
	jr nz, .variables
	; Wave RAM $00 and $FF by turns, and BGP, as Nintendo's leaves them.
	ld c, LOW($FF30)
	ld a, $FF
.wave
	cpl
	ldh [c], a
	inc c
	bit 6, c
	jr z, .wave
	ld a, $FC
	ldh [$FF47], a
	; The art into the tiles from BG_TILE, doubled, then the patches at full size. hl is past the
	; variables, on the art's page.
	ASSERT HIGH(Variables + 7) == HIGH(Art)
	ld l, LOW(Art)
	ld b, ART_TILES
	call Double
	call Patch
	; The cartridge's logo in the second plane: each tile two 4x4 blocks of it side by side, a
	; block's two bytes 2 apart, the logo's top half then its bottom half.
	ASSERT HIGH(LOGO) == HIGH(LOGO + 7 * 16)
	ld de, $0104
	ld hl, LOGO + 1
.logo
	ld a, [de]
	ld c, a
	inc de
	inc de
	ld a, [de]
	ld b, a
	call LogoRows
	dec de
	ld a, [de]
	ld c, a
	inc de
	inc de
	ld a, [de]
	ld b, a
	call LogoRows
	inc de
	ld a, l
	add a, 8
	ld l, a
	cp LOW(LOGO + 1 + 6 * 16)
	jr nz, .half
	ld l, LOW(LOGO + 1 + 8)
.half
	cp LOW(LOGO + 1 + 6 * 16 + 8)
	jr nz, .logo
	ld l, LOW(LOGO + 1 + 6 * 16)
	rst Mark
	ld hl, LOGO_MAP
	ld a, LOGO_TILE
	ld b, 7
.logoMap
	ld [hl+], a
	inc a
	dec b
	jr nz, .logoMap
	; BG palettes white, then colour 1 of 1 to 5: the columns' colours, then the title's blue.
	rst WhiteBG
	ld hl, TitleColours
	ld a, $80 | (8 + 2)
.colours
	ldh [$FF68], a
	ld e, a
	ld a, [hl+]
	ldh [$FF69], a
	ld a, [hl+]
	ldh [$FF69], a
	ld a, e
	add a, 8
	cp $80 | (48 + 2)
	jr nz, .colours
	; The title's tiles in place, in BG palette 0 (all white) until they come in: down each column,
	; from the fourth, the first three last.
	DEF FIRST_TILE EQU BG_TILE + (TITLE_COLUMNS - SKIPPED) * TITLE_ROWS / 8
	ld hl, TITLE_MAP
	ld c, FIRST_TILE
.titleRow
	ld a, c
	ld b, TITLE_COLUMNS
.titleColumn
	ld [hl+], a
	add a, TITLE_ROWS / 8
	cp LOGO_TILE
	jr c, .tile
	sub LOGO_TILE - BG_TILE
.tile
	dec b
	jr nz, .titleColumn
	ld de, 32 - TITLE_COLUMNS
	add hl, de
	inc c
	ld a, c
	cp FIRST_TILE + TITLE_ROWS / 8
	jr nz, .titleRow
	; Screen on on the cycle the frames are timed from: BG, tiles from $8000.
	ld bc, 149
	call Delay
	ld a, $91
	ldh [$FF40], a
	ld a, 1
	ldh [$FFFF], a

Frame:
	rst WaitFrame
	; The cartridge's logo from when Nintendo's shows it: palette 0's colour 2 black.
	ldh a, [FRAME]
	cp LOGO_FRAME
	jr nz, .columns
	ld a, $84
	ldh [$FF68], a
	xor a
	ldh [$FF69], a
	ldh [$FF69], a
.columns
	; The title's columns, each STEP frames after the one before: palette 1 + its age / STAGE, until
	; the title's blue. b = its age, e = its number.
	ld a, 1
	ldh [$FF4F], a
	ldh a, [FRAME]
	sub REVEAL
	ld b, a
	ld e, 0
	ld hl, TITLE_MAP
.column
	ld a, b
	cp WAVE * STAGE + 1
	jr nc, .next
	ld d, 0
.stage
	inc d
	sub STAGE
	jr nc, .stage
	ld a, d
	push hl
	ld c, TITLE_ROWS / 8
.row
	ld [hl], a
	push de
	ld de, 32
	add hl, de
	pop de
	dec c
	jr nz, .row
	pop hl
.next
	inc hl
	ld a, b
	sub STEP
	ld b, a
	inc e
	ld a, e
	cp TITLE_COLUMNS
	jr nz, .column
	xor a
	ldh [$FF4F], a
	; The chime, C6 then C7 4 frames on, each struck on the cycle Nintendo's is; they share their
	; high byte.
	ASSERT HIGH(1923) == HIGH(1985)
	ldh a, [FRAME]
	ld e, LOW(1923)
	ld bc, C6_WAIT
	sub CHIME
	jr z, .note
	ld e, LOW(1985)
	ld bc, C7_WAIT
	sub 4
	jr nz, .work
	; 12 cycles, which C7_WAIT's steps can't make, and for a colour cartridge 2 more, as Nintendo's
	; spaces its notes.
	ld a, [hl]
	ld a, [hl]
	ld a, [$0143]
	add a, a
	jr nc, .note
	ld a, [hl]
	nop
.note
	; Later by the cartridge's extra wait, as Nintendo's is by its work before its screen comes on,
	; and for an original Game Boy game 4 cycles more.
	ld hl, EXTRA_N
	call Extra
	ld a, [$0143]
	add a, a
	jr c, .colourNote
	ld a, [hl]
	ld a, [hl]
	nop
.colourNote
	ld a, [hl]
	ld a, [hl]
	ld a, [hl]
	call Delay
	ld a, e
	ldh [$FF13], a
	ld a, $80 | HIGH(1923)
	ldh [$FF14], a
.work
	; Out of VBlank: the original Game Boy game's colours, by its title.
	ldh a, [FRAME]
	dec a
	call z, Lookup
	ld hl, FRAME
	inc [hl]
	ld a, [hl]
	cp END
	jp nz, Frame

	; Buttons held now choose an original Game Boy game's colours instead.
	call Buttons
	rst WaitFrame
	ldh [$FF40], a
	call ClearAll
	; The cartridge's logo in the tiles from 1, then (R): VRAM as Nintendo's leaves it, though nothing
	; on the map shows it.
	ld hl, $0104
	ld b, 24
	call Double
	rst Mark
	; BG palettes white for either kind of cartridge, as Nintendo's leaves them.
	rst WhiteBG
	; KEY0 from the cartridge's colour flag, and with it the joypad's select lines low.
	ld a, [$0143]
	ldh [$FF4C], a
	ldh [$FF00], a
	add a, a
	jr c, .colour
	; An original Game Boy game: the compatibility mode, its colours in BG palette 0 and sprite
	; palettes 0 and 1.
	ld a, 4
	ldh [$FF4C], a
	rrca
	rrca
	ldh [$FF6C], a
	ldh a, [COMBO]
	add a, a
	ld e, a
	ld d, 0
	ld hl, Combos
	add hl, de
	ld a, [hl+]
	ld b, a
	ld c, [hl]
	; BG: bits 0-4.
	and 31
	ld e, LOW($FF69)
	call SetColours
	; Sprites: bits 5-9 and 10-14, each less 1.
	ld a, b
	swap a
	rrca
	and 7
	ld d, a
	ld a, c
	and 3
	add a, a
	add a, a
	add a, a
	or d
	inc a
	ld e, LOW($FF6B)
	call SetColours
	ld a, c
	rrca
	rrca
	and 31
	inc a
	call SetColours.again
	ld a, $30
	ldh [$FF00], a
	; The Color's DMA source and destination as Nintendo's leaves them for these games.
	ldh [$FF52], a
	ld a, $D4
	ldh [$FF51], a
	ld a, $99
	ldh [$FF53], a
	ld a, $D0
	ldh [$FF54], a
	ld bc, PRE_MONO
	ld de, POST_MONO
	ld hl, MonoRegisters
	jr .on
.colour
	; A Game Boy Color game: the sprite palettes' index at 1.
	ld a, $81
	ldh [$FF6A], a
	ld bc, PRE_COLOUR
	ld de, POST_COLOUR
	ld hl, ColourRegisters
.on
	; Waits that set the hand-over's timer and screen timing, to the cycle: the cartridge's extra,
	; then bc before the screen comes on, de after its first VBlank; and 4 cycles, which their steps
	; can't make.
	ld a, [hl]
	ld a, [hl]
	push hl
	ld hl, EXTRA_N
	call Extra
	call Delay
	ld a, $91
	ldh [$FF40], a
	rst WaitFrame
	; In its first VBlank, for the two titles, the logo on the map.
	ldh a, [SUM]
	cp 67
	call z, LogoMap
	cp 88
	call z, LogoMap
	; A cycle, so that the check takes 2 of Delay's steps.
	nop
	ld a, [$0143]
	add a, a
IF DEF(MODEL_AGB)
	; A colour cartridge's a cycle later: the jump is taken.
	jr c, .cycle
.cycle
ELSE
	; A colour cartridge's a cycle later: the jump is taken.
	jr c, .cycle
.cycle
ENDC
	ld b, d
	ld c, e
	ld hl, LATE_N
	call Extra
	call Delay
	ldh [$FFFF], a
	; HRAM as Nintendo's leaves it: the logo's first bytes where the variables were, and below the
	; stack what its calls left.
	ldh a, [SUM]
	ld d, a
	ld hl, $0104
	ld bc, 7 << 8 | LOW($FF80)
.logo
	ld a, [hl+]
	ldh [c], a
	inc c
	dec b
	jr nz, .logo
	pop hl
	ld bc, 12 << 8 | LOW($FFF2)
.stack
	ld a, [hl+]
	ldh [c], a
	inc c
	dec b
	jr nz, .stack
	ld sp, hl
	pop bc
	ld a, d
	add a, b
	ld b, a
	pop de
	pop hl
	pop af
IF DEF(MODEL_AGB)
	; The Advance's b is one more, and the flags are as that leaves them.
	inc b
ENDC
	ld sp, $FFFE
	jp Handover

; The cartridge's extra wait at hl: Delay's steps, then where to enter the slide in the low nibble.
; Keeps bc and de.
Extra:
	push bc
	ld a, [hl+]
	ld c, a
	ld b, 0
	call Delay
	pop bc
	ld a, [hl]
	and $0F
	add a, LOW(Slide)
	ld l, a
	ld h, HIGH(Slide)
	jp hl
Slide:
	ASSERT HIGH(Slide) == HIGH(Slide + 7)
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	ret

; Two rows of the cartridge's logo at its size to every other byte from hl: c's left nibbles and
; b's, then their right ones.
LogoRows:
	ld a, b
	swap a
	xor c
	and $0F
	xor c
	ld [hl+], a
	inc hl
	ld a, c
	swap a
	xor b
	and $F0
	xor b
	ld [hl+], a
	inc hl
	ret

; The title's patches (cgbdata.py): each the address of its top left byte, high byte first, then runs
; of rows, a count and 3 bytes, one in each of 3 tile columns, a 0 ending them; a 0 ends the patches.
Patch:
	ld hl, Patches
.patch
	ld a, [hl+]
	or a
	ret z
	ld d, a
	ld a, [hl+]
	ld e, a
.run
	ld a, [hl+]
	or a
	jr z, .patch
	ld b, a
.row
	push hl
	push de
	ld c, 3
.byte
	ld a, [hl+]
	ld [de], a
	ld a, e
	add a, TITLE_ROWS * 2
	ld e, a
	jr nc, .next
	inc d
.next
	dec c
	jr nz, .byte
	pop de
	inc de
	inc de
	dec b
	jr z, .done
	pop hl
	jr .row
.done
	pop af
	jr .run

; b tiles from 1 from the blocks at hl, each pixel doubled: a nibble becomes a byte, written to two
; rows of the first plane. They're popped two bytes at a time, the first first; c's 1 marks a
; nibble's end as it shifts out.
	ASSERT BG_TILE == 1
Double:
	ld sp, hl
	ld hl, $8000 + 16
	ld c, 1
	; 2 cycles, so that the call takes 2 of Delay's steps more than the code it replaced did.
	nop
	nop
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
	ld sp, $FFFC
	ret

; Set a's colours to palette port e from index $80, or at .again on from the last (the index steps
; itself). A standard set is white, its two colours and black. Keeps bc and e.
SetColours:
	push bc
	ld c, e
	dec c
	ld b, a
	ld a, $80
	ldh [c], a
	ld a, b
	pop bc
.again
	push bc
	ld c, e
	sub STANDARD_SETS
	jr nc, .other
	add a, STANDARD_SETS
	add a, a
	add a, a
	ld hl, StandardSets
	rst At
	ld a, $FF
	ldh [c], a
	ld a, $7F
	ldh [c], a
	ld b, 4
	rst Palette
	xor a
	ldh [c], a
	ldh [c], a
	pop bc
	ret
.other
	add a, a
	add a, a
	add a, a
	ld hl, OtherSets
	rst At
	ld b, 8
	rst Palette
	pop bc
	ret
; The original Game Boy game's colours by its title: Nintendo's games by the sum of the title's
; bytes, and for some sums the fourth letter too; anything else the default. And the hand-over's
; extra wait for the way Nintendo's takes: by the licensee, and for Nintendo's original Game Boy
; games whether the title is in the table.
Lookup:
	ld a, [$014B]
	cp 1
	ld bc, NINTENDO_N << 8 | NINTENDO_S
	jr z, .nintendo
	cp $33
	ret nz
	ld hl, $0144
	ld a, [hl+]
	cp '0'
	jr nz, .other
	ld a, [hl]
	cp '1'
	ld bc, NINTENDO_33_N << 8 | NINTENDO_33_S
	jr z, .nintendo
.other
	ld bc, OTHER_33_N << 8 | OTHER_33_S
	jr .extra
.nintendo
	ld a, [$0143]
	add a, a
	jr c, .extra
	push bc
	call .table
	pop bc
	jr nz, .extra
	; In the table, packed by cgbdata.py: Delay's steps before; the slide's entry before in bits 0-2
	; and the combination's bits 0-4 in 3-7; (the cycles after - 14) / 2 in bits 0-6 and the
	; combination's bit 5 in 7. With the newer licensee code 8 cycles more before, a step and a nop.
	push bc
	ld a, [hl+]
	ld d, a
	ld a, [hl+]
	ld e, a
	ld c, [hl]
	rrca
	rrca
	rrca
	and 31
	bit 7, c
	jr z, .combo
	or 32
.combo
	ldh [COMBO], a
	; The cycles after, divided by 7: the steps, then 7 less the remainder for the entry.
	ld a, c
	and 127
	add a, 7
	ld l, a
	ld h, 0
	add hl, hl
	ld bc, -7
	ld a, -1
.after
	inc a
	add hl, bc
	jr c, .after
	ldh [LATE_N], a
	xor a
	sub l
	ldh [LATE_S], a
	pop bc
	ld a, e
	and 7
	ld c, a
	ld a, b
	cp NINTENDO_33_N
	ld b, d
	jr nz, .extra
	inc b
	dec c
.extra
	ld a, b
	ldh [EXTRA_N], a
	ld a, c
	ldh [EXTRA_S], a
	ret
; Z if the title is in the table, hl at its packed colours and waits.
.table
	ld hl, $0134
	ld b, 16
	xor a
.sum
	add a, [hl]
	inc hl
	dec b
	jr nz, .sum
	ldh [SUM], a
	ld c, a
	; Those that need the fourth letter first, then by the sum alone, where a sum whose letters
	; don't match has its own wait too.
	ld hl, Lettered
	ld b, LETTERED
.lettered
	ld a, [hl+]
	cp c
	ld a, [$0137]
	jr nz, .skip
	cp [hl]
	jr z, .letter
.skip
	ld de, 4
	add hl, de
	dec b
	jr nz, .lettered
	; Then by the sum alone: its bit in PlainSums, and the entries of the sums before it with theirs
	; counted. c shifts the bits out, a 1 after them marking its end.
	ld hl, PlainSums
	ld de, PlainPacks - 3
	ld b, c
	inc b
	ld c, $80
.bit
	sla c
	jr nz, .counted
	ld c, [hl]
	inc hl
	scf
	rl c
.counted
	jr nc, .without
	inc de
	inc de
	inc de
.without
	dec b
	jr nz, .bit
	ld h, d
	ld l, e
	jr c, .found
	inc b
	ret
.letter
	inc hl
.found
	xor a
	ret

; FRAME, COMBO and SUM, then the plain cartridge's extra waits.
Variables:
	db 0, 0, 0, PLAIN_N, PLAIN_S, LATE_STEPS, LATE_ENTRY
; (R), as Nintendo's leaves it after the logo.
Registered:
	ASSERT HIGH(Registered) == HIGH(Registered + 7)
	db $3C, $42, $B9, $A5, $B9, $A5, $42, $3C
	INCLUDE "cgbdata-data.inc"
	ASSERT @ <= $0900, "the boot ROM ends at $08FF"
