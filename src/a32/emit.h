/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_A32_EMIT_H
#define ARMGBA_A32_EMIT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// An ARMv7-A encoder for the A32 instruction set. Registers are numbers 0-15; only ARMv7
// instructions are used, so the code runs on Cortex-A7 and A9 as well as on ARMv8 in AArch32.

struct Emitter {
	uint8_t* start;
	uint8_t* p;
	uint8_t* end;
};

enum {
	R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC,
};

enum {
	C_EQ, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL,
};

// Data-processing opcodes, as in bits 24-21.
enum {
	D_AND, D_EOR, D_SUB, D_RSB, D_ADD, D_ADC, D_SBC, D_RSC, D_TST, D_TEQ, D_CMP, D_CMN, D_ORR, D_MOV, D_BIC, D_MVN,
};

enum {
	S_LSL, S_LSR, S_ASR, S_ROR,
};

// A32 instructions are 32 bits each, always with a condition.
// Past the end, nothing is written: p still advances, and the caller sees that the code didn't fit.
static inline void emit32(struct Emitter* e, uint32_t insn) {
	if (e->p + 4 <= e->end) {
		memcpy(e->p, &insn, 4);
	}
	e->p += 4;
}

// Emits one instruction and returns its address, for instructions that are patched later.
static inline uint8_t* emitSite(struct Emitter* e, uint32_t insn) {
	uint8_t* at = e->p;
	emit32(e, insn);
	return at;
}

// Fields that don't fit are compiler bugs.
static inline uint32_t field(uint32_t value, unsigned bits) {
	if (value >> bits) {
		abort();
	}
	return value;
}

static inline uint32_t cond(int c) {
	return (uint32_t) c << 28;
}

// Returns the rotate:imm8 field that encodes value as a data-processing constant, or -1 if it has none.
static inline int32_t immEncode(uint32_t value) {
	for (int rot = 0; rot < 16; ++rot) {
		uint32_t v = rot ? (value << (2 * rot)) | (value >> (32 - 2 * rot)) : value;
		if (v < 256) {
			return (rot << 8) | v;
		}
	}
	return -1;
}

// Data processing with a constant that encodes (see immEncode). rd or rn is unused by some opcodes.
static inline void aDpI(struct Emitter* e, int c, int op, bool s, int rd, int rn, uint32_t value) {
	int32_t imm = immEncode(value);
	if (imm < 0) {
		abort();
	}
	emit32(e, cond(c) | 0x02000000 | (uint32_t) op << 21 | (uint32_t) s << 20 | field(rn, 4) << 16 | field(rd, 4) << 12 | (uint32_t) imm);
}

// The same with the rotate:imm8 field given, for a constant whose rotation decides the carry.
static inline void aDpEnc(struct Emitter* e, int c, int op, bool s, int rd, int rn, uint32_t imm12) {
	emit32(e, cond(c) | 0x02000000 | (uint32_t) op << 21 | (uint32_t) s << 20 | field(rn, 4) << 16 | field(rd, 4) << 12 | field(imm12, 12));
}

// Data processing with a register operand shifted by a constant (amount 0-31; LSR and ASR by 32 are
// amount 0, ROR 0 is RRX).
static inline void aDpR(struct Emitter* e, int c, int op, bool s, int rd, int rn, int rm, int type, int amount) {
	emit32(e, cond(c) | (uint32_t) op << 21 | (uint32_t) s << 20 | field(rn, 4) << 16 | field(rd, 4) << 12 | field(amount, 5) << 7 |
	              field(type, 2) << 5 | field(rm, 4));
}

// Data processing with a register operand shifted by another register's low byte.
static inline void aDpRR(struct Emitter* e, int c, int op, bool s, int rd, int rn, int rm, int type, int rs) {
	emit32(e, cond(c) | (uint32_t) op << 21 | (uint32_t) s << 20 | field(rn, 4) << 16 | field(rd, 4) << 12 | field(rs, 4) << 8 |
	              field(type, 2) << 5 | 0x10 | field(rm, 4));
}

static inline void aMov(struct Emitter* e, int rd, int rm) {
	if (rd != rm) {
		aDpR(e, C_AL, D_MOV, false, rd, 0, rm, S_LSL, 0);
	}
}

// MOVW and MOVT: the low and high halves of a constant.
static inline void aMovw(struct Emitter* e, int c, int rd, uint32_t value) {
	emit32(e, cond(c) | 0x03000000 | (field(value, 16) >> 12) << 16 | field(rd, 4) << 12 | (value & 0xFFF));
}

static inline void aMovt(struct Emitter* e, int c, int rd, uint32_t value) {
	emit32(e, cond(c) | 0x03400000 | (field(value, 16) >> 12) << 16 | field(rd, 4) << 12 | (value & 0xFFF));
}

