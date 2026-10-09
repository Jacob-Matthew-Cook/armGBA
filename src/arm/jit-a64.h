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
// A transfer's base writeback, or a value kept across C calls
#define R_WB 23
#define R_COVER 24
#define A64_ZR 31
#define A64_SP 31
// The cycle count lives here while generated code runs
#define R_CYCLES 25
// cpu->nextEvent, reloaded whenever C code may have changed it
#define R_NEXT 27
#define R_GBA 28
#define FRAME_SIZE 112
// Stack slots: a C call's cycles and the address of the access in flight
#define CYCLE_SLOT 96
#define ADDRESS_SLOT (CYCLE_SLOT + 4)

enum {
	A64_EQ = 0,
	A64_NE = 1,
	A64_HS = 2,
	A64_LO = 3,
	A64_HI = 8,
	A64_GE = 10,
};

// Data processing on w registers, or x registers with A64_X: rd = rn op rm, with rm optionally shifted left for AND to SUBS
enum {
	A64_X = 0x80000000,
	A64_AND = 0x0A000000, A64_BIC = 0x0A200000, A64_ORR = 0x2A000000, A64_ORN = 0x2A200000, A64_EOR = 0x4A000000, A64_ANDS = 0x6A000000,
	A64_ADD = 0x0B000000, A64_ADDS = 0x2B000000, A64_SUB = 0x4B000000, A64_SUBS = 0x6B000000,
	A64_ADC = 0x1A000000, A64_ADCS = 0x3A000000, A64_SBC = 0x5A000000, A64_SBCS = 0x7A000000,
	A64_LSLV = 0x1AC02000, A64_LSRV = 0x1AC02400, A64_RORV = 0x1AC02C00,
};

// Immediate forms: a 12-bit value for add and subtract, a bitmask for the logical ones
enum {
	A64_ADD_IMM = 0x11000000, A64_ADDS_IMM = 0x31000000, A64_SUB_IMM = 0x51000000, A64_SUBS_IMM = 0x71000000,
	A64_AND_IMM = 0x12000000, A64_ORR_IMM = 0x32000000, A64_EOR_IMM = 0x52000000, A64_ANDS_IMM = 0x72000000,
};

// Loads and stores at [rn + rm], rm zero-extended (UXTW) or 64-bit (LSL), optionally scaled by the size
enum {
	A64_LDRB_R = 0x38600800, A64_LDRH_R = 0x78600800, A64_LDR_R = 0xB8600800, A64_LDRX_R = 0xF8600800,
	A64_STRB_R = 0x38200800, A64_STRH_R = 0x78200800, A64_STR_R = 0xB8200800,
	A64_UXTW = 0x4000, A64_LSL = 0x6000, A64_SCALED = 0x1000,
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

static uint32_t _ror(uint32_t value, unsigned amount, unsigned size) {
	uint32_t mask = size == 32 ? 0xFFFFFFFF : (1u << size) - 1;
	return amount ? ((value >> amount) | (value << (size - amount))) & mask : value;
}

// immr and imms of a logical immediate: a rotated run of ones repeated in 2 to 32-bit elements
static bool _bitmaskImm(uint32_t value, uint32_t* fields) {
	if (!value || value == 0xFFFFFFFF) {
		return false;
	}
	unsigned size = 32;
	while (size > 2 && ((value >> (size / 2)) & ((1u << (size / 2)) - 1)) == (value & ((1u << (size / 2)) - 1))) {
		size /= 2;
	}
	uint32_t element = value & (size == 32 ? 0xFFFFFFFF : (1u << size) - 1);
	unsigned ones = __builtin_popcount(element);
	unsigned rotate;
	for (rotate = 0; rotate < size; ++rotate) {
		if (_ror(element, rotate, size) == (1u << ones) - 1) {
			*fields = (((size - rotate) % size) << 16) | ((((~(size - 1)) << 1) & 0x3F) | (ones - 1)) << 10;
			return true;
		}
	}
	return false;
}

static void _dp(struct Emitter* e, uint32_t op, int rd, int rn, int rm, unsigned lsl) {
	_emit(e, op | (rm << 16) | (lsl << 10) | (rn << 5) | rd);
}

static void _dpImm(struct Emitter* e, uint32_t op, int rd, int rn, unsigned imm) {
	_emit(e, op | (imm << 10) | (rn << 5) | rd);
}

// Logical immediates must be encodable bitmasks
static void _logicImm(struct Emitter* e, uint32_t op, int rd, int rn, uint32_t value) {
	uint32_t fields = 0;
	_bitmaskImm(value, &fields);
	_emit(e, op | fields | (rn << 5) | rd);
}

static void _ldstR(struct Emitter* e, uint32_t op, int rt, int rn, int rm) {
	_emit(e, op | (rm << 16) | (rn << 5) | rt);
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

static void _ldrhW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0x79400000 | ((offset >> 1) << 10) | (rn << 5) | rt);
}

static void _strbW(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0x39000000 | (offset << 10) | (rn << 5) | rt);
}

static void _ldrX(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xF9400000 | ((offset >> 3) << 10) | (rn << 5) | rt);
}

static void _strX(struct Emitter* e, int rt, int rn, unsigned offset) {
	_emit(e, 0xF9000000 | ((offset >> 3) << 10) | (rn << 5) | rt);
}

