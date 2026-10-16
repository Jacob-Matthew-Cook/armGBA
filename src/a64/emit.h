/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_A64_EMIT_H
#define ARMGBA_A64_EMIT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// An AArch64 instruction encoder. Registers are numbers; 31 is wzr or sp by context.

struct Emitter {
	uint8_t* start;
	uint8_t* p;
	uint8_t* end;
};

enum {
	ZR = 31,
	SP = 31,
};

enum {
	C_EQ, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL,
};

// Data processing (shifted register) on w registers; OR in W64 for x registers.
enum {
	W64 = 0x80000000,
	A_AND = 0x0A000000, A_BIC = 0x0A200000, A_ORR = 0x2A000000, A_ORN = 0x2A200000, A_EOR = 0x4A000000,
	A_ANDS = 0x6A000000, A_BICS = 0x6A200000,
	A_ADD = 0x0B000000, A_ADDS = 0x2B000000, A_SUB = 0x4B000000, A_SUBS = 0x6B000000,
	A_ADC = 0x1A000000, A_ADCS = 0x3A000000, A_SBC = 0x5A000000, A_SBCS = 0x7A000000,
	A_LSLV = 0x1AC02000, A_LSRV = 0x1AC02400, A_ASRV = 0x1AC02800, A_RORV = 0x1AC02C00,
};

enum {
	S_LSL, S_LSR, S_ASR, S_ROR,
};

// Add and subtract immediates (12 bits, optionally shifted left by 12).
enum {
	A_ADD_IMM = 0x11000000, A_ADDS_IMM = 0x31000000, A_SUB_IMM = 0x51000000, A_SUBS_IMM = 0x71000000,
};

// Register-offset loads and stores: [xn, wm UXTW] or [xn, xm].
enum {
	L_LDRB = 0x38600800, L_LDRH = 0x78600800, L_LDR = 0xB8600800, L_LDRX = 0xF8600800,
	L_LDRSB = 0x38E00800, L_LDRSH = 0x78E00800,
	L_STRB = 0x38200800, L_STRH = 0x78200800, L_STR = 0xB8200800, L_STRX = 0xF8200800,
	L_UXTW = 0x4000, L_LSL = 0x6000,
};

// AArch64 instructions are 32 bits each.
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

// Data processing with two registers, the second optionally shifted; op is the opcode with its
// register fields empty.
static inline void aDp(struct Emitter* e, uint32_t op, int rd, int rn, int rm, int shift, unsigned amount) {
	emit32(e, op | (shift << 22) | (rm << 16) | (field(amount, 6) << 10) | (rn << 5) | rd);
}

// Data processing with a 12-bit constant.
static inline void aDpImm(struct Emitter* e, uint32_t op, int rd, int rn, unsigned imm) {
	if (imm >> 12) {
		if (imm & 0xFFF) {
			abort();
		}
		emit32(e, op | 0x00400000 | (field(imm >> 12, 12) << 10) | (rn << 5) | rd);
		return;
	}
	emit32(e, op | (imm << 10) | (rn << 5) | rd);
}

// MOV, 32 bits
static inline void aMov(struct Emitter* e, int rd, int rm) {
	aDp(e, A_ORR, rd, ZR, rm, S_LSL, 0);
}

// MOV, 64 bits
static inline void aMovX(struct Emitter* e, int rd, int rm) {
	if (rd == SP || rm == SP) {
		emit32(e, 0x91000000 | (rm << 5) | rd);
		return;
	}
	aDp(e, A_ORR | W64, rd, ZR, rm, S_LSL, 0);
}

// MVN
static inline void aMvn(struct Emitter* e, int rd, int rm) {
	aDp(e, A_ORN, rd, ZR, rm, S_LSL, 0);
}

// Loads a 32-bit constant in as few MOVZ/MOVN/MOVK instructions as it takes.
static inline void aMovImm(struct Emitter* e, int rd, uint32_t value) {
	if (!(value & 0xFFFF) && value) {
		emit32(e, 0x52A00000 | ((value >> 16) << 5) | rd);
	} else if ((value >> 16) == 0xFFFF) {
		emit32(e, 0x12800000 | ((~value & 0xFFFF) << 5) | rd);
	} else {
		emit32(e, 0x52800000 | ((value & 0xFFFF) << 5) | rd);
		if (value >> 16) {
			emit32(e, 0x72A00000 | ((value >> 16) << 5) | rd);
		}
	}
}