// Loads a 32-bit constant in one instruction where it encodes, otherwise MOVW and MOVT.
static inline void aMovI(struct Emitter* e, int rd, uint32_t value) {
	if (immEncode(value) >= 0) {
		aDpI(e, C_AL, D_MOV, false, rd, 0, value);
	} else if (immEncode(~value) >= 0) {
		aDpI(e, C_AL, D_MVN, false, rd, 0, ~value);
	} else {
		aMovw(e, C_AL, rd, value & 0xFFFF);
		if (value >> 16) {
			aMovt(e, C_AL, rd, value >> 16);
		}
	}
}

// rd = rn + value for any value, using tmp when it doesn't encode as one constant.
static inline void aAddI(struct Emitter* e, int rd, int rn, int32_t value, int tmp) {
	if (immEncode((uint32_t) value) >= 0) {
		aDpI(e, C_AL, D_ADD, false, rd, rn, (uint32_t) value);
	} else if (immEncode((uint32_t) -value) >= 0) {
		aDpI(e, C_AL, D_SUB, false, rd, rn, (uint32_t) -value);
	} else {
		aMovI(e, tmp, (uint32_t) value);
		aDpR(e, C_AL, D_ADD, false, rd, rn, tmp, S_LSL, 0);
	}
}

// Loads and stores of words and bytes at base plus or minus a 12-bit offset.
static inline void aMem(struct Emitter* e, int c, bool load, bool byte, int rt, int rn, int32_t offset) {
	uint32_t up = offset >= 0;
	uint32_t magnitude = up ? (uint32_t) offset : (uint32_t) -offset;
	emit32(e, cond(c) | 0x05000000 | up << 23 | (uint32_t) byte << 22 | (uint32_t) load << 20 | field(rn, 4) << 16 | field(rt, 4) << 12 |
	              field(magnitude, 12));
}

static inline void aLdr(struct Emitter* e, int rt, int rn, int32_t offset) {
	aMem(e, C_AL, true, false, rt, rn, offset);
}

static inline void aStr(struct Emitter* e, int rt, int rn, int32_t offset) {
	aMem(e, C_AL, false, false, rt, rn, offset);
}

static inline void aLdrb(struct Emitter* e, int rt, int rn, int32_t offset) {
	aMem(e, C_AL, true, true, rt, rn, offset);
}

static inline void aStrb(struct Emitter* e, int rt, int rn, int32_t offset) {
	aMem(e, C_AL, false, true, rt, rn, offset);
}

// Loads and stores of words and bytes at base plus a register, shifted left by a constant.
static inline void aMemR(struct Emitter* e, bool load, bool byte, int rt, int rn, int rm, int lsl) {
	emit32(e, cond(C_AL) | 0x07800000 | (uint32_t) byte << 22 | (uint32_t) load << 20 | field(rn, 4) << 16 | field(rt, 4) << 12 |
	              field(lsl, 5) << 7 | field(rm, 4));
}

// Halfword and signed loads and stores: the H/S bits (0xB0 halfword, 0xD0 signed byte, 0xF0 signed
// halfword) at base plus or minus an 8-bit offset.
enum {
	H_HALF = 0xB0, H_SBYTE = 0xD0, H_SHALF = 0xF0,
};

static inline void aMemH(struct Emitter* e, bool load, int kind, int rt, int rn, int32_t offset) {
	uint32_t up = offset >= 0;
	uint32_t magnitude = field(up ? (uint32_t) offset : (uint32_t) -offset, 8);
	emit32(e, cond(C_AL) | 0x01400000 | up << 23 | (uint32_t) load << 20 | field(rn, 4) << 16 | field(rt, 4) << 12 | (magnitude >> 4) << 8 |
	              (uint32_t) kind | (magnitude & 0xF));
}

// The same at base plus a register.
static inline void aMemHR(struct Emitter* e, bool load, int kind, int rt, int rn, int rm) {
	emit32(e, cond(C_AL) | 0x01800000 | (uint32_t) load << 20 | field(rn, 4) << 16 | field(rt, 4) << 12 | (uint32_t) kind | field(rm, 4));
}

// PUSH and POP of a register list.
static inline void aPush(struct Emitter* e, uint32_t list) {
	emit32(e, cond(C_AL) | 0x092D0000 | field(list, 16));
}

static inline void aPop(struct Emitter* e, uint32_t list) {
	emit32(e, cond(C_AL) | 0x08BD0000 | field(list, 16));
}

// LDMIA and STMIA from a base, without writeback.
static inline void aLdmStm(struct Emitter* e, bool load, int rn, uint32_t list) {
	emit32(e, cond(C_AL) | 0x08800000 | (uint32_t) load << 20 | field(rn, 4) << 16 | field(list, 16));
}

