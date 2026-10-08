/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AArch64 backend for jit.c

#define R_CPU 19
#define R_SMC 20
#define R_IWRAM 21
#define R_EWRAM 22
#define R_WB 23
#define R_COVER 24
#define FRAME_SIZE 80
#define CYCLE_SLOT 64

enum {
	A64_EQ = 0,
	A64_NE = 1,
	A64_GE = 10,
};

static void _emit(struct Emitter* e, uint32_t insn) {
	memcpy(e->p, &insn, 4);
	e->p += 4;
}

static uint8_t* _emitSite(struct Emitter* e, uint32_t insn) {
	uint8_t* at = e->p;
	_emit(e, insn);
	return at;
}

static void _patch(uint8_t* at, const uint8_t* target) {
	int32_t offset = (target - at) / 4;
	uint32_t insn;
	memcpy(&insn, at, 4);
	if ((insn & 0xFC000000) == 0x14000000) {
		insn |= offset & 0x3FFFFFF;
	} else if ((insn & 0x7E000000) == 0x36000000) {
		insn |= (offset & 0x3FFF) << 5;
	} else {
		insn |= (offset & 0x7FFFF) << 5;
	}
	memcpy(at, &insn, 4);
}

static void _movImm32(struct Emitter* e, int rd, uint32_t value) {
	_emit(e, 0x52800000 | ((value & 0xFFFF) << 5) | rd);
	if (value >> 16) {
		_emit(e, 0x72A00000 | ((value >> 16) << 5) | rd);
	}
}

static void _movImm64(struct Emitter* e, int rd, uint64_t value) {
	_emit(e, 0xD2800000 | ((value & 0xFFFF) << 5) | rd);
	int hw;
	for (hw = 1; hw < 4; ++hw) {
		uint32_t part = (value >> (16 * hw)) & 0xFFFF;
		if (part) {
			_emit(e, 0xF2800000 | (hw << 21) | (part << 5) | rd);
		}
	}
}

static void _ldrW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xB9400000 | ((offset >> 2) << 10) | (rn << 5) | rt);
}

static void _strW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xB9000000 | ((offset >> 2) << 10) | (rn << 5) | rt);
}

static void _ldrbW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0x39400000 | (offset << 10) | (rn << 5) | rt);
}

static void _ldrX(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xF9400000 | ((offset >> 3) << 10) | (rn << 5) | rt);
}

static void _stpX(struct Emitter* e, int rt, int rt2, int offset) {
	_emit(e, 0xA9000000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _ldpX(struct Emitter* e, int rt, int rt2, int offset) {
	_emit(e, 0xA9400000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _addWImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_emit(e, 0x11000000 | (imm << 10) | (rn << 5) | rd);
}

static void _addXImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_emit(e, 0x91000000 | (imm << 10) | (rn << 5) | rd);
}

static void _addW(struct Emitter* e, int rd, int rn, int rm) {
	_emit(e, 0x0B000000 | (rm << 16) | (rn << 5) | rd);
}

static void _cmpW(struct Emitter* e, int rn, int rm) {
	_emit(e, 0x6B00001F | (rm << 16) | (rn << 5));
}

static void _movW(struct Emitter* e, int rd, int rm) {
	_emit(e, 0x2A0003E0 | (rm << 16) | rd);
}

static void _movX(struct Emitter* e, int rd, int rm) {
	_emit(e, 0xAA0003E0 | (rm << 16) | rd);
}

// AND with a contiguous run of ones: len bits starting at lsb
static void _andImm(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned len) {
	_emit(e, 0x12000000 | (((32 - lsb) & 31) << 16) | ((len - 1) << 10) | (rn << 5) | rd);
}

static void _ubfx(struct Emitter* e, int rd, int rn, unsigned lsb, unsigned width) {
	_emit(e, 0x53000000 | (lsb << 16) | ((lsb + width - 1) << 10) | (rn << 5) | rd);
}

static void _lslWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x53000000 | (((32 - shift) & 31) << 16) | ((31 - shift) << 10) | (rn << 5) | rd);
}

static void _lsrWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x53007C00 | (shift << 16) | (rn << 5) | rd);
}

static void _asrWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x13007C00 | (shift << 16) | (rn << 5) | rd);
}

static void _rorWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x13800000 | (rn << 16) | (shift << 10) | (rn << 5) | rd);
}

static void _lsrvW(struct Emitter* e, int rd, int rn, int rm) {
	_emit(e, 0x1AC02400 | (rm << 16) | (rn << 5) | rd);
}

static void _orrWShift(struct Emitter* e, int rd, int rn, int rm, unsigned lsl) {
	_emit(e, 0x2A000000 | (rm << 16) | (lsl << 10) | (rn << 5) | rd);
}

