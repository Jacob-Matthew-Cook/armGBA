/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// AArch64 backend for jit.c

#define R_CPU 19
#define R_JIT 20
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
		_movImm64(e, R_JIT, (uintptr_t) c->jit);
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

static void _jumpTo(struct Compiler* c, const uint8_t* target) {
	_patch(_b(&c->e), target);
}

static void _patchLink(uint8_t* site, const uint8_t* target) {
	uint32_t insn = 0x14000000 | (((target - site) / 4) & 0x3FFFFFF);
	memcpy(site, &insn, 4);
	__builtin___clear_cache((char*) site, (char*) site + 4);
}

// Blocks are entered here from the dispatcher and from linked exits
static void _blockEntry(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_movImm64(e, 0, (uintptr_t) c->block);
	_emit(e, 0xF9000000 | ((JIT_CURRENT / 8) << 10) | (R_JIT << 5) | 0); // str x0, [x20, #current]
	_emit(e, 0x39000000 | (JIT_SMC_HIT << 10) | (R_JIT << 5) | 31); // strb wzr, [x20, #smcHit]
}

static void _linkJump(struct Compiler* c) {
	struct ARMJitLink* link = &c->block->links[c->nLinks++];
	link->site = _b(&c->e);
}

static void _linkStub(struct Compiler* c, struct ARMJitLink* link) {
	_movImm64(&c->e, 2, (uintptr_t) link);
	_jumpTo(c, c->jit->linkDispatch);
}

static void _emitTrampoline(struct Compiler* c) {
	struct Emitter* e = &c->e;
	c->jit->enter = (void (*)(struct ARMCore*, void*)) e->p;
	_prologue(c);
	_emit(e, 0xD61F0000 | (1 << 5)); // br x1

	// A link stub passes its link in x2 so the exit gets patched to the block found
	c->jit->linkDispatch = e->p;
	_emit(e, 0xF9000000 | ((JIT_PENDING_LINK / 8) << 10) | (R_JIT << 5) | 2); // str x2, [x20, #pendingLink]
	uint8_t* toLookup = _b(e);

	// Next block for the PC and mode in the CPU state, if it is compiled and nothing is due
	c->jit->dispatch = e->p;
	_emit(e, 0xF9000000 | ((JIT_PENDING_LINK / 8) << 10) | (R_JIT << 5) | 31); // str xzr, [x20, #pendingLink]
	_patch(toLookup, e->p);
	uint8_t* toC[8];
	_ldrW(e, 0, R_CPU, OFF_CYCLES);
	_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
	_cmpW(e, 0, 1);
	toC[0] = _bCond(e, A64_GE);
	_ldrW(e, 2, R_CPU, OFF_EXECUTION_MODE);
	_ldrW(e, 3, R_CPU, OFF_PC);
	_emit(e, 0x51001000 | (3 << 5) | 3); // sub w3, w3, #4
	_emit(e, 0x0B000000 | (2 << 16) | (1 << 10) | (3 << 5) | 3); // add w3, w3, w2, lsl #1
	_ubfx(e, 4, 3, 12, 16);
	_addXImm(e, 5, R_JIT, JIT_PAGES);
	_emit(e, 0xF8607800 | (4 << 16) | (5 << 5) | 5); // ldr x5, [x5, x4, lsl #3]
	toC[1] = _emitSite(e, 0xB4000000 | 5); // cbz x5
	_ubfx(e, 6, 3, 1, 11);
	_emit(e, 0xF8607800 | (6 << 16) | (5 << 5) | 5); // ldr x5, [x5, x6, lsl #3]
	toC[2] = _emitSite(e, 0xB4000000 | 5); // cbz x5
	_ldrW(e, 7, 5, BLOCK_PC);
	_cmpW(e, 7, 3);
	toC[3] = _bCond(e, A64_NE);
	_ldrbW(e, 7, 5, BLOCK_THUMB);
	_cmpW(e, 7, 2);
	toC[4] = _bCond(e, A64_NE);
	_ldrW(e, 7, R_CPU, OFF_PREFETCH0);
	_ldrW(e, 8, 5, BLOCK_OP0);
	_cmpW(e, 7, 8);
	toC[5] = _bCond(e, A64_NE);
	_ldrW(e, 7, R_CPU, OFF_PREFETCH1);
	_ldrW(e, 8, 5, BLOCK_OP1);
	_cmpW(e, 7, 8);
	toC[6] = _bCond(e, A64_NE);
	_ldrX(e, 1, R_JIT, JIT_PENDING_LINK);
	uint8_t* noLink = _emitSite(e, 0xB4000000 | 1); // cbz x1
	_emit(e, 0xF9000000 | ((JIT_CURRENT / 8) << 10) | (R_JIT << 5) | 5); // str x5, [x20, #current]
	_movX(e, 0, R_JIT);
	_movX(e, 2, 5);
	_movImm64(e, 16, (uintptr_t) ARMJitLink);
	_blr(e, 16);
	_ldrX(e, 5, R_JIT, JIT_CURRENT);
	_patch(noLink, e->p);
	_ldrX(e, 16, 5, BLOCK_ENTRY);
	_emit(e, 0xD61F0000 | (16 << 5)); // br x16

	c->jit->toC = e->p;
	unsigned i;
	for (i = 0; i < 7; ++i) {
		_patch(toC[i], e->p);
	}
	_epilogue(c);
}

