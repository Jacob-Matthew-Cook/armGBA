; Written by cgbdata.py.
Art:
	db $00, $00, $37, $77, $00, $88, $FF, $CC, $EF, $33, $67, $77, $00, $00, $6E, $EE
	db $7F, $CC, $FF, $11, $3B, $BB, $FF, $00, $8C, $CC, $7F, $CC, $FF, $11, $08, $88
	db $00, $01, $DC, $CF, $8C, $CC, $CF, $FC, $3F, $E3, $76, $66, $99, $FF, $E6, $66
	db $CC, $CC, $07, $71, $3B, $BB, $FF, $00, $CC, $66, $CC, $CC, $00, $01, $00, $08
	db $11, $30, $F8, $00, $E6, $60, $CC, $C0, $33, $30, $66, $60, $60, $00, $66, $60
	db $CF, $70, $1F, $F0, $BB, $30, $0F, $F0, $6E, $C0, $CF, $70, $1F, $F0, $88, $00
; BG palettes 1 to 5, colour 3: the colours the columns come in through, then the title's blue.
TitleColours:
	dw $03FF, $001F, $7C1F, $03E0, $7C00
; Colour sets: white, two colours and black as just the two; then the others whole.
StandardSets:
	dw $03EF, $01D6
	dw $03FF, $012F
	dw $1BEF, $6180
	dw $32BF, $00D0
	dw $6E31, $454A
	dw $03FF, $001F
	dw $421F, $1CF2
	dw $1BEF, $0200
	dw $027F, $001F
	dw $7E8C, $7C00
	dw $03EA, $011F
	dw $5294, $294A
	dw $42B5, $3DC8
	dw $033F, $0193
	dw $01DF, $0112
OtherSets:
	dw $7E74, $03FF, $0180, $0000
	dw $0000, $4200, $037F, $7FFF
	dw $036A, $021F, $03FF, $7FFF
	dw $7ED6, $4BFF, $2175, $0000
	dw $4FFF, $7ED2, $3A4C, $1CE0
	dw $03ED, $7FFF, $255F, $0000
	dw $67FF, $77AC, $1A13, $2D6B
	dw $639F, $4279, $15B0, $04CB
	dw $53FF, $4A5F, $7E52, $0000
	dw $231F, $035F, $00F2, $0009
	dw $7FFF, $7EEB, $001F, $7C00
	dw $299F, $001A, $000C, $0000
	dw $7C00, $7FFF, $3FFF, $7E00
	dw $7FFF, $7FFF, $7E8C, $7C00
	dw $0000, $7FFF, $421F, $1CF2
	dw $03FF, $001F, $000C, $0000
	dw $7FFF, $3FFF, $7E00, $001F
	dw $7FFF, $03E0, $0206, $0120
; Per combination: the BG set in bits 0-4, the sprite sets less 1 in 5-9 and 10-14.
Combos:
	dw $14A2
	dw $1903
	dw $0843
	dw $5EE4
	dw $6085
	dw $14C6
	dw $1085
	dw $20A7
	dw $62E4
	dw $14A8
	dw $672F
	dw $6B2F
	dw $3DF0
	dw $14A7
	dw $14A0
	dw $1771
	dw $2103
	dw $1509
	dw $14AA
	dw $60E8
	dw $7392
	dw $1BA9
	dw $78A9
	dw $294B
	dw $61AC
	dw $20A9
	dw $318D
	dw $23E6
	dw $1CE8
	dw $16F3
	dw $39CF
	dw $2DAC
	dw $612A
	dw $5C64
	dw $08A4
	dw $20C3
	dw $18C3
	dw $04A2
	dw $35AC
	dw $14A4
	dw $0B74
	dw $21B5
	dw $204C
	dw $0CA4
	dw $20A2
	dw $20C6
	dw $0856
	dw $18A9
	dw $5AD7
	dw $1901
	dw $252A