static void _stpX(struct Emitter* e, int rt, int rt2, int offset) {
	_emit(e, 0xA9000000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _ldpX(struct Emitter* e, int rt, int rt2, int offset) {
	_emit(e, 0xA9400000 | (((offset / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

// stp rt, rt2, [sp, #-bytes]! and ldp rt, rt2, [sp], #bytes
static void _pushPair(struct Emitter* e, int rt, int rt2, int bytes) {
	_emit(e, 0xA9800000 | (((-bytes / 8) & 0x7F) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _popPair(struct Emitter* e, int rt, int rt2, int bytes) {
	_emit(e, 0xA8C00000 | ((bytes / 8) << 15) | (rt2 << 10) | (31 << 5) | rt);
}

static void _addWImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_dpImm(e, A64_ADD_IMM, rd, rn, imm);
}

static void _addXImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_emit(e, 0x91000000 | (imm << 10) | (rn << 5) | rd);
}

static void _addW(struct Emitter* e, int rd, int rn, int rm) {
	_dp(e, A64_ADD, rd, rn, rm, 0);
}

static void _cmpW(struct Emitter* e, int rn, int rm) {
	_dp(e, A64_SUBS, A64_ZR, rn, rm, 0);
}

static void _cmpWImm(struct Emitter* e, int rn, unsigned imm) {
	_dpImm(e, A64_SUBS_IMM, A64_ZR, rn, imm);
}

static void _movW(struct Emitter* e, int rd, int rm) {
	_dp(e, A64_ORR, rd, A64_ZR, rm, 0);
}

static void _movX(struct Emitter* e, int rd, int rm) {
	_emit(e, 0xAA0003E0 | (rm << 16) | rd);
}

static void _negW(struct Emitter* e, int rd, int rm) {
	_dp(e, A64_SUB, rd, A64_ZR, rm, 0);
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

static void _sxtb(struct Emitter* e, int rd, int rn) {
	_emit(e, 0x13001C00 | (rn << 5) | rd);
}

static void _sxth(struct Emitter* e, int rd, int rn) {
	_emit(e, 0x13003C00 | (rn << 5) | rd);
}

static void _madd(struct Emitter* e, int rd, int rn, int rm, int ra) {
	_emit(e, 0x1B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}

static void _csel(struct Emitter* e, int rd, int rn, int rm, int cond) {
	_emit(e, 0x1A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd);
}

// rd = cond ? rn + 1 : rn, and rd = cond ? 1 : 0
static void _cinc(struct Emitter* e, int rd, int rn, int cond) {
	_emit(e, 0x1A800400 | (rn << 16) | ((cond ^ 1) << 12) | (rn << 5) | rd);
}

static void _cset(struct Emitter* e, int rd, int cond) {
	_cinc(e, rd, A64_ZR, cond);
}

static void _mrsNzcv(struct Emitter* e, int rt) {
	_emit(e, 0xD53B4200 | rt);
}

static void _msrNzcv(struct Emitter* e, int rt) {
	_emit(e, 0xD51B4200 | rt);
}

static void _blr(struct Emitter* e, int rn) {
	_emit(e, 0xD63F0000 | (rn << 5));
}

static void _br(struct Emitter* e, int rn) {
	_emit(e, 0xD61F0000 | (rn << 5));
}

static void _ret(struct Emitter* e, int rn) {
	_emit(e, 0xD65F0000 | (rn << 5));
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

static uint8_t* _cbzX(struct Emitter* e, int rt) {
	return _emitSite(e, 0xB4000000 | rt);
}

static uint8_t* _cbnzX(struct Emitter* e, int rt) {
	return _emitSite(e, 0xB5000000 | rt);
}

static uint8_t* _tbz(struct Emitter* e, int rt, unsigned bit) {
	return _emitSite(e, 0x36000000 | (bit << 19) | rt);
}

static uint8_t* _tbnz(struct Emitter* e, int rt, unsigned bit) {
	return _emitSite(e, 0x37000000 | (bit << 19) | rt);
}

static uint8_t* _b(struct Emitter* e) {
	return _emitSite(e, 0x14000000);
}

static uint8_t* _bl(struct Emitter* e) {
	return _emitSite(e, 0x94000000);
}

// C sees and may change the cycle count and the next event
static void _blrC(struct Emitter* e, int rn) {
	_strW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_blr(e, rn);
	_ldrW(e, R_CYCLES, R_CPU, OFF_CYCLES);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
}

// Runs the due events; Z is set when the frame is done
static void _callEvents(struct Emitter* e) {
	_movX(e, 0, R_CPU);
	_movImm64(e, 16, (uintptr_t) ARMJitEvents);
	_blrC(e, 16);
	_logicImm(e, A64_ANDS_IMM, A64_ZR, 0, 0xFF);
}

static void _prologue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_pushPair(e, 29, 30, FRAME_SIZE);
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
		_movImm64(e, R_GBA, (uintptr_t) c->gba);
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
	_popPair(e, 29, 30, FRAME_SIZE);
	_ret(e, 30);
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
	_br(e, 1);

	// A link stub passes its link in x2 so the exit gets patched to the block found
	c->jit->linkDispatch = e->p;
	_strX(e, 2, R_JIT, JIT_PENDING_LINK);
	uint8_t* toLookup = _b(e);

	// Next block for the PC and mode in the CPU state, if it is compiled and nothing is due
	c->jit->dispatch = e->p;
	_strX(e, A64_ZR, R_JIT, JIT_PENDING_LINK);
	uint8_t* toLookup2 = _b(e);

	// Due events run here; the frame loop decides whether to come back
	c->jit->events = e->p;
	_callEvents(e);
	uint8_t* toC[8];
	unsigned nToC = 0;
	toC[nToC++] = _bCond(e, A64_EQ);
	_strX(e, A64_ZR, R_JIT, JIT_PENDING_LINK);

	_patch(toLookup, e->p);
	_patch(toLookup2, e->p);
	_strbW(e, A64_ZR, R_JIT, JIT_SMC_HIT);
	_ldrW(e, R_NEXT, R_CPU, OFF_NEXT_EVENT);
	_cmpW(e, R_CYCLES, R_NEXT);
	_patch(_bCond(e, A64_GE), c->jit->events);
	_ldrW(e, 2, R_CPU, OFF_EXECUTION_MODE);
	_ldrW(e, 3, R_CPU, OFF_PC);
	_dpImm(e, A64_SUB_IMM, 3, 3, 4);
	_dp(e, A64_ADD, 3, 3, 2, 1);
	_ubfx(e, 4, 3, 12, 16);
	_addXImm(e, 5, R_JIT, JIT_PAGES);
	_ldstR(e, A64_LDRX_R | A64_LSL | A64_SCALED, 5, 5, 4);
	toC[nToC++] = _cbzX(e, 5);
	_ubfx(e, 6, 3, 1, 11);
	_ldstR(e, A64_LDRX_R | A64_LSL | A64_SCALED, 5, 5, 6);
	toC[nToC++] = _cbzX(e, 5);
	_ldrW(e, 7, 5, ENTRY_PC);
	_cmpW(e, 7, 3);
	toC[nToC++] = _bCond(e, A64_NE);
	_ldrbW(e, 7, 5, ENTRY_THUMB);
	_cmpW(e, 7, 2);
	toC[nToC++] = _bCond(e, A64_NE);
	_ldrW(e, 7, R_CPU, OFF_PREFETCH0);
	_ldrW(e, 8, 5, ENTRY_OP0);
	_dp(e, A64_EOR, 7, 7, 8, 0);
	_ldrW(e, 8, 5, ENTRY_OP_MASK0);
	_dp(e, A64_ANDS, A64_ZR, 7, 8, 0);
	toC[nToC++] = _bCond(e, A64_NE);
	_ldrW(e, 7, R_CPU, OFF_PREFETCH1);
	_ldrW(e, 8, 5, ENTRY_OP1);
	_dp(e, A64_EOR, 7, 7, 8, 0);
	_ldrW(e, 8, 5, ENTRY_OP_MASK1);
	_dp(e, A64_ANDS, A64_ZR, 7, 8, 0);
	toC[nToC++] = _bCond(e, A64_NE);
	_ldrX(e, 1, R_JIT, JIT_PENDING_LINK);
	uint8_t* noLink = _cbzX(e, 1);
	_movX(e, R_WB, 5);
	_movX(e, 0, R_JIT);
	_movX(e, 2, 5);
	_movImm64(e, 16, (uintptr_t) ARMJitLink);
	_blr(e, 16);
	_movX(e, 5, R_WB);
	_patch(noLink, e->p);
	_ldrX(e, 16, 5, ENTRY_CODE);
	_br(e, 16);

	c->jit->toC = e->p;
	unsigned i;
	for (i = 0; i < nToC; ++i) {
		_patch(toC[i], e->p);
	}
	_epilogue(c);

	// Runs a due exit's events, then returns to the block unless the PC moved, its code changed or the frame ended
	c->jit->resume = e->p;
	_ldrW(e, R_WB, R_CPU, OFF_PC);
	_movX(e, 26, 30);
	_callEvents(e);
	_patch(_bCond(e, A64_EQ), c->jit->toC);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_cmpW(e, 0, R_WB);
	_patch(_bCond(e, A64_NE), c->jit->dispatch);
	_ldrbW(e, 0, R_JIT, JIT_SMC_HIT);
	_patch(_cbnzW(e, 0), c->jit->dispatch);
	_ret(e, 26);
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

// A word or halfword of IWRAM or EWRAM, through their base registers
static void _ldrRam(struct Compiler* c, int rt, uint32_t address, bool halfword) {
	struct Emitter* e = &c->e;
	int base = (address >> 24) == GBA_REGION_IWRAM ? R_IWRAM : R_EWRAM;
	uint32_t offset = address & (base == R_IWRAM ? GBA_SIZE_IWRAM - 1 : GBA_SIZE_EWRAM - 1);
	unsigned scale = halfword ? 2 : 4;
	if (offset < 0x1000 * scale && !(offset & (scale - 1))) {
		(halfword ? _ldrhW : _ldrW)(e, rt, base, offset);
	} else {
		_movImm32(e, rt, offset);
		_ldstR(e, (halfword ? A64_LDRH_R : A64_LDR_R) | A64_UXTW, rt, base, rt);
	}
}

// Copy a patched word as it is now into jit->fetched
static void _fetchWord(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	if (index > c->count + 1 || !c->hot[index]) {
		return;
	}
	_ldrRam(c, 0, c->pc + c->width * index, c->thumb);
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

static void _eventCheck(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	_cmpW(e, R_CYCLES, R_NEXT);
	_exitAt(c, _bCond(e, A64_GE), index | EXIT_DUE);
}

// A conditional instruction that does not run still takes its fetch
static void _skipped(struct Compiler* c, uint8_t* fail) {
	struct Emitter* e = &c->e;
	if (fail) {
		uint8_t* after = _b(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
}

// Jumps to the returned site when an event comes due before the last instruction of a run
static uint8_t* _runCheck(struct Compiler* c, uint32_t cycles) {
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

// Host NZCV = guest NZCV, using w9 and w10
static void _loadFlags(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_ldrW(e, 9, R_CPU, OFF_CPSR);
	_andImm(e, 10, 9, 28, 4);
	_msrNzcv(e, 10);
}

static void _loadCarry(struct Compiler* c, int rd) {
	_ldrW(&c->e, 9, R_CPU, OFF_CPSR);
	_ubfx(&c->e, rd, 9, 29, 1);
}

// Barrel shifter for an immediate shift amount on w3: value in w1, carry out in w2 if wanted
static void _shiftImm(struct Compiler* c, unsigned type, unsigned amount, bool carry) {
	struct Emitter* e = &c->e;
	switch (type) {
	case SHIFT_LSL:
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
	case SHIFT_LSR:
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
	case SHIFT_ASR:
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
	case SHIFT_ROR:
		if (!amount) {
			_loadCarry(c, 2);
			_lsrWImm(e, 1, 3, 1);
			_dp(e, A64_ORR, 1, 1, 2, 31);
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

// The host instruction for w3 = w0 op imm, or 0 when the immediate does not fit one
static uint32_t _aluImm(unsigned opcode, bool s, uint32_t imm) {
	uint32_t fields;
	switch (opcode) {
	case ALU_ADD:
	case ALU_CMN:
	case ALU_SUB:
	case ALU_CMP: {
		bool sub = opcode == ALU_SUB || opcode == ALU_CMP;
		uint32_t op = s ? (sub ? A64_SUBS_IMM : A64_ADDS_IMM) : (sub ? A64_SUB_IMM : A64_ADD_IMM);
		if (imm < 0x1000) {
			return op | (imm << 10) | 3;
		}
		if (!(imm & 0xFFF) && imm < 0x1000000) {
			return op | (1 << 22) | ((imm >> 12) << 10) | 3;
		}
		return 0;
	}
	case ALU_AND:
	case ALU_TST:
		return _bitmaskImm(imm, &fields) ? A64_AND_IMM | fields | 3 : 0;
	case ALU_EOR:
	case ALU_TEQ:
		return _bitmaskImm(imm, &fields) ? A64_EOR_IMM | fields | 3 : 0;
	case ALU_ORR:
		return _bitmaskImm(imm, &fields) ? A64_ORR_IMM | fields | 3 : 0;
	case ALU_BIC:
		return _bitmaskImm(~imm, &fields) ? A64_AND_IMM | fields | 3 : 0;
	default:
		return 0;
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

	// Both registers load before either is used, so the in-order core waits on one load at most
	if (!alu->immediate) {
		_loadSource(c, 3, alu->m);
	}
	if (opcode != ALU_MOV && opcode != ALU_MVN) {
		_loadSource(c, 0, alu->n);
	}
	// Immediates go into the host instruction when they fit
	uint32_t hostImm = alu->immediate ? _aluImm(opcode, s, alu->imm) : 0;
	if (alu->immediate) {
		if (opcode == ALU_MOV || opcode == ALU_MVN) {
			_movImm32(e, 3, opcode == ALU_MOV ? alu->imm : ~alu->imm);
		} else if (!hostImm) {
			_movImm32(e, 1, alu->imm);
		}
		if (carryOut) {
			if (alu->immCarry >= 0) {
				_movImm32(e, 2, alu->immCarry);
			} else {
				_loadCarry(c, 2);
			}
		}
	} else {
		_shiftImm(c, alu->shiftType, alu->shiftAmount, carryOut);
	}
	if (carryIn) {
		_loadFlags(c);
	}

	if (hostImm) {
		_emit(e, hostImm);
	} else switch (opcode) {
	case ALU_AND:
	case ALU_TST:
		_dp(e, A64_AND, 3, 0, 1, 0);
		break;
	case ALU_EOR:
	case ALU_TEQ:
		_dp(e, A64_EOR, 3, 0, 1, 0);
		break;
	case ALU_ORR:
		_dp(e, A64_ORR, 3, 0, 1, 0);
		break;
	case ALU_BIC:
		_dp(e, A64_BIC, 3, 0, 1, 0);
		break;
	case ALU_MOV:
		if (!alu->immediate) {
			_movW(e, 3, 1);
		}
		break;
	case ALU_MVN:
		if (!alu->immediate) {
			_dp(e, A64_ORN, 3, A64_ZR, 1, 0);
		}
		break;
	case ALU_ADD:
	case ALU_CMN:
		_dp(e, s ? A64_ADDS : A64_ADD, 3, 0, 1, 0);
		break;
	case ALU_SUB:
	case ALU_CMP:
		_dp(e, s ? A64_SUBS : A64_SUB, 3, 0, 1, 0);
		break;
	case ALU_RSB:
		_dp(e, s ? A64_SUBS : A64_SUB, 3, 1, 0, 0);
		break;
	case ALU_ADC:
		_dp(e, s ? A64_ADCS : A64_ADC, 3, 0, 1, 0);
		break;
	case ALU_SBC:
		_dp(e, s ? A64_SBCS : A64_SBC, 3, 0, 1, 0);
		break;
	case ALU_RSC:
		_dp(e, s ? A64_SBCS : A64_SBC, 3, 1, 0, 0);
		break;
	}

	if (setFlags) {
		if (logical) {
			// N and Z from the result, C from the shifter, V and bits 24-27 kept
			_ldrW(e, 9, R_CPU, OFF_CPSR);
			_andImm(e, 9, 9, 0, 29);
			_andImm(e, 4, 3, 31, 1);
			_dp(e, A64_ORR, 9, 9, 4, 0);
			_cmpWImm(e, 3, 0);
			_cset(e, 4, A64_EQ);
			_dp(e, A64_ORR, 9, 9, 4, 30);
			_dp(e, A64_ORR, 9, 9, 2, 29);
		} else {
			_mrsNzcv(e, 4);
			_ldrW(e, 9, R_CPU, OFF_CPSR);
			// Add and subtract clear the whole flags byte; SBC and RSC keep bits 24-27
			if (carryIn && opcode != ALU_ADC) {
				_andImm(e, 9, 9, 0, 28);
			} else {
				_andImm(e, 9, 9, 0, 24);
			}
			_dp(e, A64_ORR, 9, 9, 4, 0);
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

// Access at [base + w8]; loads land in w0, rotated like the GBA by the address in w4; stores take w6
static void _memAccess(struct Emitter* e, bool load, unsigned size, int base) {
	static const uint32_t loads[] = { 0, A64_LDRB_R, A64_LDRH_R, 0, A64_LDR_R };
	static const uint32_t stores[] = { 0, A64_STRB_R, A64_STRH_R, 0, A64_STR_R };
	if (load) {
		_ldstR(e, loads[size] | A64_UXTW, 0, base, 8);
		if (size > 1) {
			_andImm(e, 9, 4, 0, size == 4 ? 2 : 1);
			_lslWImm(e, 9, 9, 3);
			_dp(e, A64_RORV, 0, 0, 9, 0);
		}
	} else {
		_ldstR(e, stores[size] | A64_UXTW, 6, base, 8);
	}
}

// w3 = what GBAMemoryStall makes of a data access wait from cartridge code with prefetch on
static void _romStall(struct Compiler* c, unsigned i, int32_t wait) {
	struct Emitter* e = &c->e;
	int32_t s = c->seq16;
	int32_t n = c->nonseq16;
	uint32_t pc = c->pc + c->width * (i + 2);
	uint64_t advances;
	uint64_t stalls;
	_stallTables(s, wait, &advances, &stalls);
	_ldrW(e, 10, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	// The tables are built while the load is in flight
	_movImm64(e, 13, advances);
	_movImm64(e, 14, stalls);
	_movImm32(e, 11, pc);
	_dp(e, A64_SUB, 10, 10, 11, 0);
	// w12 = 8 * previous loads, or 0 when the last prefetch is not 0 to 14 bytes ahead
	_ubfx(e, 12, 10, 1, 3);
	_lslWImm(e, 12, 12, 3);
	_cmpWImm(e, 10, 16);
	_csel(e, 12, 12, A64_ZR, A64_LO);
	// lastPrefetchedPc = pc + 2 * (loads + previous - 1)
	_dp(e, A64_LSRV | A64_X, 13, 13, 12, 0);
	_andImm(e, 13, 13, 0, 8);
	_dp(e, A64_ADD, 13, 11, 13, 1);
	_strW(e, 13, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	// max(wait, stall) - stall - (n - s), as wait - (n - s) - min(wait, stall)
	_dp(e, A64_LSRV | A64_X, 14, 14, 12, 0);
	_sxtb(e, 14, 14);
	int32_t adjust = wait - (n - s);
	_dpImm(e, adjust >= 0 ? A64_ADD_IMM : A64_SUB_IMM, 3, 14, adjust >= 0 ? adjust : -adjust);
}

// w3 = a data access wait, through GBAMemoryStall when cartridge code runs with prefetch on
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
			_ldrW(e, 9, A64_SP, ADDRESS_SLOT);
			halfword = _tbz(e, 9, 0);
		}
		_sxtb(e, 0, 0);
		if (halfword) {
			uint8_t* extended = _b(e);
			_patch(halfword, e->p);
			_sxth(e, 0, 0);
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

// IWRAM or EWRAM, wait in w3; a store into compiled code takes the returned site with its word in w9
static uint8_t* _memRam(struct Compiler* c, unsigned i, const struct MemOp* mem, bool ewram) {
	struct Emitter* e = &c->e;
	unsigned lsb = mem->size == 4 ? 2 : mem->size == 2 ? 1 : 0;
	_andImm(e, 8, 4, lsb, (ewram ? 18 : 15) - lsb);
	_memAccess(e, mem->load, mem->size, ewram ? R_EWRAM : R_IWRAM);
	uint8_t* smc = NULL;
	if (!mem->load) {
		_lsrWImm(e, 9, 8, 2);
		if (ewram) {
			_movImm32(e, 10, ARM_JIT_IWRAM_WORDS);
			_addW(e, 9, 9, 10);
		}
		_ldstR(e, A64_LDRB_R | A64_UXTW, 10, R_COVER, 9);
		smc = _cbnzW(e, 10);
	}
	_dataWait(c, i, _ramWait(c, mem, ewram) + (mem->load ? 2 : 1));
	return smc;
}

static void _memInvalidate(struct Compiler* c, unsigned i, const struct MemOp* mem, bool ewram) {
	struct Emitter* e = &c->e;
	_movX(e, 0, R_JIT);
	_movW(e, 1, 9);
	_movImm64(e, 16, (uintptr_t) ARMJitInvalidateWord);
	_blrC(e, 16);
	_dataWait(c, i, _ramWait(c, mem, ewram) + 1);
}

// Timer counters, without the generic load and I/O dispatch
static void _memTimer(struct Compiler* c, unsigned i, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	_cmpWImm(e, 7, GBA_REGION_IO);
	*miss = _bCond(e, A64_NE);
	_movImm32(e, 9, 0x00FFFFF3);
	_dp(e, A64_AND, 9, 4, 9, 0);
	_cmpWImm(e, 9, GBA_REG_TM0CNT_LO);
	*slow = _bCond(e, A64_NE);
	_movX(e, 0, R_GBA);
	_movW(e, 1, 4);
	_movImm64(e, 16, (uintptr_t) _readTimer);
	_blrC(e, 16);
	_dataWait(c, i, 2);
}

// VRAM: stores tell the renderer about changed halfwords; byte stores stay with the handlers
static void _memVram(struct Compiler* c, const struct MemOp* mem, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned size = mem->size;
	_cmpWImm(e, 7, GBA_REGION_VRAM);
	*miss = _bCond(e, A64_NE);
	_andImm(e, 8, 4, 0, 17);
	_movImm32(e, 9, GBA_SIZE_VRAM);
	_cmpW(e, 8, 9);
	*slow = _bCond(e, A64_HS);
	_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 15 : size == 2 ? 16 : 17);
	_ldrX(e, 10, R_GBA, offsetof(struct GBA, video.vram));
	_strW(e, 4, A64_SP, ADDRESS_SLOT);
	if (load) {
		_memAccess(e, true, size, 10);
		_movW(e, R_WB, 0); // writeback is already stored for loads
	} else {
		if (size == 4) {
			_ldstR(e, A64_LDR_R | A64_UXTW, 9, 10, 8);
			_cmpW(e, 9, 6);
		} else {
			_ldstR(e, A64_LDRH_R | A64_UXTW, 9, 10, 8);
			_andImm(e, 11, 6, 0, 16);
			_cmpW(e, 9, 11);
		}
		uint8_t* unchanged = _bCond(e, A64_EQ);
		_memAccess(e, false, size, 10);
		// Once per 4 KiB of VRAM between events, unless a tile cache wants every address
		_ldrX(e, 9, R_GBA, offsetof(struct GBA, video.renderer));
		_ldrX(e, 9, 9, offsetof(struct GBAVideoRenderer, cache));
		uint8_t* cached = _cbnzX(e, 9);
		_ldrW(e, 9, A64_SP, ADDRESS_SLOT);
		_ubfx(e, 9, 9, 12, 5);
		_ldrW(e, 10, R_JIT, JIT_VRAM_NOTIFIED);
		_dp(e, A64_LSRV, 11, 10, 9, 0);
		uint8_t* notified = _tbnz(e, 11, 0);
		_movImm32(e, 11, 1);
		_dp(e, A64_LSLV, 11, 11, 9, 0);
		_dp(e, A64_ORR, 10, 10, 11, 0);
		_strW(e, 10, R_JIT, JIT_VRAM_NOTIFIED);
		_patch(cached, e->p);
		unsigned call;
		for (call = 0; call < (size == 4 ? 2 : 1); ++call) {
			_ldrX(e, 0, R_GBA, offsetof(struct GBA, video.renderer));
			_ldrW(e, 1, A64_SP, ADDRESS_SLOT);
			_andImm(e, 1, 1, size == 4 ? 2 : 1, size == 4 ? 15 : 16);
			if (size == 4 && call == 0) {
				_addWImm(e, 1, 1, 2);
			}
			_ldrX(e, 16, 0, offsetof(struct GBAVideoRenderer, writeVRAM));
			_blrC(e, 16);
		}
		_patch(notified, e->p);
		_ldrW(e, 4, A64_SP, ADDRESS_SLOT);
		_patch(unchanged, e->p);
	}
	_ldrW(e, 9, R_GBA, offsetof(struct GBA, video.stallMask));
	uint8_t* noStall = _cbzW(e, 9);
	_movX(e, 0, R_GBA);
	_movW(e, 1, 4);
	_movImm32(e, 2, size);
	_movImm64(e, 16, (uintptr_t) GBAMemoryVRAMWait);
	_blrC(e, 16);
	_movW(e, 3, 0);
	uint8_t* waited = _b(e);
	_patch(noStall, e->p);
	_movImm32(e, 3, size == 4 ? 1 : 0);
	_patch(waited, e->p);
	_addWImm(e, 3, 3, load ? 2 : 1);
	if (load) {
		_movW(e, 0, R_WB);
	}
}

// Cartridge reads: no prefetch buffer stall for these addresses; region 0x0D is EEPROM
static void _memCart(struct Compiler* c, const struct MemOp* mem, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	unsigned size = mem->size;
	_dpImm(e, A64_SUB_IMM, 9, 7, GBA_REGION_ROM0);
	_cmpWImm(e, 9, (GBA_REGION_ROM2 - GBA_REGION_ROM0));
	*miss = _bCond(e, A64_HI);
	_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 23 : size == 2 ? 24 : 25);
	_movImm32(e, 9, c->gba->memory.romSize);
	_cmpW(e, 8, 9);
	*slow = _bCond(e, A64_HS);
	_ldrX(e, 10, R_GBA, offsetof(struct GBA, memory.rom));
	_memAccess(e, true, size, 10);
	_addXImm(e, 10, R_GBA, size == 4 ? offsetof(struct GBA, memory.waitstatesNonseq32) : offsetof(struct GBA, memory.waitstatesNonseq16));
	_ldstR(e, A64_LDRB_R | A64_UXTW, 3, 10, 7);
	_addWImm(e, 3, 3, 2);
}

// Everything else goes through the memory handlers
static void _memSlow(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	unsigned size = mem->size;
	_storeState(c, i + 1);
	_strW(e, A64_ZR, A64_SP, CYCLE_SLOT);
	_movX(e, 0, R_CPU);
	_movW(e, 1, 4);
	unsigned offset;
	if (mem->load) {
		_addXImm(e, 2, A64_SP, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, load32) : size == 2 ? offsetof(struct ARMMemory, load16) : offsetof(struct ARMMemory, load8);
	} else {
		_movW(e, 2, 6);
		_addXImm(e, 3, A64_SP, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, store32) : size == 2 ? offsetof(struct ARMMemory, store16) : offsetof(struct ARMMemory, store8);
	}
	_ldrX(e, 16, R_CPU, OFF_MEMORY + offset);
	_blrC(e, 16);
	_ldrW(e, 3, A64_SP, CYCLE_SLOT);
	if (!mem->load) {
		// The interpreter charges a store N - S as the store leaves them, not as compiled
		unsigned nonseq;
		unsigned seq;
		_fetchWaits(c, &nonseq, &seq);
		_ldrW(e, 9, R_CPU, nonseq);
		_ldrW(e, 10, R_CPU, seq);
		_dp(e, A64_SUB, 9, 9, 10, 0);
		_dp(e, A64_ADD, 3, 3, 9, 0);
		_dpImm(e, A64_SUB_IMM, 3, 3, c->memCycles - c->aluCycles);
	}
}

static void _emitMem(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned k = c->nCold;
	bool cold = false;

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
		_ldrRam(c, 0, mem->up ? mem->base.value + mem->offset : mem->base.value - mem->offset, false);
		_dataWait(c, i, literalWait);
		goto tail;
	}
	_loadSource(c, 4, mem->base);
	if (!load) {
		_loadSource(c, 6, _reg(mem->rd));
	}
	if (mem->runtimeOffset) {
		_ldrW(e, 1, R_JIT, JIT_FETCHED + 4 * mem->fetchedIndex);
		_ubfx(e, 9, 1, 23, 1);
		_andImm(e, 1, 1, 0, 12);
		uint8_t* up = _cbnzW(e, 9);
		_negW(e, 1, 1);
		_patch(up, e->p);
	} else if (mem->immediateOffset) {
		_movImm32(e, 1, mem->offset);
	} else {
		_loadSource(c, 3, mem->m);
		_shiftImm(c, mem->shiftType, mem->shiftAmount, false);
	}
	_dp(e, mem->up || mem->runtimeOffset ? A64_ADD : A64_SUB, 5, 4, 1, 0);
	if (mem->writeback) {
		_movW(e, R_WB, 5);
	}
	if (mem->pre) {
		_movW(e, 4, 5);
	}
	if (load && mem->writeback) {
		_strW(e, R_WB, R_CPU, 4 * mem->base.reg);
	}
	if (mem->signExtend && mem->size == 2) {
		_strW(e, 4, A64_SP, ADDRESS_SLOT); // the address decides how LDRSH extends
	}

	cold = true;
	c->cold[k].index = i;
	c->cold[k].mem = *mem;
	c->cold[k].miss[0] = NULL;
	c->cold[k].miss[1] = NULL;
	c->cold[k].smc = NULL;
	_lsrWImm(e, 7, 4, 24);
	switch (_memPath(c, mem)) {
	case PATH_IWRAM:
	case PATH_EWRAM:
		c->cold[k].smcEwram = _memPath(c, mem) == PATH_EWRAM;
		_cmpWImm(e, 7, (c->cold[k].smcEwram ? GBA_REGION_EWRAM : GBA_REGION_IWRAM));
		c->cold[k].miss[0] = _bCond(e, A64_NE);
		c->cold[k].smc = _memRam(c, i, mem, c->cold[k].smcEwram);
		break;
	case PATH_VRAM:
		_memVram(c, mem, &c->cold[k].miss[0], &c->cold[k].miss[1]);
		break;
	case PATH_CART:
		_memCart(c, mem, &c->cold[k].miss[0], &c->cold[k].miss[1]);
		break;
	default:
		c->cold[k].miss[0] = _b(e);
		break;
	}

tail:
	if (cold) {
		c->cold[k].done = e->p;
	}
	_memResult(c, mem);
	_skipped(c, fail);
	if (cold) {
		c->cold[k].toEvent = e->p;
		++c->nCold;
	}
	_eventCheck(c, i + 1);
}

// After the block: every region in turn, then the handlers; stores check for compiled code before returning
static void _emitMemCold(struct Compiler* c, unsigned k) {
	struct Emitter* e = &c->e;
	unsigned i = c->cold[k].index;
	const struct MemOp* mem = &c->cold[k].mem;
	bool load = mem->load;
	uint8_t* toChecked[3];
	unsigned nChecked = 0;
	uint8_t* toSlow[4];
	unsigned nSlow = 0;
	uint8_t* miss;
	uint8_t* smc;
	unsigned x;

	if (c->cold[k].smc) {
		_patch(c->cold[k].smc, e->p);
		_memInvalidate(c, i, mem, c->cold[k].smcEwram);
		toChecked[nChecked++] = _b(e);
	}
	for (x = 0; x < 2; ++x) {
		if (c->cold[k].miss[x]) {
			_patch(c->cold[k].miss[x], e->p);
		}
	}
	for (x = 0; x < 2; ++x) {
		_cmpWImm(e, 7, (x ? GBA_REGION_EWRAM : GBA_REGION_IWRAM));
		miss = _bCond(e, A64_NE);
		smc = _memRam(c, i, mem, x);
		_jumpTo(c, c->cold[k].done);
		if (smc) {
			_patch(smc, e->p);
			_memInvalidate(c, i, mem, x);
			toChecked[nChecked++] = _b(e);
		}
		_patch(miss, e->p);
	}
	if (load && mem->size == 2) {
		_memTimer(c, i, &miss, &toSlow[nSlow++]);
		_jumpTo(c, c->cold[k].done);
		_patch(miss, e->p);
	}
	if (!c->romCode && (load || mem->size > 1)) {
		_memVram(c, mem, &miss, &toSlow[nSlow++]);
		_jumpTo(c, c->cold[k].done);
		_patch(miss, e->p);
	}
	if (load) {
		_memCart(c, mem, &toSlow[nSlow], &toSlow[nSlow + 1]);
		nSlow += 2;
		_jumpTo(c, c->cold[k].done);
	}
	for (x = 0; x < nSlow; ++x) {
		_patch(toSlow[x], e->p);
	}
	_memSlow(c, i, mem);
	if (load) {
		_jumpTo(c, c->cold[k].done);
		return;
	}
	for (x = 0; x < nChecked; ++x) {
		_patch(toChecked[x], e->p);
	}
	_memResult(c, mem);
	_smcCheck(c, i + 1);
	_jumpTo(c, c->cold[k].toEvent);
}

static void _emitColdPaths(struct Compiler* c) {
	unsigned k;
	for (k = 0; k < c->nCold; ++k) {
		_emitMemCold(c, k);
	}
}

// Jumps to the returned site unless the word the pipeline fetched for ops[i], masked, is value
static uint8_t* _fetchedMismatch(struct Compiler* c, unsigned i, uint32_t mask, uint32_t value) {
	struct Emitter* e = &c->e;
	_ldrW(e, 0, R_JIT, JIT_FETCHED + 4 * i);
	if (mask != 0xFFFFFFFF) {
		_movImm32(e, 1, mask);
		_dp(e, A64_AND, 0, 0, 1, 0);
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
		_movImm64(e, 11, (uintptr_t) _conditionLut);
		_ldstR(e, A64_LDRH_R | A64_UXTW | A64_SCALED, 11, 11, 10);
		_dp(e, A64_LSRV, 11, 11, 9, 0);
		uint8_t* toExec = _tbnz(e, 11, 0);
		_addCycles(c, c->aluCycles);
		toCheck = _b(e);
		_patch(toExec, e->p);
		_ubfx(e, 12, 1, 20, 8);
		_ubfx(e, 13, 1, 4, 4);
		_dp(e, A64_ORR, 12, 13, 12, 4);
		_movImm64(e, 14, (uintptr_t) _armTable);
	}
	_ldstR(e, A64_LDRX_R | A64_UXTW | A64_SCALED, 16, 14, 12);
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
	unsigned cond = _condition(op, c->thumb);

	_storeState(c, i + 1);
	uint8_t* toCheck = NULL;
	if (cond != 0xE) {
		_ldrW(e, 0, R_CPU, OFF_CPSR);
		_lsrWImm(e, 0, 0, 28);
		_movImm32(e, 1, _conditionLut[cond]);
		_dp(e, A64_LSRV, 1, 1, 0, 0);
		uint8_t* toExec = _tbnz(e, 1, 0);
		_addCycles(c, c->aluCycles);
		toCheck = _b(e);
		_patch(toExec, e->p);
	}

	_movX(e, 0, R_CPU);
	_movImm32(e, 1, op);
	_movImm64(e, 16, (uintptr_t) _handler(op, c->thumb));
	_blrC(e, 16);
	_ldrW(e, 0, R_CPU, OFF_PC);
	_movImm32(e, 1, address + 2 * c->width);
	_cmpW(e, 0, 1);
	uint8_t* branched = _bCond(e, A64_NE);
	uint8_t* sequential = _b(e);

	_patch(branched, e->p);
	if (_loopsToStart(c, i)) {
		// A taken branch back to the start of this block keeps running here
		_movImm32(e, 1, c->pc + c->width);
		_cmpW(e, 0, 1);
		_exitAt(c, _bCond(e, A64_NE), EXIT_DIRECT);
		_ldrbW(e, 0, R_JIT, JIT_SMC_HIT);
		_exitAt(c, _cbnzW(e, 0), EXIT_DIRECT);
		_cmpW(e, R_CYCLES, R_NEXT);
		_exitAt(c, _bCond(e, A64_GE), EXIT_EVENTS);
		c->loops[c->nLoops++] = _b(e);
	} else {
		uint32_t target;
		if (_branchTarget(c, i, &target)) {
			_cmpW(e, R_CYCLES, R_NEXT);
			_exitAt(c, _bCond(e, A64_GE), EXIT_EVENTS);
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
			_ldrRam(c, 0, address, c->thumb);
		}
		_strW(e, 0, R_CPU, k ? OFF_PREFETCH1 : OFF_PREFETCH0);
	}
}

// Branches within a region, as GBASetActiveRegion and ARMWritePC or ThumbWritePC run them
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
	_ldrW(e, 10, R_GBA, offsetof(struct GBA, memory.activeRegion));
	_cmpWImm(e, 10, region);
	slow[nSlow++] = _bCond(e, A64_NE);
	if (region != GBA_REGION_BIOS) {
		_ldrW(e, 10, R_GBA, offsetof(struct GBA, idleOptimization));
		_cmpWImm(e, 10, IDLE_LOOP_DETECT);
		slow[nSlow++] = _bCond(e, A64_GE);
		_ldrW(e, 10, R_GBA, offsetof(struct GBA, idleLoop));
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
	_strW(e, 10, R_GBA, offsetof(struct GBA, lastJump));
	_strW(e, A64_ZR, R_GBA, offsetof(struct GBA, memory.lastPrefetchedPc));
	_ldrW(e, 10, R_CPU, OFF_ACTIVE_MASK);
	if (c->thumb) {
		_logicImm(e, A64_ORR_IMM, 10, 10, 2);
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
		_exitAt(c, _bCond(e, A64_GE), EXIT_EVENTS);
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

// LDM and STM on IWRAM or EWRAM, timed as GBALoadMultiple and GBAStoreMultiple do
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
		_cmpWImm(e, 7, (iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM));
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
				_ldstR(e, A64_LDRB_R | A64_UXTW, 9, R_COVER, 8);
				_dp(e, A64_ORR, 10, 10, 9, 0);
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
				_ldstR(e, A64_LDR_R | A64_UXTW, 9, base, 8);
				_strW(e, 9, R_CPU, 4 * r);
			} else {
				_ldrW(e, 9, R_CPU, 4 * r);
				_ldstR(e, A64_STR_R | A64_UXTW, 9, base, 8);
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
	_skipped(c, fail);
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
	_dp(e, A64_EOR, 9, 9, 1, 0);
	_movImm32(e, 3, m->rn >= 0 ? 2 : 1);
	unsigned k;
	for (k = 1; k < 4; ++k) {
		_lsrWImm(e, 10, 9, 8 * k);
		_cmpWImm(e, 10, 0);
		_cinc(e, 3, 3, A64_NE);
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
		_madd(e, 0, 2, 1, 5);
	} else {
		_madd(e, 0, 2, 1, A64_ZR);
	}
	_strW(e, 0, R_CPU, 4 * m->rd);
	if (m->s) {
		// N and Z from the result; ARM takes C from the last shifter result
		_ldrW(e, 9, R_CPU, OFF_CPSR);
		_andImm(e, 9, 9, 0, c->thumb ? 30 : 29);
		_lsrWImm(e, 10, 0, 31);
		_dp(e, A64_ORR, 9, 9, 10, 31);
		_cmpWImm(e, 0, 0);
		_cset(e, 10, A64_EQ);
		_dp(e, A64_ORR, 9, 9, 10, 30);
		if (!c->thumb) {
			_ldrW(e, 10, R_CPU, OFF_SHIFTER_CARRY);
			_andImm(e, 10, 10, 0, 1);
			_dp(e, A64_ORR, 9, 9, 10, 29);
		}
		_strW(e, 9, R_CPU, OFF_CPSR);
	}
	_addCyclesReg(c, 3, c->memCycles);
	_skipped(c, fail);
	_eventCheck(c, i + 1);
}


// Runs the due events, then comes back to target unless the resume routine goes elsewhere
static void _resumeAfterEvents(struct Compiler* c, const uint8_t* target) {
	uint8_t* site = _bl(&c->e);
	_patch(site, c->jit->resume);
	_jumpTo(c, target);
}