// Multiplies. The S variants set N and Z.
static inline void aMul(struct Emitter* e, bool s, int rd, int rm, int rs) {
	emit32(e, cond(C_AL) | 0x00000090 | (uint32_t) s << 20 | field(rd, 4) << 16 | field(rs, 4) << 8 | field(rm, 4));
}

static inline void aMla(struct Emitter* e, bool s, int rd, int rm, int rs, int ra) {
	emit32(e, cond(C_AL) | 0x00200090 | (uint32_t) s << 20 | field(rd, 4) << 16 | field(ra, 4) << 12 | field(rs, 4) << 8 | field(rm, 4));
}

// The long multiplies: op is 0x00800090 UMULL, 0x00C00090 SMULL, 0x00A00090 UMLAL, 0x00E00090 SMLAL.
enum {
	M_UMULL = 0x00800090, M_SMULL = 0x00C00090, M_UMLAL = 0x00A00090, M_SMLAL = 0x00E00090,
};

static inline void aMull(struct Emitter* e, uint32_t op, bool s, int lo, int hi, int rm, int rs) {
	emit32(e, cond(C_AL) | op | (uint32_t) s << 20 | field(hi, 4) << 16 | field(lo, 4) << 12 | field(rs, 4) << 8 | field(rm, 4));
}

// Moves the flags to and from a register: MRS rd, APSR and MSR APSR_nzcvq, rm.
static inline void aMrs(struct Emitter* e, int rd) {
	emit32(e, cond(C_AL) | 0x010F0000 | field(rd, 4) << 12);
}

static inline void aMsr(struct Emitter* e, int rm) {
	emit32(e, cond(C_AL) | 0x0128F000 | field(rm, 4));
}

static inline void aClz(struct Emitter* e, int rd, int rm) {
	emit32(e, cond(C_AL) | 0x016F0F10 | field(rd, 4) << 12 | field(rm, 4));
}

// Zero and sign extensions of a byte or halfword: op is 0x06EF0070 UXTB, 0x06FF0070 UXTH, 0x06AF0070
// SXTB, 0x06BF0070 SXTH.
enum {
	X_UXTB = 0x06EF0070, X_UXTH = 0x06FF0070, X_SXTB = 0x06AF0070, X_SXTH = 0x06BF0070,
};

static inline void aExtend(struct Emitter* e, uint32_t op, int rd, int rm) {
	emit32(e, cond(C_AL) | op | field(rd, 4) << 12 | field(rm, 4));
}

// UBFX: width bits of rn from lsb into rd's low bits.
static inline void aUbfx(struct Emitter* e, int rd, int rn, int lsb, int width) {
	emit32(e, cond(C_AL) | 0x07E00050 | field(width - 1, 5) << 16 | field(rd, 4) << 12 | field(lsb, 5) << 7 | field(rn, 4));
}

// BFI: rn's low width bits into rd at lsb.
static inline void aBfi(struct Emitter* e, int rd, int rn, int lsb, int width) {
	emit32(e, cond(C_AL) | 0x07C00010 | field(lsb + width - 1, 5) << 16 | field(rd, 4) << 12 | field(lsb, 5) << 7 | field(rn, 4));
}

// Branches. B and BL reach 32 MiB either way; their target is relative to the instruction plus 8.
static inline uint32_t branchField(const uint8_t* site, const void* target) {
	int32_t delta = (int32_t) ((const uint8_t*) target - site - 8);
	if ((delta & 3) || delta < -(1 << 25) || delta >= (1 << 25)) {
		abort();
	}
	return ((uint32_t) delta >> 2) & 0xFFFFFF;
}

// Emits a branch; with a NULL target it is patched later. Returns its site.
static inline uint8_t* aB(struct Emitter* e, int c, const void* target) {
	uint8_t* at = e->p;
	emit32(e, cond(c) | 0x0A000000 | (target ? branchField(at, target) : 0));
	return at;
}

static inline uint8_t* aBl(struct Emitter* e, const void* target) {
	uint8_t* at = e->p;
	emit32(e, cond(C_AL) | 0x0B000000 | (target ? branchField(at, target) : 0));
	return at;
}

static inline void aBx(struct Emitter* e, int rm) {
	emit32(e, cond(C_AL) | 0x012FFF10 | field(rm, 4));
}

static inline void aBlx(struct Emitter* e, int rm) {
	emit32(e, cond(C_AL) | 0x012FFF30 | field(rm, 4));
}

// Rewrites the B or BL at a site to go to target, keeping its condition and kind.
static inline void patchBranch(uint8_t* site, const void* target) {
	uint32_t insn;
	memcpy(&insn, site, 4);
	insn = (insn & 0xFF000000) | branchField(site, target);
	memcpy(site, &insn, 4);
}

// Points a forward branch at the code about to be emitted.
static inline void patchHere(struct Emitter* e, uint8_t* site) {
	if (site + 4 <= e->end) {
		patchBranch(site, e->p);
	}
}

#endif
