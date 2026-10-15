/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_X64_EMIT_H
#define ARMGBA_X64_EMIT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
	RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
	R8, R9, R10, R11, R12, R13, R14, R15,
};

enum {
	CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A,
	CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G,
};

struct Emitter {
	uint8_t* start;
	uint8_t* p;
	uint8_t* end;
};

// A register or a memory operand [base + index * scale + disp]
struct Opnd {
	bool mem;
	int reg;
	int base;
	int index;
	int scale;
	int32_t disp;
};

// An operand is a register, or memory at a base register plus a displacement, with an optional
// scaled index register.
static inline struct Opnd R(int reg) {
	return (struct Opnd) { .mem = false, .reg = reg };
}

static inline struct Opnd M(int base, int32_t disp) {
	return (struct Opnd) { .mem = true, .base = base, .index = -1, .disp = disp };
}

static inline struct Opnd MI(int base, int index, int scale, int32_t disp) {
	return (struct Opnd) { .mem = true, .base = base, .index = index, .scale = scale, .disp = disp };
}

// Writes bytes, and 32-bit and 64-bit values, into the code stream.
static inline void e8(struct Emitter* e, uint8_t b) {
	*e->p++ = b;
}

static inline void e32(struct Emitter* e, uint32_t v) {
	memcpy(e->p, &v, 4);
	e->p += 4;
}

static inline void e64(struct Emitter* e, uint64_t v) {
	memcpy(e->p, &v, 8);
	e->p += 8;
}

// Width: 1, 2, 4 or 8 bytes. Byte operations on registers 4-7 need an empty REX prefix.
static inline void emitRex(struct Emitter* e, int width, int reg, struct Opnd rm, bool byteRegs) {
	int rex = 0;
	if (width == 8) {
		rex |= 8;
	}
	if (reg & 8) {
		rex |= 4;
	}
	if (rm.mem) {
		if (rm.index >= 0 && (rm.index & 8)) {
			rex |= 2;
		}
		if (rm.base & 8) {
			rex |= 1;
		}
	} else if (rm.reg & 8) {
		rex |= 1;
	}
	bool force = byteRegs && width == 1 && ((reg >= 4 && reg < 8) || (!rm.mem && rm.reg >= 4 && rm.reg < 8));
	if (rex || force) {
		e8(e, 0x40 | rex);
	}
}

// Writes the ModRM byte and what follows it: the SIB byte and displacement, in the shortest form
// that fits.
static inline void emitModrm(struct Emitter* e, int reg, struct Opnd rm) {
	reg &= 7;
	if (!rm.mem) {
		e8(e, 0xC0 | (reg << 3) | (rm.reg & 7));
		return;
	}
	int base = rm.base & 7;
	bool sib = rm.index >= 0 || base == RSP;
	int mod;
	if (rm.disp == 0 && base != RBP) {
		mod = 0;
	} else if (rm.disp >= -128 && rm.disp <= 127) {
		mod = 1;
	} else {
		mod = 2;
	}
	if (sib) {
		e8(e, (mod << 6) | (reg << 3) | 4);
		int scale = rm.scale == 8 ? 3 : rm.scale == 4 ? 2 : rm.scale == 2 ? 1 : 0;
		int index = rm.index >= 0 ? (rm.index & 7) : 4;
		e8(e, (scale << 6) | (index << 3) | base);
	} else {
		e8(e, (mod << 6) | (reg << 3) | base);
	}
	if (mod == 1) {
		e8(e, (uint8_t) rm.disp);
	} else if (mod == 2) {
		e32(e, rm.disp);
	}
}

// An opcode with a ModRM: [66] [REX] op... modrm
static inline void emitOp(struct Emitter* e, int width, const uint8_t* op, int opLen, int reg, struct Opnd rm) {
	if (width == 2) {
		e8(e, 0x66);
	}
	emitRex(e, width, reg, rm, true);
	for (int i = 0; i < opLen; ++i) {
		e8(e, op[i]);
	}
	emitModrm(e, reg, rm);
}

static inline void emitOp1(struct Emitter* e, int width, uint8_t op, int reg, struct Opnd rm) {
	emitOp(e, width, &op, 1, reg, rm);
}

static inline void emitOp2(struct Emitter* e, int width, uint8_t op0, uint8_t op1, int reg, struct Opnd rm) {
	uint8_t op[2] = { op0, op1 };
	emitOp(e, width, op, 2, reg, rm);
}

enum AluX {
	X_ADD = 0,
	X_OR = 1,
	X_ADC = 2,
	X_SBB = 3,
	X_AND = 4,
	X_SUB = 5,
	X_XOR = 6,
	X_CMP = 7,
};

// alu dst, src: dst is a register or memory, src a register.
static inline void xAluRR(struct Emitter* e, int alu, int width, struct Opnd dst, int src) {
	emitOp1(e, width, (alu << 3) | (width == 1 ? 0 : 1), src, dst);
}