// Loads a 64-bit constant, 16 bits at a time.
static inline void aMovImm64(struct Emitter* e, int rd, uint64_t value) {
	emit32(e, 0xD2800000 | ((value & 0xFFFF) << 5) | rd);
	for (int hw = 1; hw < 4; ++hw) {
		uint32_t part = (value >> (16 * hw)) & 0xFFFF;
		if (part) {
			emit32(e, 0xF2800000 | (hw << 21) | (part << 5) | rd);
		}
	}
}

// rd = rn + value for any 32-bit value, using tmp if it doesn't fit an immediate.
static inline void aAddImm(struct Emitter* e, int rd, int rn, int32_t value, int tmp) {
	uint32_t magnitude = value < 0 ? -(uint32_t) value : (uint32_t) value;
	uint32_t op = value < 0 ? A_SUB_IMM : A_ADD_IMM;
	if (magnitude < 0x1000) {
		aDpImm(e, op, rd, rn, magnitude);
	} else if (!(magnitude & 0xFFF) && magnitude < 0x1000000) {
		aDpImm(e, op, rd, rn, magnitude);
	} else if (magnitude < 0x1000000) {
		aDpImm(e, op, rd, rn, magnitude & 0xFFF000);
		aDpImm(e, op, rd, rd, magnitude & 0xFFF);
	} else {
		aMovImm(e, tmp, value);
		aDp(e, A_ADD, rd, rn, tmp, S_LSL, 0);
	}
}

// A 64-bit add of a constant, for addresses in struct GBA.
static inline void aAddImmX(struct Emitter* e, int rd, int rn, uint32_t value) {
	if (value < 0x1000) {
		aDpImm(e, A_ADD_IMM | W64, rd, rn, value);
	} else {
		aDpImm(e, A_ADD_IMM | W64, rd, rn, value & 0xFFF000);
		if (value & 0xFFF) {
			aDpImm(e, A_ADD_IMM | W64, rd, rd, value & 0xFFF);
		}
	}
}

// Logical immediates: AND, ORR, EOR and ANDS with a bitmask constant.
enum {
	A_AND_IMM = 0x12000000, A_ORR_IMM = 0x32000000, A_EOR_IMM = 0x52000000, A_ANDS_IMM = 0x72000000,
};

// Returns the immr:imms fields for a 32-bit bitmask immediate, or -1 if the value has no encoding.
static inline int logicalImm(uint32_t value) {
	if (!value || value == ~0u) {
		return -1;
	}
	unsigned size = 32;
	while (size > 2) {
		unsigned half = size / 2;
		uint32_t mask = (1u << half) - 1;
		if ((value & mask) != ((value >> half) & mask)) {
			break;
		}
		size = half;
	}
	uint32_t mask = size == 32 ? ~0u : (1u << size) - 1;
	uint32_t elem = value & mask;
	unsigned ones = __builtin_popcount(elem);
	uint32_t run = ones == 32 ? ~0u : (1u << ones) - 1;
	for (unsigned r = 0; r < size; ++r) {
		uint32_t rotated = r ? ((run >> r) | (run << (size - r))) & mask : run;
		if (rotated == elem) {
			unsigned imms = ((~(size - 1) << 1) | (ones - 1)) & 0x3F;
			return (r << 6) | imms;
		}
	}
	return -1;
}

// Emits AND, ORR or EOR with a constant if AArch64 can encode it; returns false if not.
static inline bool aLogicImm(struct Emitter* e, uint32_t op, int rd, int rn, uint32_t value) {
	int enc = logicalImm(value);
	if (enc < 0) {
		return false;
	}
	emit32(e, op | ((enc >> 6) << 16) | ((enc & 0x3F) << 10) | (rn << 5) | rd);
	return true;
}

// Bitfields.
static inline void aUbfx(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned width) {
	emit32(e, 0x53000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd);
}

// BFI: rn's low bits into a field of rd.
static inline void aBfi(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned width) {
	emit32(e, 0x33000000 | (((32 - lsb) & 31) << 16) | ((width - 1) << 10) | (rn << 5) | rd);
}

// BFXIL: a field of rn into rd's low bits.
static inline void aBfxil(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned width) {
	emit32(e, 0x33000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd);
}

// rd = rn & (len ones starting at lsb)
static inline void aAndBits(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned len) {
	emit32(e, 0x12000000 | (((32 - lsb) & 31) << 16) | ((len - 1) << 10) | (rn << 5) | rd);
}

// The shifts and the rotate by a constant, 32 bits.
static inline void aLslImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	emit32(e, 0x53000000 | (((32 - shift) & 31) << 16) | ((31 - shift) << 10) | (rn << 5) | rd);
}

static inline void aLsrImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	emit32(e, 0x53007C00 | (shift << 16) | (rn << 5) | rd);
}

