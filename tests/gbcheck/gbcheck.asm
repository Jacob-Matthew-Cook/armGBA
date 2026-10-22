; gbcheck: what a boot ROM leaves a game, as the game sees it, written to the cartridge's RAM
; (MBC5, 4 banks of 8 KiB) for the harness to compare.
; Bank 0: $A000 registers AF BC DE HL SP, DIV, LY, STAT and the two phase counts; $A020 I/O
; $FF00-$FF7F; $A0A0 IE; $A100 HRAM; $A200 OAM; $A300 the Color's palettes (64 BG, 64 OBJ);
; $A3FC "DONE". Bank 1: VRAM bank 0. Bank 2: VRAM bank 1. Bank 3: WRAM $C000-$DFFF.
; The byte at Config (0 to 8) delays the phase counts by that many cycles, so runs with each
; find the phases to the cycle.

SECTION "entry", ROM0[$100]
	jp Capture
	ds $150 - @, 0

SECTION "main", ROM0[$150]
Config:
	db 0
Capture:
	ld [$DFF0], sp
	ld sp, $DFF0
	push af
	push bc
	push de
	push hl
	ldh a, [$FF04]
	ld b, a
	ldh a, [$FF44]
	ld c, a
	ldh a, [$FF41]
	ld d, a
	push bc
	push de
	; Into the sled at 8 - Config nops.
	ld a, [Config]
	ld e, a
	ld a, 8
	sub e
	ld e, a
	ld d, 0
	ld hl, Sled
	add hl, de
	jp hl
Sled:
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	; Iterations until DIV next changes, then until LY next changes.
	ldh a, [$FF04]
	ld b, a
	ld hl, 0
.div
	inc hl
	ldh a, [$FF04]
	cp b
	jr z, .div
	push hl
	ldh a, [$FF44]
	ld b, a
	ld hl, 0
.ly
	inc hl
	ldh a, [$FF44]
	cp b
	jr z, .ly
	push hl
	; The cartridge RAM on, bank 0.
	ld a, $0A
	ld [$0000], a
	xor a
	ld [$4000], a
	ld hl, $DFE0
	ld de, $A000
	ld c, $12
	call Copy
	ld hl, $FF00
	ld de, $A020
	ld c, $80
	call Copy
	ldh a, [$FFFF]
	ld [$A0A0], a
	ld hl, $FF80
	ld de, $A100
	ld c, $7F
	call Copy
	; The palettes, by index: reading doesn't step it.
	ld de, $A300
	xor a
.bg
	ldh [$FF68], a
	ld b, a
	ldh a, [$FF69]
	ld [de], a
	inc de
	ld a, b
	inc a
	cp $40
	jr nz, .bg
	xor a
.obj
	ldh [$FF6A], a
	ld b, a
	ldh a, [$FF6B]
	ld [de], a
	inc de
	ld a, b
	inc a
	cp $40
	jr nz, .obj
	; OAM in the next VBlank, then the LCD off for VRAM.
.wait
	ldh a, [$FF44]
	cp 144
	jr nz, .wait
	ld hl, $FE00
	ld de, $A200
	ld c, $A0
	call Copy
	xor a
	ldh [$FF40], a
	ld a, 1
	ld [$4000], a
	xor a
	ldh [$FF4F], a
	ld hl, $8000
	ld de, $A000
	call Copy8K
	ld a, 2
	ld [$4000], a
	ld a, 1
	ldh [$FF4F], a
	ld hl, $8000
	ld de, $A000
	call Copy8K
	ld a, 3
	ld [$4000], a
	ld hl, $C000
	ld de, $A000
	call Copy8K
	xor a
	ld [$4000], a
	ld hl, $A3FC
	ld a, 'D'
	ld [hl+], a
	ld a, 'O'
	ld [hl+], a
	ld a, 'N'
	ld [hl+], a
	ld a, 'E'
	ld [hl], a
	di
.end
	jr .end

; c bytes from hl to de.
Copy:
	ld a, [hl+]
	ld [de], a
	inc de
	dec c
	jr nz, Copy
	ret

; 8 KiB from hl to de.
Copy8K:
	ld bc, $2000
.loop
	ld a, [hl+]
	ld [de], a
	inc de
	dec bc
	ld a, b
	or c
	jr nz, .loop
	ret
