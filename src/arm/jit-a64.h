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
// The cycle count lives here while generated code runs
#define R_CYCLES 25
// cpu->nextEvent, reloaded whenever C code may have changed it
#define R_NEXT 27
#define FRAME_SIZE 112
#define CYCLE_SLOT 96

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
	if ((insn & 0x7C000000) == 0x14000000) { // B, BL
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

// C sees and may change the cycle count and the next event
static void _blrC(struct Emitter* e, int rn) {
	_strW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_blr(e, rn);
	_ldrW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
}

static void _prologue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_emit(e, 0xA9800000 | ((-FRAME_SIZE / 8) & 0x7F) << 15 | (30 << 10) | (31 << 5) | 29); // stp x29, x30, [sp, #-FRAME_SIZE]!
	_stpX(e, 19, 20, 16);
	_stpX(e, 21, 22, 32);
	_stpX(e, 23, 24, 48);
	_stpX(e, 25, 26, 64);
	_stpX(e, 27, 28, 80);
	_movX(e, R_CPU, 0);
	if (c->gba) {
		_movImm64(e, R_JIT, (uintptr_t) c->jit);
		_movImm64(e, R_IWRAM, (uintptr_t) c->gba->memory.iwram);
		_movImm64(e, R_EWRAM, (uintptr_t) c->gba->memory.wram);
		_movImm64(e, R_COVER, (uintptr_t) c->jit->cover);
	}
	_ldrW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
}

static void _epilogue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_strW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldpX(e, 27, 28, 80);
	_ldpX(e, 25, 26, 64);
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