static inline void aAsrImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	emit32(e, 0x13007C00 | (shift << 16) | (rn << 5) | rd);
}

static inline void aRorImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	emit32(e, 0x13800000 | (rn << 16) | (shift << 10) | (rn << 5) | rd);
}

// Sign and zero extensions.
static inline void aSxtb(struct Emitter* e, int rd, int rn) {
	emit32(e, 0x13001C00 | (rn << 5) | rd);
}

static inline void aSxth(struct Emitter* e, int rd, int rn) {
	emit32(e, 0x13003C00 | (rn << 5) | rd);
}

static inline void aUxth(struct Emitter* e, int rd, int rn) {
	emit32(e, 0x53003C00 | (rn << 5) | rd);
}

// xd = xn + (xm << 32)
static inline void aAddHigh(struct Emitter* e, int rd, int rn, int rm) {
	aDp(e, A_ADD | W64, rd, rn, rm, S_LSL, 32);
}

// LSR by a constant, 64 bits.
static inline void aLsrImmX(struct Emitter* e, int rd, int rn, unsigned shift) {
	emit32(e, 0xD340FC00 | (shift << 16) | (rn << 5) | rd);
}

// MADD: ra + rn * rm
static inline void aMadd(struct Emitter* e, int rd, int rn, int rm, int ra) {
	emit32(e, 0x1B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

// xd = wn * wm, 64-bit
static inline void aSmull(struct Emitter* e, int rd, int rn, int rm) {
	emit32(e, 0x9B207C00 | (rm << 16) | (rn << 5) | rd);
}

// A 32x32 multiply to 64 bits, unsigned.
static inline void aUmull(struct Emitter* e, int rd, int rn, int rm) {
	emit32(e, 0x9BA07C00 | (rm << 16) | (rn << 5) | rd);
}

// UDIV
static inline void aUdiv(struct Emitter* e, int rd, int rn, int rm) {
	emit32(e, 0x1AC00800 | (rm << 16) | (rn << 5) | rd);
}

// CLZ
static inline void aClz(struct Emitter* e, int rd, int rn) {
	emit32(e, 0x5AC01000 | (rn << 5) | rd);
}

// CSEL: rn if the condition holds, else rm.
static inline void aCsel(struct Emitter* e, int rd, int rn, int rm, int cond) {
	emit32(e, 0x1A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

// rd = cond ? 1 : 0
static inline void aCset(struct Emitter* e, int rd, int cond) {
	emit32(e, 0x1A9F07E0 | ((cond ^ 1) << 12) | rd);
}

// Moves the host's flags to and from a register.
static inline void aMrsNzcv(struct Emitter* e, int rt) {
	emit32(e, 0xD53B4200 | rt);
}

static inline void aMsrNzcv(struct Emitter* e, int rt) {
	emit32(e, 0xD51B4200 | rt);
}

// Loads and stores with unsigned scaled offsets.
static inline uint32_t scaled(unsigned offset, unsigned scale) {
	if (offset & ((1 << scale) - 1)) {
		abort();
	}
	return field(offset >> scale, 12) << 10;
}

// Loads and stores at an unsigned offset from a base register, which the encoding scales by the
// size.
static inline void aLdr(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0xB9400000 | scaled(offset, 2) | (rn << 5) | rt);
}

static inline void aStr(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0xB9000000 | scaled(offset, 2) | (rn << 5) | rt);
}

static inline void aLdrh(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0x79400000 | scaled(offset, 1) | (rn << 5) | rt);
}

static inline void aStrh(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0x79000000 | scaled(offset, 1) | (rn << 5) | rt);
}

static inline void aLdrb(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0x39400000 | scaled(offset, 0) | (rn << 5) | rt);
}

static inline void aStrb(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0x39000000 | scaled(offset, 0) | (rn << 5) | rt);
}

// The 64-bit ones.
static inline void aLdrX(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0xF9400000 | scaled(offset, 3) | (rn << 5) | rt);
}

static inline void aStrX(struct Emitter* e, int rt, int rn, unsigned offset) {
	emit32(e, 0xF9000000 | scaled(offset, 3) | (rn << 5) | rt);
}

// A load or store at base plus a register, with the register extended as given.
static inline void aLdst(struct Emitter* e, uint32_t op, int rt, int rn, int rm, uint32_t extend) {
	emit32(e, op | extend | (rm << 16) | (rn << 5) | rt);
}

// stp/ldp of x registers at [sp, #offset], and with pre/post-index writeback.
static inline void aStp(struct Emitter* e, int rt, int rt2, int offset) {
	emit32(e, 0xA9000000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (SP << 5) | rt);
}