static void _blr(struct Emitter* e, int rn) {
	_emit(e, 0xD63F0000 | (rn << 5));
}

static uint8_t* _bCond(struct Emitter* e, int cond) {
	return _emitSite(e, 0x54000000 | cond);
}

static uint8_t* _cbzW(struct Emitter* e, int rt) {
	return _emitSite(e, 0x34000000 | rt);
}

static uint8_t* _cbnzW(struct Emitter* e, int rt) {
	return _emitSite(e, 0x35000000 | rt);
}

static uint8_t* _tbnz(struct Emitter* e, int rt, unsigned bit) {
	return _emitSite(e, 0x37000000 | (bit << 19) | rt);
}

static uint8_t* _b(struct Emitter* e) {
	return _emitSite(e, 0x14000000);
}

static void _prologue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_emit(e, 0xA9800000 | ((-FRAME_SIZE / 8) & 0x7F) << 15 | (30 << 10) | (31 << 5) | 29); // stp x29, x30, [sp, #-FRAME_SIZE]!
	_stpX(e, 19, 20, 16);
	_stpX(e, 21, 22, 32);
	_stpX(e, 23, 24, 48);
	_movX(e, R_CPU, 0);
	if (c->gba) {
		_movImm64(e, R_SMC, (uintptr_t) &c->jit->smcHit);
		_movImm64(e, R_IWRAM, (uintptr_t) c->gba->memory.iwram);
		_movImm64(e, R_EWRAM, (uintptr_t) c->gba->memory.wram);
		_movImm64(e, R_COVER, (uintptr_t) c->jit->cover);
	}
}

static void _epilogue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_ldpX(e, 23, 24, 48);
	_ldpX(e, 21, 22, 32);
	_ldpX(e, 19, 20, 16);
	_emit(e, 0xA8C00000 | ((FRAME_SIZE / 8) << 15) | (30 << 10) | (31 << 5) | 29); // ldp x29, x30, [sp], #FRAME_SIZE
	_emit(e, 0xD65F03C0); // ret
}

static void _exitJump(struct Compiler* c, int index) {
	_exitAt(c, _b(&c->e), index);
}

// Interpreter state just before ops[index] runs
static void _storeState(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	_movImm32(e, 0, c->pc + 4 * index + 4);
	_strW(e, 0, R_CPU, OFF_PC);
	_movImm32(e, 0, c->ops[index]);
	_strW(e, 0, R_CPU, OFF_PREFETCH0);
	_movImm32(e, 0, c->ops[index + 1]);
	_strW(e, 0, R_CPU, OFF_PREFETCH1);
}

static void _addCyclesReg(struct Compiler* c, int reg, uint32_t constant) {
	struct Emitter* e = &c->e;
	_ldrW(e, 1, R_CPU, OFF_CYCLES);
	if (reg >= 0) {
		_addW(e, 1, 1, reg);
	}
	if (constant) {
		_addWImm(e, 1, 1, constant);
	}
	_strW(e, 1, R_CPU, OFF_CYCLES);
}

static void _addCycles(struct Compiler* c, uint32_t constant) {
	_addCyclesReg(c, -1, constant);
}

static void _eventCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_ldrW(e, 0, R_CPU, OFF_CYCLES);
	_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
	_cmpW(e, 0, 1);
	_exitAt(c, _bCond(e, A64_GE), index);
}

static void _segmentCheck(struct Compiler* c, unsigned index, uint32_t cycles) {
	struct Emitter* e = &c->e;
	_ldrW(e, 0, R_CPU, OFF_CYCLES);
	_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
	_addWImm(e, 0, 0, cycles);
	_cmpW(e, 0, 1);
	_exitAt(c, _bCond(e, A64_GE), index);
}

static void _smcCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_ldrbW(e, 0, R_SMC, 0);
	_exitAt(c, _cbnzW(e, 0), index);
}

static void _loadReg(struct Compiler* c, int rt, unsigned reg, uint32_t address) {
	if (reg == ARM_PC) {
		_movImm32(&c->e, rt, address + 8);
	} else {
		_ldrW(&c->e, rt, R_CPU, 4 * reg);
	}
}

// Host NZCV = guest NZCV; leaves CPSR in w9
static void _loadFlags(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_ldrW(e, 9, R_CPU, OFF_CPSR);
	_andImm(e, 10, 9, 28, 4);
	_emit(e, 0xD51B4200 | 10); // msr nzcv, x10
}