; Title checksum and fourth letter, then the combination and waits; then by the checksum alone.
Lettered:
	db $0D, $45, $9B, $1F, $30
	db $0D, $52, $F4, $24, $1C
	db $18, $4B, $D1, $46, $30
	db $27, $42, $CB, $5F, $20
	db $27, $4E, $D6, $3C, $38
	db $28, $41, $BA, $63, $4C
	db $28, $46, $8C, $6F, $38
	db $46, $45, $82, $A6, $28
	db $46, $52, $CB, $AE, $50
	db $61, $41, $D8, $3E, $38
	db $61, $45, $AE, $CD, $2C
	db $66, $45, $98, $77, $10
	db $6A, $49, $9D, $97, $14
	db $6A, $4B, $D4, $43, $30
	db $A5, $41, $B9, $67, $4C
	db $A5, $52, $91, $27, $C8
	db $B3, $42, $C0, $5A, $20
	db $B3, $52, $E6, $04, $94
	db $B3, $55, $8B, $31, $80
	db $BF, $20, $9B, $3C, $B4
	db $BF, $43, $D9, $43, $88
	db $C6, $41, $CD, $C7, $00
	db $D3, $49, $D4, $52, $D4
	db $D3, $52, $AA, $59, $B4
	db $F4, $20, $A2, $76, $10
	db $F4, $2D, $DF, $66, $F0
	db $18, $49, $99, $03, $70
	db $66, $4C, $9B, $05, $70
	db $A5, $4F, $92, $01, $70
	db $C6, $20, $92, $02, $70
Plain:
	db $01, $B5, $0E, $3C
	db $0C, $9C, $15, $48
	db $10, $92, $09, $3C
	db $14, $56, $2F, $40
	db $15, $7B, $32, $1C
	db $16, $4D, $14, $48
	db $17, $91, $3E, $38
	db $19, $37, $4E, $18
	db $1D, $29, $51, $20
	db $29, $AA, $09, $3C
	db $34, $44, $71, $10
	db $35, $6C, $15, $48
	db $36, $56, $7B, $0C
	db $39, $54, $85, $3C
	db $3C, $3C, $8D, $2C
	db $3D, $27, $92, $14
	db $3E, $85, $99, $18
	db $43, $5E, $83, $43
	db $49, $93, $5D, $20
	db $4B, $4D, $6C, $38
	db $4E, $9A, $B4, $2C
	db $52, $B3, $0C, $3C
	db $58, $5E, $BB, $5A
	db $59, $71, $C5, $00
	db $5C, $61, $5B, $20
	db $5D, $BD, $0A, $3C
	db $67, $B4, $15, $48
	db $68, $A0, $0B, $3C
	db $69, $8C, $22, $1C
	db $6B, $BF, $46, $30
	db $6D, $BF, $0C, $3C
	db $6F, $7A, $D7, $6C
	db $70, $6D, $D9, $44
	db $71, $AB, $E2, $18
	db $75, $73, $16, $48
	db $86, $AA, $EB, $04
	db $88, $43, $F3, $20
	db $8B, $A4, $3F, $38
	db $8C, $3E, $FF, $00
	db $90, $4F, $6E, $38
	db $92, $59, $14, $48
	db $95, $98, $02, $94
	db $97, $4B, $82, $3C
	db $99, $76, $13, $48
	db $9A, $71, $6C, $38
	db $9C, $91, $0F, $B0
	db $9D, $B6, $11, $B4
	db $A2, $99, $1A, $C8
	db $A8, $75, $EC, $04
	db $AA, $57, $2A, $F0
	db $B7, $A1, $14, $48
	db $BD, $7B, $6A, $38
	db $C9, $6A, $4C, $A4
	db $CE, $A2, $47, $88
	db $D1, $58, $45, $88
	db $DB, $52, $33, $1C
	db $E0, $B9, $9D, $18
	db $E8, $97, $63, $4C
	db $F0, $A0, $45, $88
	db $F2, $78, $26, $1C
	db $F6, $98, $0F, $3C
	db $F7, $96, $1D, $C8
	db $FF, $7D, $E4, $18
	db $00, $16, $03, $70
	db $3F, $82, $03, $70
	db $0D, $A2, $05, $70
	db $18, $9B, $04, $70
	db $27, $98, $07, $70
	db $28, $91, $06, $70
	db $46, $8F, $04, $70
	db $61, $99, $02, $70
	db $66, $9D, $06, $70
	db $6A, $9E, $01, $70
	db $A5, $92, $01, $70
	db $B3, $91, $07, $70
	db $BF, $A0, $03, $70
	db $C6, $94, $03, $70
	db $D3, $96, $05, $70
	db $F4, $A4, $07, $70
; Per direction (right, left, up, down as the joypad bits): none, A, B, and A and B as A.
ButtonCombos:
	db 50, 0, 12, 0
	db 47, 34, 23, 34
	db 2, 45, 46, 45
	db 48, 6, 49, 6