// Entering at ops[index]: take patched words from the pipeline
static void _entryStub(struct Compiler* c, unsigned index, const uint8_t* target) {
	struct Emitter* e = &c->e;
	unsigned i;
	for (i = 0; i < 2; ++i) {
		if (c->hot[index + i]) {
			_ldrW(e, 0, R_CPU, i ? OFF_PREFETCH1 : OFF_PREFETCH0);
			_strW(e, 0, R_JIT, JIT_FETCHED + 4 * (index + i));
		}
	}
	_jumpTo(c, target);
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
	uint8_t* toLookup2 = _b(e);

	// Due events run here; the frame loop decides whether to come back
	c->jit->events = e->p;
	_strW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_movX(e, 0, R_CPU);
	_movImm64(e, 16, (uintptr_t) ARMJitEvents);
	_blr(e, 16);
	_ldrW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	uint8_t* toC[8];
	_emit(e, 0x7200001F | (7 << 10)); // tst w0, #0xFF
	toC[0] = _bCond(e, A64_EQ);
	_emit(e, 0xF9000000 | ((JIT_PENDING_LINK / 8) << 10) | (R_JIT << 5) | 31); // str xzr, [x20, #pendingLink]

	_patch(toLookup, e->p);
	_patch(toLookup2, e->p);
	_emit(e, 0x39000000 | (JIT_SMC_HIT << 10) | (R_JIT << 5) | 31); // strb wzr, [x20, #smcHit]
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
	_cmpW(e, R_CYCLES, R_NEXT);
	_patch(_bCond(e, A64_GE), c->jit->events);
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
	_ldrW(e, 7, 5, ENTRY_PC);
	_cmpW(e, 7, 3);
	toC[3] = _bCond(e, A64_NE);
	_ldrbW(e, 7, 5, ENTRY_THUMB);
	_cmpW(e, 7, 2);
	toC[4] = _bCond(e, A64_NE);
	_ldrW(e, 7, R_CPU, OFF_PREFETCH0);
	_ldrW(e, 8, 5, ENTRY_OP0);
	_emit(e, 0x4A000000 | (8 << 16) | (7 << 5) | 7); // eor w7, w7, w8
	_ldrW(e, 8, 5, ENTRY_OP_MASK0);
	_emit(e, 0x6A00001F | (8 << 16) | (7 << 5)); // tst w7, w8
	toC[5] = _bCond(e, A64_NE);
	_ldrW(e, 7, R_CPU, OFF_PREFETCH1);
	_ldrW(e, 8, 5, ENTRY_OP1);
	_emit(e, 0x4A000000 | (8 << 16) | (7 << 5) | 7); // eor w7, w7, w8
	_ldrW(e, 8, 5, ENTRY_OP_MASK1);
	_emit(e, 0x6A00001F | (8 << 16) | (7 << 5)); // tst w7, w8
	toC[6] = _bCond(e, A64_NE);
	_ldrX(e, 1, R_JIT, JIT_PENDING_LINK);
	uint8_t* noLink = _emitSite(e, 0xB4000000 | 1); // cbz x1
	_movX(e, R_WB, 5);
	_movX(e, 0, R_JIT);
	_movX(e, 2, 5);
	_movImm64(e, 16, (uintptr_t) ARMJitLink);
	_blr(e, 16);
	_movX(e, 5, R_WB);
	_patch(noLink, e->p);
	_ldrX(e, 16, 5, ENTRY_CODE);
	_emit(e, 0xD61F0000 | (16 << 5)); // br x16

	c->jit->toC = e->p;
	unsigned i;
	for (i = 0; i < 7; ++i) {
		_patch(toC[i], e->p);
	}
	_epilogue(c);

	// Called by a block's due exit with its state stored: runs the events, then returns there
	// unless an interrupt moved the PC, code in the block was written or the frame loop stops
	c->jit->resume = e->p;
	_ldrW(e, R_WB, R_CPU, OFF_PC);
	_movX(e, 26, 30);
	_strW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_movX(e, 0, R_CPU);
	_movImm64(e, 16, (uintptr_t) ARMJitEvents);
	_blr(e, 16);
	_ldrW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
	_emit(e, 0x7200001F | (7 << 10)); // tst w0, #0xFF
	_patch(_bCond(e, A64_EQ), c->jit->toC);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_cmpW(e, 0, R_WB);
	_patch(_bCond(e, A64_NE), c->jit->dispatch);
	_ldrbW(e, 0, R_JIT, JIT_SMC_HIT);
	_patch(_cbnzW(e, 0), c->jit->dispatch);
	_emit(e, 0xD65F0000 | (26 << 5)); // ret x26
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

// Copy a patched word as it is now into jit->fetched
static void _fetchWord(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
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

// A patched word two instructions ahead, when the GBA's pipeline would fetch it
static void _fetchAhead(struct Compiler* c, unsigned i) {
	_fetchWord(c, i + 2);
}

static void _addCyclesReg(struct Compiler* c, int reg, uint32_t constant) {
	struct Emitter* e = &c->e;
	if (reg >= 0) {
		_addW(e, R_CYCLES, R_CYCLES, reg);
	}
	if (constant) {
		_addWImm(e, R_CYCLES, R_CYCLES, constant);
	}
}

static void _addCycles(struct Compiler* c, uint32_t constant) {
	_addCyclesReg(c, -1, constant);
}

static void _eventCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_cmpW(e, R_CYCLES, R_NEXT);
	_exitAt(c, _bCond(e, A64_GE), index < 0 ? index : index | EXIT_DUE);
}

// Jumps to the returned site when an event comes due before the last instruction of a run
static uint8_t* _segmentCheck(struct Compiler* c, uint32_t cycles) {
	struct Emitter* e = &c->e;
	_addWImm(e, 0, R_CYCLES, cycles);
	_cmpW(e, 0, R_NEXT);
	return _bCond(e, A64_GE);
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

// w3 = what GBAMemoryStall makes of a data access wait from cartridge code with prefetch on
static void _romStall(struct Compiler* c, unsigned i, int32_t wait) {
	struct Emitter* e = &c->e;
	int32_t s = c->seq16;
	int32_t n = c->nonseq16;
	uint32_t pc = c->pc + c->width * (i + 2);
	_movImm64(e, 9, (uintptr_t) &c->gba->memory.lastPrefetchedPc);
	// Fewer loads when they overlap the last prefetch
	_ldrW(e, 10, 9, 0);
	_movImm32(e, 11, pc);
	_emit(e, 0x4B000000 | (11 << 16) | (10 << 5) | 10); // sub w10, w10, w11
	_movImm32(e, 12, 0);
	_emit(e, 0x7100001F | (16 << 10) | (10 << 5)); // cmp w10, #16
	uint8_t* far = _bCond(e, 2); // b.hs
	_lsrWImm(e, 12, 10, 1);
	_patch(far, e->p);
	_movImm32(e, 10, 8);
	_emit(e, 0x4B000000 | (12 << 16) | (10 << 5) | 10); // sub w10, w10, w12
	_movImm32(e, 11, _prefetchLoads(s, wait));
	_cmpW(e, 10, 11);
	_emit(e, 0x1A800000 | (11 << 16) | (3 << 12) | (10 << 5) | 11); // csel w11, w10, w11, lo
	// lastPrefetchedPc = pc + 2 * (loads + previous - 1)
	_addW(e, 10, 11, 12);
	_addW(e, 10, 10, 10);
	_movImm32(e, 13, pc - 2);
	_addW(e, 10, 10, 13);
	_strW(e, 10, 9, 0);
	// stall = s * loads + 1; wait = max(wait, stall) - stall - (n - s)
	_movImm32(e, 13, s);
	_emit(e, 0x1B000000 | (13 << 16) | (31 << 10) | (11 << 5) | 10); // mul w10, w11, w13
	_addWImm(e, 10, 10, 1);
	_movImm32(e, 3, wait - (n - s));
	_emit(e, 0x4B000000 | (10 << 16) | (3 << 5) | 3); // sub w3, w3, w10
	_movImm32(e, 13, wait);
	_cmpW(e, 10, 13);
	uint8_t* fits = _bCond(e, 13); // b.le
	_movImm32(e, 3, s - n);
	_patch(fits, e->p);
}

// w3 = the wait of a data access to IWRAM or EWRAM
static void _dataWait(struct Compiler* c, unsigned i, int32_t wait) {
	if (c->romCode && c->prefetch) {
		_romStall(c, i, wait);
	} else {
		_movImm32(&c->e, 3, wait);
	}
}

static void _memResult(struct Compiler* c, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	if (mem->signExtend) {
		uint8_t* halfword = NULL;
		if (mem->size == 2) {
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
	if (mem->load) {
		_strW(e, 0, R_CPU, 4 * mem->rd);
	} else if (mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
	}
	_addCyclesReg(c, 3, c->memCycles);
}

static void _emitMem(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned size = mem->size;

	uint8_t* done[5] = { NULL, NULL, NULL, NULL, NULL };
	uint8_t* toSlow[3] = { NULL, NULL, NULL };
	uint8_t* checked[2] = { NULL, NULL };
	uint8_t* toCart = NULL;
	uint8_t* toEvent = NULL;
	unsigned d;

	uint8_t* fail = NULL;
	if (mem->cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, mem->cond ^ 1);
	}
	uint32_t literal;
	void* literalHost;
	int32_t literalWait;
	switch (_literal(c, mem, &literal, &literalHost, &literalWait)) {
	case LITERAL_CONSTANT:
		_movImm32(e, 0, literal);
		_movImm32(e, 3, literalWait);
		goto tail;
	case LITERAL_RAM:
		_movImm64(e, 0, (uintptr_t) literalHost);
		_ldrW(e, 0, 0, 0);
		_dataWait(c, i, literalWait);
		goto tail;
	}
	if (!load) {
		_loadSource(c, 6, _reg(mem->rd));
	}
	if (mem->runtimeOffset) {
		_ldrW(e, 1, R_JIT, JIT_FETCHED + 4 * mem->fetchedIndex);
		_ubfx(e, 9, 1, 23, 1);
		_andImm(e, 1, 1, 0, 12);
		uint8_t* up = _cbnzW(e, 9);
		_emit(e, 0x4B0003E0 | (1 << 16) | 1); // neg w1, w1
		_patch(up, e->p);
	} else if (mem->immediateOffset) {
		_movImm32(e, 1, mem->offset);
	} else {
		_loadSource(c, 3, mem->m);
		_shiftImm(c, mem->shiftType, mem->shiftAmount, false);
	}
	_loadSource(c, 4, mem->base);
	_emit(e, (mem->up || mem->runtimeOffset ? 0x0B000000 : 0x4B000000) | (1 << 16) | (4 << 5) | 5); // w5 = base +/- offset
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
		_blrC(e, 16);
		_dataWait(c, i, 1);
		checked[0] = _b(e);
		_patch(noCode, e->p);
	}
	_dataWait(c, i, load ? 2 : 1);
	done[0] = _b(e);

	_patch(toEwram, e->p);
	int32_t ewramWait = (size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16)[GBA_REGION_EWRAM];
	_emit(e, 0x7100001F | (2 << 10) | (7 << 5)); // cmp w7, #2
	toCart = _bCond(e, A64_NE);
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
		_blrC(e, 16);
		_dataWait(c, i, ewramWait + 1);
		checked[1] = _b(e);
		_patch(noCode, e->p);
	}
	_dataWait(c, i, ewramWait + (load ? 2 : 1));
	done[1] = _b(e);

	// Timer counters, without the generic load and I/O dispatch
	_patch(toCart, e->p);
	toCart = NULL;
	if (load && size == 2) {
		_emit(e, 0x7100001F | (GBA_REGION_IO << 10) | (7 << 5)); // cmp w7, #4
		uint8_t* notIo = _bCond(e, A64_NE);
		_movImm32(e, 9, 0x00FFFFF3);
		_emit(e, 0x0A000000 | (9 << 16) | (4 << 5) | 9); // and w9, w4, w9
		_emit(e, 0x7100001F | (GBA_REG_TM0CNT_LO << 10) | (9 << 5)); // cmp w9, #TM0CNT_LO
		uint8_t* notTimer = _bCond(e, A64_NE);
		_movImm64(e, 0, (uintptr_t) c->gba);
		_movW(e, 1, 4);
		_movImm64(e, 16, (uintptr_t) _readTimer);
		_blrC(e, 16);
		_dataWait(c, i, 2);
		done[4] = _b(e);
		_patch(notTimer, e->p);
		_patch(notIo, e->p);
	}

	// VRAM: stores tell the renderer about changed halfwords; byte stores stay with the handlers
	if (!c->romCode && (load || size > 1)) {
		_emit(e, 0x7100001F | (GBA_REGION_VRAM << 10) | (7 << 5)); // cmp w7, #6
		toCart = _bCond(e, A64_NE);
		_andImm(e, 8, 4, 0, 17);
		_movImm32(e, 9, GBA_SIZE_VRAM);
		_cmpW(e, 8, 9);
		toSlow[2] = _bCond(e, 2); // b.hs
		_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 15 : size == 2 ? 16 : 17);
		_movImm64(e, 10, (uintptr_t) c->gba->video.vram);
		_strW(e, 4, 31, CYCLE_SLOT + 4);
		if (load) {
			_memAccess(e, true, size, 10);
			_movW(e, R_WB, 0); // writeback is already stored for loads
		} else {
			if (size == 4) {
				_emit(e, 0xB8604800 | (8 << 16) | (10 << 5) | 9); // ldr w9, [x10, w8, uxtw]
				_cmpW(e, 9, 6);
			} else {
				_emit(e, 0x78604800 | (8 << 16) | (10 << 5) | 9); // ldrh w9, [x10, w8, uxtw]
				_andImm(e, 11, 6, 0, 16);
				_cmpW(e, 9, 11);
			}
			uint8_t* unchanged = _bCond(e, A64_EQ);
			_memAccess(e, false, size, 10);
			// Once per block, unless a tile cache wants every address
			_movImm64(e, 9, (uintptr_t) &c->gba->video.renderer);
			_ldrX(e, 9, 9, 0);
			_ldrX(e, 9, 9, offsetof(struct GBAVideoRenderer, cache));
			uint8_t* cached = _emitSite(e, 0xB5000000 | 9); // cbnz x9
			_ldrW(e, 9, 31, CYCLE_SLOT + 4);
			_ubfx(e, 9, 9, 12, 5);
			_ldrW(e, 10, R_JIT, JIT_VRAM_NOTIFIED);
			_lsrvW(e, 11, 10, 9);
			uint8_t* notified = _tbnz(e, 11, 0);
			_movImm32(e, 11, 1);
			_emit(e, 0x1AC02000 | (9 << 16) | (11 << 5) | 11); // lslv w11, w11, w9
			_orrWShift(e, 10, 10, 11, 0);
			_strW(e, 10, R_JIT, JIT_VRAM_NOTIFIED);
			_patch(cached, e->p);
			unsigned call;
			for (call = 0; call < (size == 4 ? 2 : 1); ++call) {
				_movImm64(e, 9, (uintptr_t) &c->gba->video.renderer);
				_ldrX(e, 0, 9, 0);
				_ldrW(e, 1, 31, CYCLE_SLOT + 4);
				_andImm(e, 1, 1, size == 4 ? 2 : 1, size == 4 ? 15 : 16);
				if (size == 4 && call == 0) {
					_addWImm(e, 1, 1, 2);
				}
				_ldrX(e, 16, 0, offsetof(struct GBAVideoRenderer, writeVRAM));
				_blrC(e, 16);
			}
			_patch(notified, e->p);
			_ldrW(e, 4, 31, CYCLE_SLOT + 4);
			_patch(unchanged, e->p);
		}
		_movImm64(e, 9, (uintptr_t) &c->gba->video.stallMask);
		_ldrW(e, 9, 9, 0);
		uint8_t* noStall = _cbzW(e, 9);
		_movImm64(e, 0, (uintptr_t) c->gba);
		_movW(e, 1, 4);
		_movImm32(e, 2, size);
		_movImm64(e, 16, (uintptr_t) GBAMemoryVRAMWait);
		_blrC(e, 16);
		_movW(e, 3, 0);
		_ldrW(e, 4, 31, CYCLE_SLOT + 4);
		uint8_t* waited = _b(e);
		_patch(noStall, e->p);
		_movImm32(e, 3, size == 4 ? 1 : 0);
		_patch(waited, e->p);
		_addWImm(e, 3, 3, load ? 2 : 1);
		if (load) {
			_movW(e, 0, R_WB);
		}
		done[3] = _b(e);
	}
	if (toCart) {
		_patch(toCart, e->p);
	}
	if (load) {
		// Cartridge reads: no prefetch buffer stall for these addresses; region 0x0D is EEPROM
		_emit(e, 0x51000000 | (GBA_REGION_ROM0 << 10) | (7 << 5) | 9); // sub w9, w7, #ROM0
		_emit(e, 0x7100001F | ((GBA_REGION_ROM2 - GBA_REGION_ROM0) << 10) | (9 << 5)); // cmp w9, #4
		toSlow[0] = _bCond(e, 8); // b.hi
		_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 23 : size == 2 ? 24 : 25);
		_movImm32(e, 9, c->gba->memory.romSize);
		_cmpW(e, 8, 9);
		toSlow[1] = _bCond(e, 2); // b.hs
		_movImm64(e, 10, (uintptr_t) c->gba->memory.rom);
		_memAccess(e, true, size, 10);
		char* cartWaits = size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16;
		_movImm64(e, 10, (uintptr_t) cartWaits);
		_emit(e, 0x38604800 | (7 << 16) | (10 << 5) | 3); // ldrb w3, [x10, w7, uxtw]
		_addWImm(e, 3, 3, 2);
		done[2] = _b(e);
	}

	unsigned t;
	for (t = 0; t < 3; ++t) {
		if (toSlow[t]) {
			_patch(toSlow[t], e->p);
		}
	}
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
	_blrC(e, 16);
	_ldrW(e, 3, 31, CYCLE_SLOT);

	// Only stores that called out can have hit compiled code
	if (!load) {
		unsigned k;
		for (k = 0; k < 2; ++k) {
			if (checked[k]) {
				_patch(checked[k], e->p);
			}
		}
		_memResult(c, mem);
		_smcCheck(c, i + 1);
		toEvent = _b(e);
	}