static void _loadCarry(struct Compiler* c, int rd) {
	_ldrW(&c->e, 9, R_CPU, OFF_CPSR);
	_ubfx(&c->e, rd, 9, 29, 1);
}

// Barrel shifter for an immediate shift amount on w3: value in w1, carry out in w2 if wanted
static void _shiftImm(struct Compiler* c, unsigned type, unsigned amount, bool carry) {
	struct Emitter* e = &c->e;
	switch (type) {
	case 0: // LSL
		if (!amount) {
			_movW(e, 1, 3);
			if (carry) {
				_loadCarry(c, 2);
			}
		} else {
			_lslWImm(e, 1, 3, amount);
			if (carry) {
				_ubfx(e, 2, 3, 32 - amount, 1);
			}
		}
		break;
	case 1: // LSR
		if (!amount) {
			_movImm32(e, 1, 0);
			if (carry) {
				_lsrWImm(e, 2, 3, 31);
			}
		} else {
			_lsrWImm(e, 1, 3, amount);
			if (carry) {
				_ubfx(e, 2, 3, amount - 1, 1);
			}
		}
		break;
	case 2: // ASR
		if (!amount) {
			_asrWImm(e, 1, 3, 31);
			if (carry) {
				_lsrWImm(e, 2, 3, 31);
			}
		} else {
			_asrWImm(e, 1, 3, amount);
			if (carry) {
				_ubfx(e, 2, 3, amount - 1, 1);
			}
		}
		break;
	case 3: // ROR, RRX
		if (!amount) {
			_loadCarry(c, 2);
			_lsrWImm(e, 1, 3, 1);
			_orrWShift(e, 1, 1, 2, 31);
			if (carry) {
				_andImm(e, 2, 3, 0, 1);
			}
		} else {
			_rorWImm(e, 1, 3, amount);
			if (carry) {
				_ubfx(e, 2, 3, amount - 1, 1);
			}
		}
		break;
	}
}