static void _exitJump(struct Compiler* c, int index) {
	_exitAt(c, _b(&c->e), index);
}

static void _storePrefetch(struct Compiler* c, unsigned offset, unsigned index) {
	struct Emitter* e = &c->e;
	if (c->hot[index]) {
		_ldrW(e, 0, R_JIT, JIT_FETCHED + 4 * index);
	} else {
		_movImm32(e, 0, c->ops[index]);
	}
	_strW(e, 0, R_CPU, offset);
}

// Interpreter state just before ops[index] runs
static void _storeState(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	_movImm32(e, 0, c->pc + c->width * (index + 1));
	_strW(e, 0, R_CPU, OFF_PC);
	_storePrefetch(c, OFF_PREFETCH0, index);
	_storePrefetch(c, OFF_PREFETCH1, index + 1);
}

// Copy a patched word two instructions ahead, when the GBA's pipeline would fetch it
static void _fetchAhead(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	unsigned index = i + 2;
	if (index > c->count + 1 || !c->hot[index]) {
		return;
	}
	_movImm64(e, 0, (uintptr_t) _hostAddress(c, c->pc + c->width * index));
	if (c->thumb) {
		_emit(e, 0x79400000 | (0 << 5) | 0); // ldrh w0, [x0]
	} else {
		_ldrW(e, 0, 0, 0);
	}
	_strW(e, 0, R_JIT, JIT_FETCHED + 4 * index);
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
	_exitAt(c, _bCond(e, A64_GE), index < 0 ? index : index | EXIT_DUE);
}

static void _segmentCheck(struct Compiler* c, unsigned index, uint32_t cycles) {
	struct Emitter* e = &c->e;
	_ldrW(e, 0, R_CPU, OFF_CYCLES);
	_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
	_addWImm(e, 0, 0, cycles);
	_cmpW(e, 0, 1);
	_exitAt(c, _bCond(e, A64_GE), index | EXIT_DUE);
}

static void _smcCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_ldrbW(e, 0, R_JIT, JIT_SMC_HIT);
	_exitAt(c, _cbnzW(e, 0), index);
}

