; gbpal: an original Game Boy cartridge that shows the colours a Game Boy Color's boot ROM gave it:
; BG colours 0 to 3 as bands across the screen, and colours 1 to 3 of both sprite palettes as
; squares, each through an identity BGP/OBP, for the harness to read from a screenshot. Built with
; COLOUR0, the first squares show colour 0 of each sprite palette instead of colour 1.

SECTION "entry", ROM0[$100]
	jp Main
	ds $150 - @, 0

SECTION "main", ROM0[$150]
Main:
	di
.vblank
	ldh a, [$FF44]
	cp 144
	jr nz, .vblank
	xor a
	ldh [$FF40], a
	; Tiles 0 to 3, each all one colour.
	ld hl, $8000
	ld de, Shades
	ld c, 4
.tile
	ld a, [de]
	inc de
	ld b, a
	ld a, [de]
	inc de
	push de
	ld d, a
	ld e, 8
.line
	ld a, b
	ld [hl+], a
	ld a, d
	ld [hl+], a
	dec e
	jr nz, .line
	pop de
	dec c
	jr nz, .tile
	; Every row: five tiles of each colour.
	ld hl, $9800
	ld c, 18
.row
	ld de, Row
	ld b, 32
.column
	ld a, [de]
	inc de
	ld [hl+], a
	dec b
	jr nz, .column
	dec c
	jr nz, .row
	ld hl, $FE00
	ld de, Sprites
	ld b, Sprites.end - Sprites
.sprite
	ld a, [de]
	inc de
	ld [hl+], a
	dec b
	jr nz, .sprite
	ld b, $A0 - (Sprites.end - Sprites)
	xor a
.clear
	ld [hl+], a
	dec b
	jr nz, .clear
	ld a, $E4
	ldh [$FF47], a
IF DEF(COLOUR0)
	; Sprite pixels of 1 shown in shade 0: colour 0 of both sprite palettes, which a game's OBP can
	; show too.
	ld a, $E0
ENDC
	ldh [$FF48], a
	ldh [$FF49], a
	ld a, $93
	ldh [$FF40], a
.end
	jr .end

Shades:
	db $00, $00, $FF, $00, $00, $FF, $FF, $FF
Row:
	db 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3
	ds 12, 0
Sprites:
	; y, x, tile, attributes: palette 0 at x 20, 40, 60; palette 1 at x 100, 120, 140.
	db 116, 28, 1, $00
	db 116, 48, 2, $00
	db 116, 68, 3, $00
	db 116, 108, 1, $10
	db 116, 128, 2, $10
	db 116, 148, 3, $10
.end