// storeCarry: MULS and MLAS take C from the last shifter result, so keep it for them
static void _emitAlu(struct Compiler* c, unsigned i, bool storeCarry) {
	struct Emitter* e = &c->e;
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + 4 * i;
	unsigned cond = op >> 28;
	unsigned opcode = (op >> 21) & 0xF;
	bool s = op & 0x00100000;
	unsigned rn = (op >> 16) & 0xF;
	unsigned rd = (op >> 12) & 0xF;
	bool logical = _isLogical(opcode);
	bool carryIn = opcode == ALU_ADC || opcode == ALU_SBC || opcode == ALU_RSC;
	bool carryOut = (s && logical) || storeCarry;

	uint8_t* skip = NULL;
	if (cond != 0xE) {
		_loadFlags(c);
		skip = _bCond(e, cond ^ 1);
	}

	if (op & 0x02000000) {
		unsigned rotate = (op >> 7) & 0x1E;
		uint32_t value = op & 0xFF;
		if (rotate) {
			value = (value >> rotate) | (value << (32 - rotate));
		}
		_movImm32(e, 1, value);
		if (carryOut) {
			if (rotate) {
				_movImm32(e, 2, value >> 31);
			} else {
				_loadCarry(c, 2);
			}
		}
	} else {
		_loadReg(c, 3, op & 0xF, address);
		_shiftImm(c, (op >> 5) & 3, (op >> 7) & 0x1F, carryOut);
	}
	if (opcode != ALU_MOV && opcode != ALU_MVN) {
		_loadReg(c, 0, rn, address);
	}
	if (carryIn) {
		_loadFlags(c);
	}

	switch (opcode) {
	case ALU_AND:
	case ALU_TST:
		_emit(e, 0x0A000000 | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_EOR:
	case ALU_TEQ:
		_emit(e, 0x4A000000 | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_ORR:
		_emit(e, 0x2A000000 | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_BIC:
		_emit(e, 0x0A200000 | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_MOV:
		_movW(e, 3, 1);
		break;
	case ALU_MVN:
		_emit(e, 0x2A2003E0 | (1 << 16) | 3);
		break;
	case ALU_ADD:
	case ALU_CMN:
		_emit(e, (s ? 0x2B000000 : 0x0B000000) | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_SUB:
	case ALU_CMP:
		_emit(e, (s ? 0x6B000000 : 0x4B000000) | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_RSB:
		_emit(e, (s ? 0x6B000000 : 0x4B000000) | (0 << 16) | (1 << 5) | 3);
		break;
	case ALU_ADC:
		_emit(e, (s ? 0x3A000000 : 0x1A000000) | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_SBC:
		_emit(e, (s ? 0x7A000000 : 0x5A000000) | (1 << 16) | (0 << 5) | 3);
		break;
	case ALU_RSC:
		_emit(e, (s ? 0x7A000000 : 0x5A000000) | (0 << 16) | (1 << 5) | 3);
		break;
	}

	if (s) {
		if (logical) {
			// N and Z from the result, C from the shifter, V and bits 24-27 kept
			_ldrW(e, 9, R_CPU, OFF_CPSR);
			_andImm(e, 9, 9, 0, 29);
			_andImm(e, 4, 3, 31, 1);
			_orrWShift(e, 9, 9, 4, 0);
			_emit(e, 0x7100001F | (3 << 5)); // cmp w3, #0
			_emit(e, 0x1A9F17E0 | 4); // cset w4, eq
			_orrWShift(e, 9, 9, 4, 30);
			_orrWShift(e, 9, 9, 2, 29);
		} else {
			_emit(e, 0xD53B4200 | 4); // mrs x4, nzcv
			_ldrW(e, 9, R_CPU, OFF_CPSR);
			if (carryIn) {
				_andImm(e, 9, 9, 0, 28);
			} else {
				_andImm(e, 9, 9, 0, 24); // _additionS and _subtractionS clear the whole flags byte
			}
			_orrWShift(e, 9, 9, 4, 0);
		}
		_strW(e, 9, R_CPU, OFF_CPSR);
	}
	if (opcode < ALU_TST || opcode > ALU_CMN) {
		_strW(e, 3, R_CPU, 4 * rd);
	}
	if (storeCarry) {
		_strW(e, 2, R_CPU, OFF_SHIFTER_CARRY);
	}
	if (skip) {
		_patch(skip, e->p);
	}
}

// IWRAM or EWRAM access at [base + w8]; loads land in w0 (rotated like the GBA), stores take w6
static void _memAccess(struct Emitter* e, bool load, unsigned size, int base) {
	static const uint32_t loads[] = { 0, 0x38604800, 0x78604800, 0, 0xB8604800 };
	static const uint32_t stores[] = { 0, 0x38204800, 0x78204800, 0, 0xB8204800 };
	if (load) {
		_emit(e, loads[size] | (8 << 16) | (base << 5) | 0);
		if (size > 1) {
			_andImm(e, 9, 4, 0, size == 4 ? 2 : 1);
			_lslWImm(e, 9, 9, 3);
			_emit(e, 0x1AC02C00 | (9 << 16) | (0 << 5) | 0); // rorv w0, w0, w9
		}
	} else {
		_emit(e, stores[size] | (8 << 16) | (base << 5) | 6);
	}
}

static void _emitMem(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + 4 * i;
	unsigned cond = op >> 28;
	bool load = op & (1 << 20);
	bool p = op & (1 << 24);
	bool u = op & (1 << 23);
	bool w = op & (1 << 21);
	bool writeback = !p || w;
	unsigned rn = (op >> 16) & 0xF;
	unsigned rd = (op >> 12) & 0xF;
	bool halfword = !(op & 0x0C000000);
	unsigned size = halfword ? 2 : (op & (1 << 22)) ? 1 : 4;

	uint8_t* fail = NULL;
	if (cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, cond ^ 1);
	}
	if (!load) {
		_loadReg(c, 6, rd, address);
	}
	if (halfword) {
		if (op & (1 << 22)) {
			_movImm32(e, 1, ((op >> 4) & 0xF0) | (op & 0xF));
		} else {
			_loadReg(c, 1, op & 0xF, address);
		}
	} else if (op & 0x02000000) {
		// Addressing mode 2 shifts: LSR and ASR #0 mean #32, ROR #0 is RRX
		_loadReg(c, 3, op & 0xF, address);
		_shiftImm(c, (op >> 5) & 3, (op >> 7) & 0x1F, false);
	} else {
		_movImm32(e, 1, op & 0xFFF);
	}
	_loadReg(c, 4, rn, address);
	_emit(e, (u ? 0x0B000000 : 0x4B000000) | (1 << 16) | (4 << 5) | 5); // w5 = base +/- offset
	if (writeback) {
		_movW(e, R_WB, 5);
	}
	if (p) {
		_movW(e, 4, 5);
	}
	if (load && writeback) {
		_strW(e, R_WB, R_CPU, 4 * rn);
	}

	uint8_t* done[2];
	_lsrWImm(e, 7, 4, 24);
	_emit(e, 0x7100001F | (3 << 10) | (7 << 5)); // cmp w7, #3
	uint8_t* toEwram = _bCond(e, A64_NE);
	_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 13 : size == 2 ? 14 : 15);
	_memAccess(e, load, size, R_IWRAM);
	if (!load) {
		// Writes into compiled code
		_lsrWImm(e, 9, 8, 2);
		_emit(e, 0x38604800 | (9 << 16) | (R_COVER << 5) | 10); // ldrb w10, [x24, w9, uxtw]
		uint8_t* noCode = _cbzW(e, 10);
		_movImm64(e, 0, (uintptr_t) c->jit);
		_movW(e, 1, 9);
		_movImm64(e, 16, (uintptr_t) ARMJitInvalidateWord);
		_blr(e, 16);
		_patch(noCode, e->p);
	}
	_movImm32(e, 3, load ? 2 : 1);
	done[0] = _b(e);

	_patch(toEwram, e->p);
	_emit(e, 0x7100001F | (2 << 10) | (7 << 5)); // cmp w7, #2
	uint8_t* toSlow = _bCond(e, A64_NE);
	_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 16 : size == 2 ? 17 : 18);
	_memAccess(e, load, size, R_EWRAM);
	char* waits = size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16;
	_movImm64(e, 9, (uintptr_t) &waits[GBA_REGION_EWRAM]);
	_ldrbW(e, 3, 9, 0);
	_addWImm(e, 3, 3, load ? 2 : 1);
	done[1] = _b(e);

	_patch(toSlow, e->p);
	_storeState(c, i + 1);
	_strW(e, 31, 31, CYCLE_SLOT); // str wzr, [sp, #CYCLE_SLOT]
	_movX(e, 0, R_CPU);
	_movW(e, 1, 4);
	unsigned offset;
	if (load) {
		_addXImm(e, 2, 31, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, load32) : size == 2 ? offsetof(struct ARMMemory, load16) : offsetof(struct ARMMemory, load8);
	} else {
		_movW(e, 2, 6);
		_addXImm(e, 3, 31, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, store32) : size == 2 ? offsetof(struct ARMMemory, store16) : offsetof(struct ARMMemory, store8);
	}
	_ldrX(e, 16, R_CPU, OFF_MEMORY + offset);
	_blr(e, 16);
	_ldrW(e, 3, 31, CYCLE_SLOT);

	_patch(done[0], e->p);
	_patch(done[1], e->p);
	if (load) {
		_strW(e, 0, R_CPU, 4 * rd);
	} else if (writeback) {
		_strW(e, R_WB, R_CPU, 4 * rn);
	}
	_addCyclesReg(c, 3, c->memCycles);
	if (fail) {
		uint8_t* after = _b(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
	if (!load) {
		_smcCheck(c, i + 1);
	}
	_eventCheck(c, i + 1);
}

static void _emitFallback(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + 4 * i;
	unsigned cond = op >> 28;

	_storeState(c, i + 1);
	uint8_t* toCheck = NULL;
	if (cond != 0xE) {
		_ldrW(e, 0, R_CPU, OFF_CPSR);
		_lsrWImm(e, 0, 0, 28);
		_movImm32(e, 1, _conditionLut[cond]);
		_lsrvW(e, 1, 1, 0);
		uint8_t* toExec = _tbnz(e, 1, 0);
		_addCycles(c, c->aluCycles);
		toCheck = _b(e);
		_patch(toExec, e->p);
	}

	_movX(e, 0, R_CPU);
	_movImm32(e, 1, op);
	_movImm64(e, 16, (uintptr_t) _armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)]);
	_blr(e, 16);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_movImm32(e, 1, address + 8);
	_cmpW(e, 0, 1);
	uint8_t* branched = _bCond(e, A64_NE);
	uint8_t* sequential = _b(e);

	// A taken branch back to the start of this block keeps running here
	_patch(branched, e->p);
	bool loops = false;
	if ((op & 0x0E000000) == 0x0A000000) {
		int32_t offset = (int32_t) (op << 8) >> 6;
		loops = address + 8 + offset == c->pc;
	}
	if (loops) {
		_movImm32(e, 1, c->pc + 4);
		_cmpW(e, 0, 1);
		_exitAt(c, _bCond(e, A64_NE), EXIT_DIRECT);
		_ldrbW(e, 0, R_SMC, 0);
		_exitAt(c, _cbnzW(e, 0), EXIT_DIRECT);
		_ldrW(e, 0, R_CPU, OFF_CYCLES);
		_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
		_cmpW(e, 0, 1);
		_exitAt(c, _bCond(e, A64_GE), EXIT_DIRECT);
		_patch(_b(e), c->body);
	} else {
		_exitJump(c, EXIT_DIRECT);
	}

	_patch(sequential, e->p);
	if (toCheck) {
		_patch(toCheck, e->p);
	}
	_smcCheck(c, i + 1);
	_eventCheck(c, i + 1);
}