static void _loadSource(struct Compiler* c, int rt, struct Source source) {
	if (source.constant) {
		_movImm32(&c->e, rt, source.value);
	} else {
		_ldrW(&c->e, rt, R_CPU, 4 * source.reg);
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
static void _emitAlu(struct Compiler* c, const struct AluOp* alu, bool storeCarry) {
	struct Emitter* e = &c->e;
	unsigned opcode = alu->opcode;
	bool s = alu->s;
	bool logical = _isLogical(opcode);
	bool carryIn = opcode == ALU_ADC || opcode == ALU_SBC || opcode == ALU_RSC;
	bool setFlags = s && alu->flagsLive;
	bool carryOut = (setFlags && logical) || storeCarry;

	uint8_t* skip = NULL;
	if (alu->cond != 0xE) {
		_loadFlags(c);
		skip = _bCond(e, alu->cond ^ 1);
	}

	if (alu->immediate) {
		_movImm32(e, 1, alu->imm);
		if (carryOut) {
			if (alu->immCarry >= 0) {
				_movImm32(e, 2, alu->immCarry);
			} else {
				_loadCarry(c, 2);
			}
		}
	} else {
		_loadSource(c, 3, alu->m);
		_shiftImm(c, alu->shiftType, alu->shiftAmount, carryOut);
	}
	if (opcode != ALU_MOV && opcode != ALU_MVN) {
		_loadSource(c, 0, alu->n);
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

	if (setFlags) {
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
			// Add and subtract clear the whole flags byte; SBC and RSC keep bits 24-27
			if (carryIn && opcode != ALU_ADC) {
				_andImm(e, 9, 9, 0, 28);
			} else {
				_andImm(e, 9, 9, 0, 24);
			}
			_orrWShift(e, 9, 9, 4, 0);
		}
		_strW(e, 9, R_CPU, OFF_CPSR);
	}
	if (opcode < ALU_TST || opcode > ALU_CMN) {
		_strW(e, 3, R_CPU, 4 * alu->rd);
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

static void _emitMem(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned size = mem->size;

	uint8_t* fail = NULL;
	if (mem->cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, mem->cond ^ 1);
	}
	if (!load) {
		_loadSource(c, 6, _reg(mem->rd));
	}
	if (mem->immediateOffset) {
		_movImm32(e, 1, mem->offset);
	} else {
		_loadSource(c, 3, mem->m);
		_shiftImm(c, mem->shiftType, mem->shiftAmount, false);
	}
	_loadSource(c, 4, mem->base);
	_emit(e, (mem->up ? 0x0B000000 : 0x4B000000) | (1 << 16) | (4 << 5) | 5); // w5 = base +/- offset
	if (mem->writeback) {
		_movW(e, R_WB, 5);
	}
	if (mem->pre) {
		_movW(e, 4, 5);
	}
	if (load && mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
	}
	if (mem->signExtend) {
		_strW(e, 4, 31, CYCLE_SLOT + 4); // the address decides how LDRSH extends
	}

	uint8_t* done[2] = { NULL, NULL };
	uint8_t* toSlow = NULL;
	if (c->romCode) {
		goto slow;
	}
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
	toSlow = _bCond(e, A64_NE);
	_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 16 : size == 2 ? 17 : 18);
	_memAccess(e, load, size, R_EWRAM);
	if (!load) {
		_lsrWImm(e, 9, 8, 2);
		_movImm32(e, 10, ARM_JIT_IWRAM_WORDS);
		_addW(e, 9, 9, 10);
		_emit(e, 0x38604800 | (9 << 16) | (R_COVER << 5) | 10); // ldrb w10, [x24, w9, uxtw]
		uint8_t* noCode = _cbzW(e, 10);
		_movImm64(e, 0, (uintptr_t) c->jit);
		_movW(e, 1, 9);
		_movImm64(e, 16, (uintptr_t) ARMJitInvalidateWord);
		_blr(e, 16);
		_patch(noCode, e->p);
	}
	char* waits = size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16;
	_movImm64(e, 9, (uintptr_t) &waits[GBA_REGION_EWRAM]);
	_ldrbW(e, 3, 9, 0);
	_addWImm(e, 3, 3, load ? 2 : 1);
	done[1] = _b(e);

	_patch(toSlow, e->p);
slow:
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

	if (done[0]) {
		_patch(done[0], e->p);
		_patch(done[1], e->p);
	}
	if (mem->signExtend) {
		uint8_t* halfword = NULL;
		if (size == 2) {
			// LDRSH of an odd address sign-extends the rotated byte
			_ldrW(e, 9, 31, CYCLE_SLOT + 4);
			halfword = _emitSite(e, 0x36000000 | (0 << 19) | 9); // tbz w9, #0
		}
		_emit(e, 0x13001C00 | (0 << 5) | 0); // sxtb w0, w0
		if (halfword) {
			uint8_t* extended = _b(e);
			_patch(halfword, e->p);
			_emit(e, 0x13003C00 | (0 << 5) | 0); // sxth w0, w0
			_patch(extended, e->p);
		}
	}
	if (load) {
		_strW(e, 0, R_CPU, 4 * mem->rd);
	} else if (mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
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

static const uint32_t _conditionLut32[16] = {
	0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
	0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000
};

// A patched instruction: run its handler with the opcode the pipeline fetched
static void _emitDynamic(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t address = c->pc + c->width * i;

	_storeState(c, i + 1);
	_ldrW(e, 1, R_JIT, JIT_FETCHED + 4 * i);
	uint8_t* toCheck = NULL;
	if (c->thumb) {
		_lsrWImm(e, 12, 1, 6);
		_movImm64(e, 14, (uintptr_t) _thumbTable);
	} else {
		_ldrW(e, 9, R_CPU, OFF_CPSR);
		_lsrWImm(e, 9, 9, 28);
		_lsrWImm(e, 10, 1, 28);
		_movImm64(e, 11, (uintptr_t) _conditionLut32);
		_emit(e, 0xB8605800 | (10 << 16) | (11 << 5) | 11); // ldr w11, [x11, w10, uxtw #2]
		_lsrvW(e, 11, 11, 9);
		uint8_t* toExec = _tbnz(e, 11, 0);
		_addCycles(c, c->aluCycles);
		toCheck = _b(e);
		_patch(toExec, e->p);
		_ubfx(e, 12, 1, 20, 8);
		_ubfx(e, 13, 1, 4, 4);
		_orrWShift(e, 12, 13, 12, 4);
		_movImm64(e, 14, (uintptr_t) _armTable);
	}
	_emit(e, 0xF8605800 | (12 << 16) | (14 << 5) | 16); // ldr x16, [x14, w12, uxtw #3]
	_movX(e, 0, R_CPU);
	_blr(e, 16);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_movImm32(e, 1, address + 2 * c->width);
	_cmpW(e, 0, 1);
	_exitAt(c, _bCond(e, A64_NE), EXIT_DIRECT);
	if (toCheck) {
		_patch(toCheck, e->p);
	}
	_smcCheck(c, i + 1);
	_eventCheck(c, i + 1);
}

static void _emitFallback(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + c->width * i;
	unsigned cond = _fallbackCond(c, op);

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
	_movImm64(e, 16, (uintptr_t) _handler(c, op));
	_blr(e, 16);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_movImm32(e, 1, address + 2 * c->width);
	_cmpW(e, 0, 1);
	uint8_t* branched = _bCond(e, A64_NE);
	uint8_t* sequential = _b(e);

	// A taken branch back to the start of this block keeps running here
	_patch(branched, e->p);
	if (_loopsToStart(c, i)) {
		_movImm32(e, 1, c->pc + c->width);
		_cmpW(e, 0, 1);
		_exitAt(c, _bCond(e, A64_NE), EXIT_DIRECT);
		_ldrbW(e, 0, R_JIT, JIT_SMC_HIT);
		_exitAt(c, _cbnzW(e, 0), EXIT_DIRECT);
		_ldrW(e, 0, R_CPU, OFF_CYCLES);
		_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
		_cmpW(e, 0, 1);
		_exitAt(c, _bCond(e, A64_GE), EXIT_TO_C);
		_patch(_b(e), c->body);
	} else {
		uint32_t target;
		if (_branchTarget(c, i, &target)) {
			_ldrW(e, 0, R_CPU, OFF_CYCLES);
			_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
			_cmpW(e, 0, 1);
			_exitAt(c, _bCond(e, A64_GE), EXIT_TO_C);
			_linkJump(c);
		} else {
			_exitJump(c, EXIT_DIRECT);
		}
	}

	_patch(sequential, e->p);
	if (toCheck) {
		_patch(toCheck, e->p);
	}
	_smcCheck(c, i + 1);
	_eventCheck(c, i + 1);
}