// alu reg, [mem]
static inline void xAluRM(struct Emitter* e, int alu, int width, int dst, struct Opnd src) {
	emitOp1(e, width, (alu << 3) | (width == 1 ? 2 : 3), dst, src);
}

static inline void xAluI(struct Emitter* e, int alu, int width, struct Opnd dst, int32_t imm) {
	if (width == 1) {
		emitOp1(e, 1, 0x80, alu, dst);
		e8(e, imm);
	} else if (imm >= -128 && imm <= 127) {
		emitOp1(e, width, 0x83, alu, dst);
		e8(e, imm);
	} else {
		emitOp1(e, width, 0x81, alu, dst);
		if (width == 2) {
			e8(e, imm);
			e8(e, imm >> 8);
		} else {
			e32(e, imm);
		}
	}
}

// Register to register; nothing is emitted for a register to itself.
static inline void xMovRR(struct Emitter* e, int width, int dst, int src) {
	if (dst == src && width != 4) {
		return;
	}
	emitOp1(e, width, width == 1 ? 0x88 : 0x89, src, R(dst));
}

// Memory to register, and register to memory.
static inline void xLoad(struct Emitter* e, int width, int dst, struct Opnd src) {
	emitOp1(e, width, width == 1 ? 0x8A : 0x8B, dst, src);
}

static inline void xStore(struct Emitter* e, int width, struct Opnd dst, int src) {
	emitOp1(e, width, width == 1 ? 0x88 : 0x89, src, dst);
}

// Loads a 32-bit constant into a register, using XOR for zero, which changes the flags.
static inline void xMovI(struct Emitter* e, int dst, uint32_t imm) {
	if (!imm) {
		xAluRR(e, X_XOR, 4, R(dst), dst);
		return;
	}
	emitRex(e, 4, 0, R(dst), false);
	e8(e, 0xB8 | (dst & 7));
	e32(e, imm);
}

// Doesn't touch the flags.
static inline void xMovIKeepFlags(struct Emitter* e, int dst, uint32_t imm) {
	emitRex(e, 4, 0, R(dst), false);
	e8(e, 0xB8 | (dst & 7));
	e32(e, imm);
}

// Loads a 64-bit constant, for C function addresses.
static inline void xMovI64(struct Emitter* e, int dst, uint64_t imm) {
	emitRex(e, 8, 0, R(dst), false);
	e8(e, 0xB8 | (dst & 7));
	e64(e, imm);
}

// Stores a constant into memory.
static inline void xStoreI(struct Emitter* e, int width, struct Opnd dst, uint32_t imm) {
	emitOp1(e, width, width == 1 ? 0xC6 : 0xC7, 0, dst);
	if (width == 1) {
		e8(e, imm);
	} else if (width == 2) {
		e8(e, imm);
		e8(e, imm >> 8);
	} else {
		e32(e, imm);
	}
}

// LEA, used as an add that leaves the flags alone.
static inline void xLea(struct Emitter* e, int width, int dst, struct Opnd src) {
	emitOp1(e, width, 0x8D, dst, src);
}

// Extends a byte or halfword into a 32-bit register; byte registers 4-7 need an empty REX prefix.
static inline void xExtend(struct Emitter* e, uint8_t op, int dst, int srcWidth, struct Opnd src) {
	struct Opnd probe = src;
	int rex = (dst & 8) ? 4 : 0;
	if (probe.mem) {
		rex |= (probe.index >= 0 && (probe.index & 8)) ? 2 : 0;
		rex |= (probe.base & 8) ? 1 : 0;
	} else {
		rex |= (probe.reg & 8) ? 1 : 0;
	}
	if (rex || (srcWidth == 1 && !probe.mem && probe.reg >= 4 && probe.reg < 8)) {
		e8(e, 0x40 | rex);
	}
	e8(e, 0x0F);
	e8(e, op);
	emitModrm(e, dst, src);
}

static inline void xMovzx(struct Emitter* e, int dst, int srcWidth, struct Opnd src) {
	xExtend(e, srcWidth == 1 ? 0xB6 : 0xB7, dst, srcWidth, src);
}

static inline void xMovsx(struct Emitter* e, int dst, int srcWidth, struct Opnd src) {
	xExtend(e, srcWidth == 1 ? 0xBE : 0xBF, dst, srcWidth, src);
}

// TEST with a register and with a constant.
static inline void xTest(struct Emitter* e, int width, struct Opnd a, int b) {
	emitOp1(e, width, width == 1 ? 0x84 : 0x85, b, a);
}

static inline void xTestI(struct Emitter* e, int width, struct Opnd a, uint32_t imm) {
	emitOp1(e, width, width == 1 ? 0xF6 : 0xF7, 0, a);
	if (width == 1) {
		e8(e, imm);
	} else {
		e32(e, imm);
	}
}

enum ShiftX {
	X_ROL = 0,
	X_ROR = 1,
	X_RCL = 2,
	X_RCR = 3,
	X_SHL = 4,
	X_SHR = 5,
	X_SAR = 7,
};