// Loads a pair of 64-bit registers from the stack.
static inline void aLdp(struct Emitter* e, int rt, int rt2, int offset) {
	emit32(e, 0xA9400000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (SP << 5) | rt);
}

// Pushes a pair, making room for a frame of bytes; aPop undoes it.
static inline void aPush(struct Emitter* e, int rt, int rt2, int bytes) {
	emit32(e, 0xA9800000 | (((-bytes / 8) & 0x7F) << 15) | (rt2 << 10) | (SP << 5) | rt);
}

static inline void aPop(struct Emitter* e, int rt, int rt2, int bytes) {
	emit32(e, 0xA8C00000 | ((bytes / 8) << 15) | (rt2 << 10) | (SP << 5) | rt);
}

// ldp/stp of w registers at [xn, #offset].
static inline void aLdpW(struct Emitter* e, int rt, int rt2, int rn, int offset) {
	emit32(e, 0x29400000 | (((offset / 4) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt);
}

// Stores a pair of 32-bit registers to memory.
static inline void aStpW(struct Emitter* e, int rt, int rt2, int rn, int offset) {
	emit32(e, 0x29000000 | (((offset / 4) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt);
}

// LSL by a constant, 64 bits.
static inline void aLslImmX(struct Emitter* e, int rd, int rn, unsigned shift) {
	emit32(e, 0xD3400000 | (((64 - shift) & 63) << 16) | ((63 - shift) << 10) | (rn << 5) | rd);
}

// Branches.
static inline void aBlr(struct Emitter* e, int rn) {
	emit32(e, 0xD63F0000 | (rn << 5));
}

// The indirect branches.
static inline void aBr(struct Emitter* e, int rn) {
	emit32(e, 0xD61F0000 | (rn << 5));
}

static inline void aRet(struct Emitter* e) {
	emit32(e, 0xD65F03C0);
}

// Rewrites the branch at a site to go to target, whichever kind of branch it is.
static inline void patchBranch(uint8_t* at, const void* target) {
	int64_t offset = ((const uint8_t*) target - at) / 4;
	uint32_t insn;
	memcpy(&insn, at, 4);
	unsigned bits = (insn & 0x7C000000) == 0x14000000 ? 26 : (insn & 0x7E000000) == 0x36000000 ? 14 : 19;
	int64_t limit = 1ll << (bits - 1);
	if (offset < -limit || offset >= limit) {
		abort();
	}
	uint32_t f = (uint32_t) offset & ((1u << bits) - 1);
	if (bits == 26) {
		insn = (insn & 0xFC000000) | f;
	} else if (bits == 14) {
		insn = (insn & ~(0x3FFFu << 5)) | (f << 5);
	} else {
		insn = (insn & ~(0x7FFFFu << 5)) | (f << 5);
	}
	memcpy(at, &insn, 4);
}

// Points a forward branch at the code about to be emitted.
static inline void patchHere(struct Emitter* e, uint8_t* site) {
	if (site + 4 <= e->end) {
		patchBranch(site, e->p);
	}
}

// The branches. Each returns its site; with a NULL target, it is patched later.
static inline uint8_t* aB(struct Emitter* e, const void* target) {
	uint8_t* site = emitSite(e, 0x14000000);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

static inline uint8_t* aBl(struct Emitter* e, const void* target) {
	uint8_t* site = emitSite(e, 0x94000000);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

static inline uint8_t* aBcond(struct Emitter* e, int cond, const void* target) {
	uint8_t* site = emitSite(e, 0x54000000 | cond);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

// Compare with zero and branch, and test a bit and branch.
static inline uint8_t* aCbz(struct Emitter* e, int rt, const void* target) {
	uint8_t* site = emitSite(e, 0x34000000 | rt);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

static inline uint8_t* aCbnz(struct Emitter* e, int rt, const void* target) {
	uint8_t* site = emitSite(e, 0x35000000 | rt);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

static inline uint8_t* aCbnzX(struct Emitter* e, int rt, const void* target) {
	uint8_t* site = emitSite(e, 0xB5000000 | rt);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

static inline uint8_t* aTbz(struct Emitter* e, int rt, unsigned bit, const void* target) {
	uint8_t* site = emitSite(e, 0x36000000 | ((bit & 31) << 19) | rt);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

static inline uint8_t* aTbnz(struct Emitter* e, int rt, unsigned bit, const void* target) {
	uint8_t* site = emitSite(e, 0x37000000 | ((bit & 31) << 19) | rt);
	if (target && site + 4 <= e->end) {
		patchBranch(site, target);
	}
	return site;
}

#endif