tail:
	for (d = 0; d < 5; ++d) {
		if (done[d]) {
			_patch(done[d], e->p);
		}
	}
	_memResult(c, mem);
	if (fail) {
		uint8_t* after = _b(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
	if (toEvent) {
		_patch(toEvent, e->p);
	}
	_eventCheck(c, i + 1);
}

static const uint32_t _conditionLut32[16] = {
	0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
	0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000
};

// Jumps to the returned site unless the word the pipeline fetched for ops[i], masked, is value
static uint8_t* _fetchedMismatch(struct Compiler* c, unsigned i, uint32_t mask, uint32_t value) {
	struct Emitter* e = &c->e;
	_ldrW(e, 0, R_JIT, JIT_FETCHED + 4 * i);
	if (mask != 0xFFFFFFFF) {
		_movImm32(e, 1, mask);
		_emit(e, 0x0A000000 | (1 << 16) | (0 << 5) | 0); // and w0, w0, w1
	}
	_movImm32(e, 1, value);
	_cmpW(e, 0, 1);
	return _bCond(e, A64_NE);
}

static uint8_t* _forwardJump(struct Compiler* c) {
	return _b(&c->e);
}

// A patched instruction through its handler, with the opcode the pipeline fetched
static void _emitDynamicHandler(struct Compiler* c, unsigned i) {
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
	_blrC(e, 16);
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
	_blrC(e, 16);
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
		_cmpW(e, R_CYCLES, R_NEXT);
		_exitAt(c, _bCond(e, A64_GE), EXIT_TO_C);
		c->loops[c->nLoops++] = _b(e);
	} else {
		uint32_t target;
		if (_branchTarget(c, i, &target)) {
			_cmpW(e, R_CYCLES, R_NEXT);
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

// The pipeline at a branch target: RAM words as they are now, others as compiled
static void _storeTargetPipeline(struct Compiler* c, uint32_t target) {
	struct Emitter* e = &c->e;
	_movImm32(e, 0, target + c->width);
	_strW(e, 0, R_CPU, OFF_PC);
	unsigned k;
	for (k = 0; k < 2; ++k) {
		uint32_t address = target + c->width * k;
		if (ARMJitRamWord(address) < 0) {
			_movImm32(e, 0, _opAt(c, address));
		} else {
			_movImm64(e, 0, (uintptr_t) _hostAddress(c, address));
			if (c->thumb) {
				_emit(e, 0x79400000 | (0 << 5) | 0); // ldrh w0, [x0]
			} else {
				_ldrW(e, 0, 0, 0);
			}
		}
		_strW(e, 0, R_CPU, k ? OFF_PREFETCH1 : OFF_PREFETCH0);
	}
}

// B, BL and Thumb conditional branches in the same region, as GBASetActiveRegion and
// ARMWritePC or ThumbWritePC would run them; anything else goes to the handler
static void _emitBranch(struct Compiler* c, unsigned i, const struct BranchOp* b) {
	struct Emitter* e = &c->e;
	uint8_t* notTaken = NULL;
	if (b->cond != 0xE) {
		_loadFlags(c);
		notTaken = _bCond(e, b->cond ^ 1);
	}
	uint8_t* slow[4];
	unsigned nSlow = 0;
	unsigned region = b->target >> 24;
	_movImm64(e, 9, (uintptr_t) c->gba);
	_ldrW(e, 10, 9, offsetof(struct GBA, memory.activeRegion));
	_emit(e, 0x7100001F | (region << 10) | (10 << 5)); // cmp w10, #region
	slow[nSlow++] = _bCond(e, A64_NE);
	if (region != GBA_REGION_BIOS) {
		_ldrW(e, 10, 9, offsetof(struct GBA, idleOptimization));
		_emit(e, 0x7100001F | (IDLE_LOOP_DETECT << 10) | (10 << 5)); // cmp w10, #IDLE_LOOP_DETECT
		slow[nSlow++] = _bCond(e, A64_GE);
		_ldrW(e, 10, 9, offsetof(struct GBA, idleLoop));
		_movImm32(e, 11, b->target);
		_cmpW(e, 10, 11);
		slow[nSlow++] = _bCond(e, A64_EQ);
	}
	if (b->thumbLink) {
		_ldrW(e, 10, R_CPU, 4 * ARM_LR);
		_movImm32(e, 11, b->lr);
		_cmpW(e, 10, 11);
		slow[nSlow++] = _bCond(e, A64_NE);
	}
	_movImm32(e, 10, b->target);
	_strW(e, 10, 9, offsetof(struct GBA, lastJump));
	_strW(e, 31, 9, offsetof(struct GBA, memory.lastPrefetchedPc));
	_ldrW(e, 10, R_CPU, OFF_ACTIVE_MASK);
	if (c->thumb) {
		_emit(e, 0x32000000 | (31 << 16) | (10 << 5) | 10); // orr w10, w10, #2
	} else {
		_andImm(e, 10, 10, 2, 30);
	}
	_strW(e, 10, R_CPU, OFF_ACTIVE_MASK);
	uint32_t address = c->pc + c->width * i;
	if (b->link || b->thumbLink) {
		_movImm32(e, 10, address + (b->link ? WORD_SIZE_ARM : 3));
		_strW(e, 10, R_CPU, 4 * ARM_LR);
	}
	_addCycles(c, 2 * c->aluCycles + c->memCycles);
	if (b->index >= 0) {
		_fetchWord(c, b->index);
		_fetchWord(c, b->index + 1);
		_eventCheck(c, b->index);
		_jumpTo(c, c->fast[b->index]);
	} else {
		_storeTargetPipeline(c, b->target);
		_cmpW(e, R_CYCLES, R_NEXT);
		_exitAt(c, _bCond(e, A64_GE), EXIT_TO_C);
		_linkJump(c);
	}

	uint8_t* next = NULL;
	if (notTaken) {
		_patch(notTaken, e->p);
		_addCycles(c, c->aluCycles);
		_eventCheck(c, i + 1);
		next = _b(e);
	}
	unsigned s;
	for (s = 0; s < nSlow; ++s) {
		_patch(slow[s], e->p);
	}
	_emitFallback(c, i);
	if (next) {
		_patch(next, e->p);
	}
}

// w8 = the address of the k-th word of a multiple transfer from w4, within the region
static void _multiAddress(struct Compiler* c, unsigned k, unsigned bits) {
	struct Emitter* e = &c->e;
	_addWImm(e, 8, 4, 4 * k);
	_andImm(e, 8, 8, 2, bits);
}

// LDM and STM on IWRAM or EWRAM as GBALoadMultiple and GBAStoreMultiple time them;
// other regions and stores into compiled code go to the handler
static void _emitMulti(struct Compiler* c, unsigned i, const struct MultiOp* m) {
	struct Emitter* e = &c->e;
	unsigned n = __builtin_popcount(m->list);
	uint8_t* fail = NULL;
	if (m->cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, m->cond ^ 1);
	}
	_ldrW(e, 1, R_CPU, 4 * m->rn);
	int32_t start = m->up ? (m->pre ? 4 : 0) : (m->pre ? -4 * (int32_t) n : -4 * (int32_t) n + 4);
	_movImm32(e, 2, start);
	_addW(e, 4, 1, 2);
	_movImm32(e, 2, m->up ? 4 * n : -4 * n);
	_addW(e, R_WB, 1, 2);
	_lsrWImm(e, 7, 4, 24);

	uint8_t* slow[3];
	unsigned nSlow = 0;
	uint8_t* done[2];
	unsigned region;
	for (region = 0; region < 2; ++region) {
		bool iwram = region == 0;
		_emit(e, 0x7100001F | ((iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM) << 10) | (7 << 5)); // cmp w7, #region
		uint8_t* other = _bCond(e, A64_NE);
		unsigned bits = iwram ? 13 : 16;
		int base = iwram ? R_IWRAM : R_EWRAM;
		unsigned k;
		if (!m->load) {
			// Whether any word written holds compiled code
			_movImm32(e, 10, 0);
			if (!iwram) {
				_movImm32(e, 11, ARM_JIT_IWRAM_WORDS);
			}
			for (k = 0; k < n; ++k) {
				_multiAddress(c, k, bits);
				_lsrWImm(e, 8, 8, 2);
				if (!iwram) {
					_addW(e, 8, 8, 11);
				}
				_emit(e, 0x38604800 | (8 << 16) | (R_COVER << 5) | 9); // ldrb w9, [x24, w8, uxtw]
				_orrWShift(e, 10, 10, 9, 0);
			}
			slow[nSlow++] = _cbnzW(e, 10);
		}
		unsigned r;
		k = 0;
		for (r = 0; r < 16; ++r) {
			if (!(m->list & (1 << r))) {
				continue;
			}
			_multiAddress(c, k, bits);
			if (m->load) {
				_emit(e, 0xB8604800 | (8 << 16) | (base << 5) | 9); // ldr w9, [base, w8, uxtw]
				_strW(e, 9, R_CPU, 4 * r);
			} else {
				_ldrW(e, 9, R_CPU, 4 * r);
				_emit(e, 0xB8204800 | (8 << 16) | (base << 5) | 9); // str w9, [base, w8, uxtw]
			}
			++k;
		}
		const struct GBAMemory* memory = &c->gba->memory;
		unsigned dataRegion = iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM;
		int32_t wait = memory->waitstatesSeq32[dataRegion] - memory->waitstatesNonseq32[dataRegion];
		wait += n * (1 + (iwram ? 0 : memory->waitstatesSeq32[GBA_REGION_EWRAM])) + (m->load ? 1 : 0);
		_dataWait(c, i, wait);
		done[region] = _b(e);
		_patch(other, e->p);
		if (!iwram) {
			slow[nSlow++] = _b(e);
		}
	}
	_patch(done[0], e->p);
	_patch(done[1], e->p);
	if (m->writeback) {
		_strW(e, R_WB, R_CPU, 4 * m->rn);
	}
	_addCyclesReg(c, 3, c->memCycles);
	if (fail) {
		uint8_t* after = _b(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
	_eventCheck(c, i + 1);
	uint8_t* next = _b(e);
	unsigned s;
	for (s = 0; s < nSlow; ++s) {
		_patch(slow[s], e->p);
	}
	_emitFallback(c, i);
	_patch(next, e->p);
}

// MUL, MLA and Thumb MUL, timed as ARM_WAIT_SMUL and the stall handler time them
static void _emitMul(struct Compiler* c, unsigned i, const struct MulOp* m) {
	struct Emitter* e = &c->e;
	uint8_t* fail = NULL;
	if (m->cond != 0xE) {
		_loadFlags(c);
		fail = _bCond(e, m->cond ^ 1);
	}
	// One more cycle for each significant byte past the first, counting leading ones as zeros
	_ldrW(e, 1, R_CPU, 4 * m->rs);
	_asrWImm(e, 9, 1, 31);
	_emit(e, 0x4A000000 | (1 << 16) | (9 << 5) | 9); // eor w9, w9, w1
	_movImm32(e, 3, m->rn >= 0 ? 2 : 1);
	unsigned k;
	for (k = 1; k < 4; ++k) {
		_lsrWImm(e, 10, 9, 8 * k);
		_emit(e, 0x7100001F | (10 << 5)); // cmp w10, #0
		_emit(e, 0x1A800400 | (3 << 16) | (0 << 12) | (3 << 5) | 3); // cinc w3, w3, ne
	}
	if (c->romCode && c->prefetch) {
		_movImm32(e, 0, c->pc + c->width * (i + 2));
		_strW(e, 0, R_CPU, OFF_PC);
		_movX(e, 0, R_CPU);
		_movW(e, 1, 3);
		_ldrX(e, 16, R_CPU, OFF_MEMORY + offsetof(struct ARMMemory, stall));
		_blrC(e, 16);
		_movW(e, 3, 0);
		_ldrW(e, 1, R_CPU, 4 * m->rs);
	}
	_ldrW(e, 2, R_CPU, 4 * m->rm);
	if (m->rn >= 0) {
		_ldrW(e, 5, R_CPU, 4 * m->rn);
		_emit(e, 0x1B000000 | (1 << 16) | (5 << 10) | (2 << 5) | 0); // madd w0, w2, w1, w5
	} else {
		_emit(e, 0x1B000000 | (1 << 16) | (31 << 10) | (2 << 5) | 0); // mul w0, w2, w1
	}
	_strW(e, 0, R_CPU, 4 * m->rd);
	if (m->s) {
		// N and Z from the result; ARM takes C from the last shifter result
		_ldrW(e, 9, R_CPU, OFF_CPSR);
		_andImm(e, 9, 9, 0, c->thumb ? 30 : 29);
		_lsrWImm(e, 10, 0, 31);
		_orrWShift(e, 9, 9, 10, 31);
		_emit(e, 0x7100001F | (0 << 5)); // cmp w0, #0
		_emit(e, 0x1A9F17E0 | 10); // cset w10, eq
		_orrWShift(e, 9, 9, 10, 30);
		if (!c->thumb) {
			_ldrW(e, 10, R_CPU, OFF_SHIFTER_CARRY);
			_andImm(e, 10, 10, 0, 1);
			_orrWShift(e, 9, 9, 10, 29);
		}
		_strW(e, 9, R_CPU, OFF_CPSR);
	}
	_addCyclesReg(c, 3, c->memCycles);
	if (fail) {
		uint8_t* after = _b(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
	_eventCheck(c, i + 1);
}


// Runs the due events, then comes back to ops[index] unless the resume routine goes elsewhere
static void _resumeAfterEvents(struct Compiler* c, unsigned index, const uint8_t* target) {
	UNUSED(index);
	uint8_t* site = _emitSite(&c->e, 0x94000000); // bl resume
	_patch(site, c->jit->resume);
	_jumpTo(c, target);
}