// Shifts and rotates by a constant, and by CL.
static inline void xShiftI(struct Emitter* e, int shift, int width, struct Opnd dst, int amount) {
	if (amount == 1) {
		emitOp1(e, width, width == 1 ? 0xD0 : 0xD1, shift, dst);
	} else {
		emitOp1(e, width, width == 1 ? 0xC0 : 0xC1, shift, dst);
		e8(e, amount);
	}
}

static inline void xShiftCl(struct Emitter* e, int shift, int width, struct Opnd dst) {
	emitOp1(e, width, width == 1 ? 0xD2 : 0xD3, shift, dst);
}

// NOT and NEG.
static inline void xNot(struct Emitter* e, int width, struct Opnd dst) {
	emitOp1(e, width, 0xF7, 2, dst);
}

static inline void xNeg(struct Emitter* e, int width, struct Opnd dst) {
	emitOp1(e, width, 0xF7, 3, dst);
}

// edx:eax = eax * src (unsigned or signed)
static inline void xMulWide(struct Emitter* e, int width, struct Opnd src, bool sign) {
	emitOp1(e, width, 0xF7, sign ? 5 : 4, src);
}

// eax = edx:eax / src, unsigned; edx = the remainder
static inline void xDiv(struct Emitter* e, int width, struct Opnd src) {
	emitOp1(e, width, 0xF7, 6, src);
}

static inline void xImul(struct Emitter* e, int width, int dst, struct Opnd src) {
	emitOp2(e, width, 0x0F, 0xAF, dst, src);
}

// SETcc and CMOVcc.
static inline void xSetcc(struct Emitter* e, int cc, struct Opnd dst) {
	emitOp2(e, 1, 0x0F, 0x90 | cc, 0, dst);
}

static inline void xCmov(struct Emitter* e, int cc, int width, int dst, struct Opnd src) {
	emitOp2(e, width, 0x0F, 0x40 | cc, dst, src);
}

// BT of a constant bit into the carry flag, and BSR.
static inline void xBtI(struct Emitter* e, int width, struct Opnd dst, int bit) {
	emitOp2(e, width, 0x0F, 0xBA, 4, dst);
	e8(e, bit);
}

static inline void xBsr(struct Emitter* e, int width, int dst, struct Opnd src) {
	emitOp2(e, width, 0x0F, 0xBD, dst, src);
}

// The flags to and from AH, and the carry flag complemented, set and cleared. The guest's flags are
// kept in the host's between instructions where they can be.
static inline void xLahf(struct Emitter* e) {
	e8(e, 0x9F);
}

static inline void xSahf(struct Emitter* e) {
	e8(e, 0x9E);
}

static inline void xCmc(struct Emitter* e) {
	e8(e, 0xF5);
}

static inline void xStc(struct Emitter* e) {
	e8(e, 0xF9);
}

static inline void xClc(struct Emitter* e) {
	e8(e, 0xF8);
}

// RET, PUSH and POP.
static inline void xRet(struct Emitter* e) {
	e8(e, 0xC3);
}

static inline void xPush(struct Emitter* e, int reg) {
	if (reg & 8) {
		e8(e, 0x41);
	}
	e8(e, 0x50 | (reg & 7));
}

static inline void xPop(struct Emitter* e, int reg) {
	if (reg & 8) {
		e8(e, 0x41);
	}
	e8(e, 0x58 | (reg & 7));
}

// Calls and jumps through a register or memory.
static inline void xCallR(struct Emitter* e, int reg) {
	emitOp1(e, 4, 0xFF, 2, R(reg));
}

static inline void xJmpR(struct Emitter* e, int reg) {
	emitOp1(e, 4, 0xFF, 4, R(reg));
}

static inline void xJmpM(struct Emitter* e, struct Opnd m) {
	emitOp1(e, 4, 0xFF, 4, m);
}

// Relative branches return the address of their 32-bit displacement, for patching.
static inline uint8_t* xJmp(struct Emitter* e, const void* target) {
	e8(e, 0xE9);
	uint8_t* site = e->p;
	e32(e, target ? (uint32_t) ((const uint8_t*) target - (e->p + 4)) : 0);
	return site;
}

static inline uint8_t* xCall(struct Emitter* e, const void* target) {
	e8(e, 0xE8);
	uint8_t* site = e->p;
	e32(e, (uint32_t) ((const uint8_t*) target - (e->p + 4)));
	return site;
}

static inline uint8_t* xJcc(struct Emitter* e, int cc, const void* target) {
	e8(e, 0x0F);
	e8(e, 0x80 | cc);
	uint8_t* site = e->p;
	e32(e, target ? (uint32_t) ((const uint8_t*) target - (e->p + 4)) : 0);
	return site;
}

// Points the jump or call at a site to target.
static inline void patchRel32(uint8_t* site, const void* target) {
	int32_t rel = (int32_t) ((const uint8_t*) target - (site + 4));
	memcpy(site, &rel, 4);
}

// Points a forward jump at the code about to be emitted.
static inline void patchHere(struct Emitter* e, uint8_t* site) {
	patchRel32(site, e->p);
}

#endif
