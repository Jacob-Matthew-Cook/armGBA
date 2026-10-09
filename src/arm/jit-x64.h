/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// x86-64 (System V) backend for jit.c

enum {
	X_RAX, X_RCX, X_RDX, X_RBX, X_RSP, X_RBP, X_RSI, X_RDI,
	X_R8, X_R9, X_R10, X_R11, X_R12, X_R13, X_R14, X_R15
};

#define X_CPU X_RBX
#define X_JIT X_R12
#define X_IWRAM X_R13
#define X_EWRAM X_R14
#define X_WB X_R15
// The cycle count lives here while generated code runs
#define X_CYCLES X_RBP

enum {
	CC_O = 0x0,
	CC_B = 0x2,
	CC_AE = 0x3,
	CC_E = 0x4,
	CC_NE = 0x5,
	CC_S = 0x8,
	CC_GE = 0xD,
	CC_LE = 0xE,
};

enum {
	OP_ADD = 0x01,
	OP_OR = 0x09,
	OP_ADC = 0x11,
	OP_SBB = 0x19,
	OP_AND = 0x21,
	OP_SUB = 0x29,
	OP_XOR = 0x31,
	OP_CMP = 0x39,
	OP_TEST = 0x85,
	OP_MOV = 0x89,
};

enum {
	SH_ROR = 1,
	SH_SHL = 4,
	SH_SHR = 5,
	SH_SAR = 7,
};

static void _byte(struct Emitter* e, uint8_t value) {
	*e->p++ = value;
}

static void _imm32(struct Emitter* e, uint32_t value) {
	memcpy(e->p, &value, 4);
	e->p += 4;
}

static void _patch(uint8_t* at, const uint8_t* target) {
	int32_t offset = target - (at + 4);
	memcpy(at, &offset, 4);
}

static void _rex(struct Emitter* e, bool w, int reg, int index, int base) {
	uint8_t rex = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
	if (rex != 0x40) {
		_byte(e, rex);
	}
}

static void _modrmReg(struct Emitter* e, int reg, int rm) {
	_byte(e, 0xC0 | ((reg & 7) << 3) | (rm & 7));
}

// [base + disp32]
static void _modrmMem(struct Emitter* e, int reg, int base, int32_t disp) {
	_byte(e, 0x80 | ((reg & 7) << 3) | (base & 7));
	if ((base & 7) == X_RSP) {
		_byte(e, 0x24);
	}
	_imm32(e, disp);
}

// [base + index + disp32]
static void _modrmIndexDisp(struct Emitter* e, int reg, int base, int index, int32_t disp) {
	_byte(e, 0x80 | ((reg & 7) << 3) | 4);
	_byte(e, ((index & 7) << 3) | (base & 7));
	_imm32(e, disp);
}

// [base + index]
static void _modrmIndex(struct Emitter* e, int reg, int base, int index) {
	bool disp8 = (base & 7) == X_RBP;
	_byte(e, (disp8 ? 0x40 : 0x00) | ((reg & 7) << 3) | 4);
	_byte(e, ((index & 7) << 3) | (base & 7));
	if (disp8) {
		_byte(e, 0);
	}
}

// op r/m32, r32
static void _rr(struct Emitter* e, uint8_t opcode, int dst, int src) {
	_rex(e, false, src, 0, dst);
	_byte(e, opcode);
	_modrmReg(e, src, dst);
}

static void _mov64(struct Emitter* e, int dst, int src) {
	_rex(e, true, src, 0, dst);
	_byte(e, 0x89);
	_modrmReg(e, src, dst);
}

// 81 /digit id: 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp
static void _ri(struct Emitter* e, int digit, int dst, uint32_t imm) {
	_rex(e, false, 0, 0, dst);
	_byte(e, 0x81);
	_modrmReg(e, digit, dst);
	_imm32(e, imm);
}

static void _movImm(struct Emitter* e, int dst, uint32_t imm) {
	_rex(e, false, 0, 0, dst);
	_byte(e, 0xB8 | (dst & 7));
	_imm32(e, imm);
}

static void _movImm64(struct Emitter* e, int dst, uint64_t imm) {
	_rex(e, true, 0, 0, dst);
	_byte(e, 0xB8 | (dst & 7));
	memcpy(e->p, &imm, 8);
	e->p += 8;
}

static void _load(struct Emitter* e, int dst, int base, int32_t disp) {
	_rex(e, false, dst, 0, base);
	_byte(e, 0x8B);
	_modrmMem(e, dst, base, disp);
}

static void _load64(struct Emitter* e, int dst, int base, int32_t disp) {
	_rex(e, true, dst, 0, base);
	_byte(e, 0x8B);
	_modrmMem(e, dst, base, disp);
}

static void _store(struct Emitter* e, int src, int base, int32_t disp) {
	_rex(e, false, src, 0, base);
	_byte(e, 0x89);
	_modrmMem(e, src, base, disp);
}

static void _storeImm(struct Emitter* e, int base, int32_t disp, uint32_t imm) {
	_rex(e, false, 0, 0, base);
	_byte(e, 0xC7);
	_modrmMem(e, 0, base, disp);
	_imm32(e, imm);
}

// add, or, and, cmp (by digit) dword [base + disp], imm
static void _memImm(struct Emitter* e, int digit, int base, int32_t disp, uint32_t imm) {
	_rex(e, false, 0, 0, base);
	_byte(e, 0x81);
	_modrmMem(e, digit, base, disp);
	_imm32(e, imm);
}

static void _cmpRegMem(struct Emitter* e, int reg, int base, int32_t disp) {
	_rex(e, false, reg, 0, base);
	_byte(e, 0x3B);
	_modrmMem(e, reg, base, disp);
}

static void _lea64(struct Emitter* e, int dst, int base, int32_t disp) {
	_rex(e, true, dst, 0, base);
	_byte(e, 0x8D);
	_modrmMem(e, dst, base, disp);
}

static void _shift(struct Emitter* e, int digit, int dst, unsigned amount) {
	_rex(e, false, 0, 0, dst);
	_byte(e, 0xC1);
	_modrmReg(e, digit, dst);
	_byte(e, amount);
}

static void _shiftCl(struct Emitter* e, int digit, int dst) {
	_rex(e, false, 0, 0, dst);
	_byte(e, 0xD3);
	_modrmReg(e, digit, dst);
}

static void _not(struct Emitter* e, int dst) {
	_rex(e, false, 0, 0, dst);
	_byte(e, 0xF7);
	_modrmReg(e, 2, dst);
}

static void _setcc(struct Emitter* e, int cc, int dst) {
	_rex(e, false, 0, 0, dst);
	_byte(e, 0x0F);
	_byte(e, 0x90 | cc);
	_modrmReg(e, 0, dst);
}

static void _movzxByte(struct Emitter* e, int dst, int src) {
	_rex(e, false, dst, 0, src);
	_byte(e, 0x0F);
	_byte(e, 0xB6);
	_modrmReg(e, dst, src);
}

// CF = bit 29 of the guest CPSR
static void _carryToCf(struct Emitter* e) {
	_byte(e, 0x0F);
	_byte(e, 0xBA);
	_modrmMem(e, 4, X_CPU, OFF_CPSR);
	_byte(e, 29);
}

static void _call(struct Emitter* e, int reg) {
	_rex(e, false, 0, 0, reg);
	_byte(e, 0xFF);
	_modrmReg(e, 2, reg);
}

// C sees and may change the cycle count
static void _callC(struct Emitter* e, int reg) {
	_store(e, X_CYCLES, X_CPU, OFF_CYCLES);
	_call(e, reg);
	_load(e, X_CYCLES, X_CPU, OFF_CYCLES);
}

static uint8_t* _jcc(struct Emitter* e, int cc) {
	_byte(e, 0x0F);
	_byte(e, 0x80 | cc);
	uint8_t* at = e->p;
	_imm32(e, 0);
	return at;
}

static uint8_t* _jmp(struct Emitter* e) {
	_byte(e, 0xE9);
	uint8_t* at = e->p;
	_imm32(e, 0);
	return at;
}

static void _push(struct Emitter* e, int reg) {
	_rex(e, false, 0, 0, reg);
	_byte(e, 0x50 | (reg & 7));
}

static void _pop(struct Emitter* e, int reg) {
	_rex(e, false, 0, 0, reg);
	_byte(e, 0x58 | (reg & 7));
}

static void _prologue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_push(e, X_RBX);
	_push(e, X_RBP);
	_push(e, X_R12);
	_push(e, X_R13);
	_push(e, X_R14);
	_push(e, X_R15);
	_byte(e, 0x48); // sub rsp, 8: keeps calls aligned and holds the cycle slot at [rsp]
	_byte(e, 0x83);
	_byte(e, 0xEC);
	_byte(e, 0x08);
	_mov64(e, X_CPU, X_RDI);
	if (c->gba) {
		_movImm64(e, X_JIT, (uintptr_t) c->jit);
		_movImm64(e, X_IWRAM, (uintptr_t) c->gba->memory.iwram);
		_movImm64(e, X_EWRAM, (uintptr_t) c->gba->memory.wram);
	}
	_load(e, X_CYCLES, X_CPU, OFF_CYCLES);
}

static void _epilogue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_store(e, X_CYCLES, X_CPU, OFF_CYCLES);
	_byte(e, 0x48); // add rsp, 8
	_byte(e, 0x83);
	_byte(e, 0xC4);
	_byte(e, 0x08);
	_pop(e, X_R15);
	_pop(e, X_R14);
	_pop(e, X_R13);
	_pop(e, X_R12);
	_pop(e, X_RBP);
	_pop(e, X_RBX);
	_byte(e, 0xC3);
}

static void _jumpTo(struct Compiler* c, const uint8_t* target) {
	_patch(_jmp(&c->e), target);
}

// mov dst, [base + index * (1 << scale) + disp]
static void _loadScaled(struct Emitter* e, bool w, int dst, int base, int index, unsigned scale, int32_t disp) {
	_rex(e, w, dst, index, base);
	_byte(e, 0x8B);
	_byte(e, 0x84 | ((dst & 7) << 3));
	_byte(e, (scale << 6) | ((index & 7) << 3) | (base & 7));
	_imm32(e, disp);
}

static void _loadScaled64(struct Emitter* e, int dst, int base, int index, int32_t disp) {
	_loadScaled(e, true, dst, base, index, 3, disp);
}

static void _test64(struct Emitter* e, int reg) {
	_rex(e, true, reg, 0, reg);
	_byte(e, 0x85);
	_modrmReg(e, reg, reg);
}

static void _patchLink(uint8_t* site, const uint8_t* target) {
	_patch(site, target);
}

// Entering at ops[index]: take patched words from the pipeline
static void _entryStub(struct Compiler* c, unsigned index, const uint8_t* target) {
	struct Emitter* e = &c->e;
	unsigned i;
	for (i = 0; i < 2; ++i) {
		if (c->hot[index + i]) {
			_load(e, X_RAX, X_CPU, i ? OFF_PREFETCH1 : OFF_PREFETCH0);
			_store(e, X_RAX, X_JIT, JIT_FETCHED + 4 * (index + i));
		}
	}
	_jumpTo(c, target);
}

static void _linkJump(struct Compiler* c) {
	struct ARMJitLink* link = &c->block->links[c->nLinks++];
	link->site = _jmp(&c->e);
}

static void _linkStub(struct Compiler* c, struct ARMJitLink* link) {
	_movImm64(&c->e, X_RDX, (uintptr_t) link);
	_jumpTo(c, c->jit->linkDispatch);
}

static void _emitTrampoline(struct Compiler* c) {
	struct Emitter* e = &c->e;
	c->jit->enter = (void (*)(struct ARMCore*, void*)) e->p;
	_prologue(c);
	_byte(e, 0xFF); // jmp rsi
	_byte(e, 0xE6);

	// A link stub passes its link in rdx so the exit gets patched to the block found
	c->jit->linkDispatch = e->p;
	_rex(e, true, X_RDX, 0, X_JIT); // mov [r12 + pendingLink], rdx
	_byte(e, 0x89);
	_modrmMem(e, X_RDX, X_JIT, JIT_PENDING_LINK);
	uint8_t* toLookup = _jmp(e);

	// Next block for the PC and mode in the CPU state, if it is compiled and nothing is due
	c->jit->dispatch = e->p;
	_rex(e, true, 0, 0, X_JIT); // mov qword [r12 + pendingLink], 0
	_byte(e, 0xC7);
	_modrmMem(e, 0, X_JIT, JIT_PENDING_LINK);
	_imm32(e, 0);
	uint8_t* toLookup2 = _jmp(e);

	// Due events run here; the frame loop decides whether to come back
	c->jit->events = e->p;
	_store(e, X_CYCLES, X_CPU, OFF_CYCLES);
	_mov64(e, X_RDI, X_CPU);
	_movImm64(e, X_RAX, (uintptr_t) ARMJitEvents);
	_call(e, X_RAX);
	_load(e, X_CYCLES, X_CPU, OFF_CYCLES);
	_byte(e, 0x84); // test al, al
	_byte(e, 0xC0);
	uint8_t* stop = _jcc(e, CC_E);
	_rex(e, true, 0, 0, X_JIT); // mov qword [r12 + pendingLink], 0
	_byte(e, 0xC7);
	_modrmMem(e, 0, X_JIT, JIT_PENDING_LINK);
	_imm32(e, 0);

	_patch(toLookup, e->p);
	_patch(toLookup2, e->p);
	_rex(e, false, 0, 0, X_JIT); // mov byte [r12 + smcHit], 0
	_byte(e, 0xC6);
	_modrmMem(e, 0, X_JIT, JIT_SMC_HIT);
	_byte(e, 0);
	_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
	_patch(_jcc(e, CC_GE), c->jit->events);
	_load(e, X_RAX, X_CPU, OFF_EXECUTION_MODE);
	_load(e, X_RCX, X_CPU, OFF_PC);
	_ri(e, 5, X_RCX, 4);
	_rr(e, OP_ADD, X_RCX, X_RAX);
	_rr(e, OP_ADD, X_RCX, X_RAX);
	_rr(e, OP_MOV, X_RDX, X_RCX);
	_shift(e, SH_SHR, X_RDX, 12);
	_ri(e, 4, X_RDX, ARM_JIT_PAGES - 1);
	_loadScaled64(e, X_RDX, X_JIT, X_RDX, JIT_PAGES);
	_test64(e, X_RDX);
	uint8_t* noPage = _jcc(e, CC_E);
	_rr(e, OP_MOV, X_RSI, X_RCX);
	_ri(e, 4, X_RSI, 0xFFF);
	_shift(e, SH_SHR, X_RSI, 1);
	_loadScaled64(e, X_RDX, X_RDX, X_RSI, 0);
	_test64(e, X_RDX);
	uint8_t* noBlock = _jcc(e, CC_E);
	_cmpRegMem(e, X_RCX, X_RDX, ENTRY_PC);
	uint8_t* otherPc = _jcc(e, CC_NE);
	_rex(e, false, X_RSI, 0, X_RDX); // movzx esi, byte [rdx + thumb]
	_byte(e, 0x0F);
	_byte(e, 0xB6);
	_modrmMem(e, X_RSI, X_RDX, ENTRY_THUMB);
	_rr(e, OP_CMP, X_RSI, X_RAX);
	uint8_t* otherMode = _jcc(e, CC_NE);
	_load(e, X_RSI, X_CPU, OFF_PREFETCH0);
	_rex(e, false, X_RSI, 0, X_RDX); // xor esi, [rdx + op0]
	_byte(e, 0x33);
	_modrmMem(e, X_RSI, X_RDX, ENTRY_OP0);
	_rex(e, false, X_RSI, 0, X_RDX); // and esi, [rdx + opMask0]
	_byte(e, 0x23);
	_modrmMem(e, X_RSI, X_RDX, ENTRY_OP_MASK0);
	uint8_t* otherOp0 = _jcc(e, CC_NE);
	_load(e, X_RSI, X_CPU, OFF_PREFETCH1);
	_rex(e, false, X_RSI, 0, X_RDX); // xor esi, [rdx + op1]
	_byte(e, 0x33);
	_modrmMem(e, X_RSI, X_RDX, ENTRY_OP1);
	_rex(e, false, X_RSI, 0, X_RDX); // and esi, [rdx + opMask1]
	_byte(e, 0x23);
	_modrmMem(e, X_RSI, X_RDX, ENTRY_OP_MASK1);
	uint8_t* otherOp1 = _jcc(e, CC_NE);
	_load64(e, X_RSI, X_JIT, JIT_PENDING_LINK);
	_test64(e, X_RSI);
	uint8_t* noLink = _jcc(e, CC_E);
	_mov64(e, X_WB, X_RDX);
	_mov64(e, X_RDI, X_JIT);
	_movImm64(e, X_RAX, (uintptr_t) ARMJitLink);
	_call(e, X_RAX);
	_mov64(e, X_RDX, X_WB);
	_patch(noLink, e->p);
	_rex(e, false, 0, 0, X_RDX); // jmp [rdx + entry]
	_byte(e, 0xFF);
	_modrmMem(e, 4, X_RDX, ENTRY_CODE);

	c->jit->toC = e->p;
	_patch(stop, e->p);
	_patch(noPage, e->p);
	_patch(noBlock, e->p);
	_patch(otherPc, e->p);
	_patch(otherMode, e->p);
	_patch(otherOp0, e->p);
	_patch(otherOp1, e->p);
	_epilogue(c);
}

static void _exitJump(struct Compiler* c, int index) {
	_exitAt(c, _jmp(&c->e), index);
}

static void _storePrefetch(struct Compiler* c, int32_t offset, unsigned index) {
	struct Emitter* e = &c->e;
	if (c->hot[index]) {
		_load(e, X_RAX, X_JIT, JIT_FETCHED + 4 * index);
		_store(e, X_RAX, X_CPU, offset);
	} else {
		_storeImm(e, X_CPU, offset, c->ops[index]);
	}
}

// Interpreter state just before ops[index] runs
static void _storeState(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	_storeImm(e, X_CPU, OFF_PC, c->pc + c->width * (index + 1));
	_storePrefetch(c, OFF_PREFETCH0, index);
	_storePrefetch(c, OFF_PREFETCH1, index + 1);
}

// Copy a patched word as it is now into jit->fetched
static void _fetchWord(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	if (index > c->count + 1 || !c->hot[index]) {
		return;
	}
	_movImm64(e, X_RAX, (uintptr_t) _hostAddress(c, c->pc + c->width * index));
	if (c->thumb) {
		_byte(e, 0x0F); // movzx ecx, word [rax]
		_byte(e, 0xB7);
		_byte(e, 0x08);
	} else {
		_byte(e, 0x8B); // mov ecx, [rax]
		_byte(e, 0x08);
	}
	_store(e, X_RCX, X_JIT, JIT_FETCHED + 4 * index);
}

// A patched word two instructions ahead, when the GBA's pipeline would fetch it
static void _fetchAhead(struct Compiler* c, unsigned i) {
	_fetchWord(c, i + 2);
}

static void _addCycles(struct Compiler* c, uint32_t constant) {
	_ri(&c->e, 0, X_CYCLES, constant);
}

static void _eventCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
	_exitAt(c, _jcc(e, CC_GE), index < 0 ? index : index | EXIT_DUE);
}

// Jumps to the returned site when an event comes due before the last instruction of a run
static uint8_t* _segmentCheck(struct Compiler* c, uint32_t cycles) {
	struct Emitter* e = &c->e;
	_rr(e, OP_MOV, X_RAX, X_CYCLES);
	_ri(e, 0, X_RAX, cycles);
	_cmpRegMem(e, X_RAX, X_CPU, OFF_NEXT_EVENT);
	return _jcc(e, CC_GE);
}

static void _smcCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_rex(e, false, 0, 0, X_JIT);
	_byte(e, 0x80); // cmp byte [r12 + smcHit], 0
	_modrmMem(e, 7, X_JIT, JIT_SMC_HIT);
	_byte(e, 0);
	_exitAt(c, _jcc(e, CC_NE), index);
}

static void _loadSource(struct Compiler* c, int dst, struct Source source) {
	if (source.constant) {
		_movImm(&c->e, dst, source.value);
	} else {
		_load(&c->e, dst, X_CPU, 4 * source.reg);
	}
}

// Jumps to the returned site when the guest condition fails (taken = false) or passes
static uint8_t* _condJump(struct Compiler* c, unsigned cond, bool taken) {
	struct Emitter* e = &c->e;
	_load(e, X_RAX, X_CPU, OFF_CPSR);
	_shift(e, SH_SHR, X_RAX, 28);
	_movImm(e, X_RCX, _conditionLut[cond]);
	_byte(e, 0x0F); // bt ecx, eax
	_byte(e, 0xA3);
	_modrmReg(e, X_RAX, X_RCX);
	return _jcc(e, taken ? CC_B : CC_AE);
}

static void _loadCarry(struct Compiler* c, int dst) {
	_load(&c->e, dst, X_CPU, OFF_CPSR);
	_shift(&c->e, SH_SHR, dst, 29);
	_ri(&c->e, 4, dst, 1);
}

// Barrel shifter for an immediate shift amount on r11d: value in edx, carry out in r8d if wanted
static void _shiftImm(struct Compiler* c, unsigned type, unsigned amount, bool carry) {
	struct Emitter* e = &c->e;
	switch (type) {
	case 0: // LSL
		_rr(e, OP_MOV, X_RDX, X_R11);
		if (amount) {
			_shift(e, SH_SHL, X_RDX, amount);
		}
		if (carry) {
			if (amount) {
				_rr(e, OP_MOV, X_R8, X_R11);
				_shift(e, SH_SHR, X_R8, 32 - amount);
				_ri(e, 4, X_R8, 1);
			} else {
				_loadCarry(c, X_R8);
			}
		}
		break;
	case 1: // LSR
		if (amount) {
			_rr(e, OP_MOV, X_RDX, X_R11);
			_shift(e, SH_SHR, X_RDX, amount);
		} else {
			_movImm(e, X_RDX, 0);
		}
		if (carry) {
			_rr(e, OP_MOV, X_R8, X_R11);
			_shift(e, SH_SHR, X_R8, amount ? amount - 1 : 31);
			_ri(e, 4, X_R8, 1);
		}
		break;
	case 2: // ASR
		_rr(e, OP_MOV, X_RDX, X_R11);
		_shift(e, SH_SAR, X_RDX, amount ? amount : 31);
		if (carry) {
			_rr(e, OP_MOV, X_R8, X_R11);
			_shift(e, SH_SHR, X_R8, amount ? amount - 1 : 31);
			_ri(e, 4, X_R8, 1);
		}
		break;
	case 3: // ROR, RRX
		_rr(e, OP_MOV, X_RDX, X_R11);
		if (amount) {
			_shift(e, SH_ROR, X_RDX, amount);
		} else {
			_loadCarry(c, X_R10);
			_shift(e, SH_SHR, X_RDX, 1);
			_shift(e, SH_SHL, X_R10, 31);
			_rr(e, OP_OR, X_RDX, X_R10);
		}
		if (carry) {
			_rr(e, OP_MOV, X_R8, X_R11);
			if (amount) {
				_shift(e, SH_SHR, X_R8, amount - 1);
			}
			_ri(e, 4, X_R8, 1);
		}
		break;
	}
}

// storeCarry: MULS and MLAS take C from the last shifter result, so keep it for them
static void _emitAlu(struct Compiler* c, const struct AluOp* alu, bool storeCarry) {
	struct Emitter* e = &c->e;
	unsigned opcode = alu->opcode;
	bool logical = _isLogical(opcode);
	bool carryIn = opcode == ALU_ADC || opcode == ALU_SBC || opcode == ALU_RSC;
	bool setFlags = alu->s && alu->flagsLive;
	bool carryOut = (setFlags && logical) || storeCarry;
	bool borrow = opcode == ALU_SUB || opcode == ALU_RSB || opcode == ALU_CMP || opcode == ALU_SBC || opcode == ALU_RSC;

	uint8_t* skip = NULL;
	if (alu->cond != 0xE) {
		skip = _condJump(c, alu->cond, false);
	}

	if (alu->immediate) {
		_movImm(e, X_RDX, alu->imm);
		if (carryOut) {
			if (alu->immCarry >= 0) {
				_movImm(e, X_R8, alu->immCarry);
			} else {
				_loadCarry(c, X_R8);
			}
		}
	} else {
		_loadSource(c, X_R11, alu->m);
		_shiftImm(c, alu->shiftType, alu->shiftAmount, carryOut);
	}
	if (opcode != ALU_MOV && opcode != ALU_MVN) {
		_loadSource(c, X_RSI, alu->n);
	}

	switch (opcode) {
	case ALU_AND:
	case ALU_TST:
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_rr(e, OP_AND, X_RAX, X_RDX);
		break;
	case ALU_EOR:
	case ALU_TEQ:
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_rr(e, OP_XOR, X_RAX, X_RDX);
		break;
	case ALU_ORR:
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_rr(e, OP_OR, X_RAX, X_RDX);
		break;
	case ALU_BIC:
		_rr(e, OP_MOV, X_RAX, X_RDX);
		_not(e, X_RAX);
		_rr(e, OP_AND, X_RAX, X_RSI);
		break;
	case ALU_MOV:
		_rr(e, OP_MOV, X_RAX, X_RDX);
		break;
	case ALU_MVN:
		_rr(e, OP_MOV, X_RAX, X_RDX);
		_not(e, X_RAX);
		break;
	case ALU_ADD:
	case ALU_CMN:
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_rr(e, OP_ADD, X_RAX, X_RDX);
		break;
	case ALU_SUB:
	case ALU_CMP:
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_rr(e, OP_SUB, X_RAX, X_RDX);
		break;
	case ALU_RSB:
		_rr(e, OP_MOV, X_RAX, X_RDX);
		_rr(e, OP_SUB, X_RAX, X_RSI);
		break;
	case ALU_ADC:
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_carryToCf(e);
		_rr(e, OP_ADC, X_RAX, X_RDX);
		break;
	case ALU_SBC:
		// x86 subtracts CF as a borrow, which is the inverse of the ARM carry
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_carryToCf(e);
		_byte(e, 0xF5); // cmc
		_rr(e, OP_SBB, X_RAX, X_RDX);
		break;
	case ALU_RSC:
		_rr(e, OP_MOV, X_RAX, X_RDX);
		_carryToCf(e);
		_byte(e, 0xF5); // cmc
		_rr(e, OP_SBB, X_RAX, X_RSI);
		break;
	}

	if (setFlags) {
		if (logical) {
			// N and Z from the result, C from the shifter, V and bits 24-27 kept
			_load(e, X_R9, X_CPU, OFF_CPSR);
			_ri(e, 4, X_R9, 0x1FFFFFFF);
			_rr(e, OP_MOV, X_RCX, X_RAX);
			_ri(e, 4, X_RCX, 0x80000000);
			_rr(e, OP_OR, X_R9, X_RCX);
			_rr(e, OP_TEST, X_RAX, X_RAX);
			_setcc(e, CC_E, X_RCX);
			_movzxByte(e, X_RCX, X_RCX);
			_shift(e, SH_SHL, X_RCX, 30);
			_rr(e, OP_OR, X_R9, X_RCX);
			_rr(e, OP_MOV, X_RCX, X_R8);
			_shift(e, SH_SHL, X_RCX, 29);
			_rr(e, OP_OR, X_R9, X_RCX);
		} else {
			_setcc(e, CC_S, X_RCX);
			_setcc(e, CC_E, X_R9);
			_setcc(e, CC_B, X_R10);
			_setcc(e, CC_O, X_R11);
			_movzxByte(e, X_RCX, X_RCX);
			_shift(e, SH_SHL, X_RCX, 31);
			_movzxByte(e, X_R9, X_R9);
			_shift(e, SH_SHL, X_R9, 30);
			_rr(e, OP_OR, X_RCX, X_R9);
			_movzxByte(e, X_R10, X_R10);
			if (borrow) {
				_ri(e, 6, X_R10, 1);
			}
			_shift(e, SH_SHL, X_R10, 29);
			_rr(e, OP_OR, X_RCX, X_R10);
			_movzxByte(e, X_R11, X_R11);
			_shift(e, SH_SHL, X_R11, 28);
			_rr(e, OP_OR, X_RCX, X_R11);
			_load(e, X_R9, X_CPU, OFF_CPSR);
			// Add and subtract clear the whole flags byte; SBC and RSC keep bits 24-27
			_ri(e, 4, X_R9, carryIn && opcode != ALU_ADC ? 0x0FFFFFFF : 0x00FFFFFF);
			_rr(e, OP_OR, X_R9, X_RCX);
		}
		_store(e, X_R9, X_CPU, OFF_CPSR);
	}
	if (opcode < ALU_TST || opcode > ALU_CMN) {
		_store(e, X_RAX, X_CPU, 4 * alu->rd);
	}
	if (storeCarry) {
		_store(e, X_R8, X_CPU, OFF_SHIFTER_CARRY);
	}
	if (skip) {
		_patch(skip, e->p);
	}
}

// IWRAM or EWRAM access at [base + rcx]; loads land in eax (rotated like the GBA), stores take r9
static void _memAccess(struct Emitter* e, bool load, unsigned size, int base) {
	if (load) {
		_rex(e, false, X_RAX, X_RCX, base);
		if (size == 4) {
			_byte(e, 0x8B);
		} else {
			_byte(e, 0x0F);
			_byte(e, size == 2 ? 0xB7 : 0xB6);
		}
		_modrmIndex(e, X_RAX, base, X_RCX);
		if (size > 1) {
			_rr(e, OP_MOV, X_RCX, X_RDI);
			_ri(e, 4, X_RCX, size == 4 ? 3 : 1);
			_shift(e, SH_SHL, X_RCX, 3);
			_shiftCl(e, SH_ROR, X_RAX);
		}
	} else {
		if (size == 2) {
			_byte(e, 0x66);
		}
		_rex(e, false, X_R9, X_RCX, base);
		_byte(e, size == 1 ? 0x88 : 0x89);
		_modrmIndex(e, X_R9, base, X_RCX);
	}
}

// r10 = what GBAMemoryStall makes of a data access wait from cartridge code with prefetch on
static void _romStall(struct Compiler* c, unsigned i, int32_t wait) {
	struct Emitter* e = &c->e;
	int32_t s = c->seq16;
	int32_t n = c->nonseq16;
	uint32_t pc = c->pc + c->width * (i + 2);
	_movImm64(e, X_R11, (uintptr_t) &c->gba->memory);
	// Fewer loads when they overlap the last prefetch
	_load(e, X_RSI, X_R11, offsetof(struct GBAMemory, lastPrefetchedPc));
	_ri(e, 5, X_RSI, pc);
	_rr(e, OP_XOR, X_R8, X_R8);
	_ri(e, 7, X_RSI, 16);
	uint8_t* far = _jcc(e, CC_AE);
	_rr(e, OP_MOV, X_R8, X_RSI);
	_shift(e, SH_SHR, X_R8, 1);
	_patch(far, e->p);
	_movImm(e, X_RSI, 8);
	_rr(e, OP_SUB, X_RSI, X_R8);
	_movImm(e, X_RDX, _prefetchLoads(s, wait));
	_rr(e, OP_CMP, X_RSI, X_RDX);
	_rex(e, false, X_RDX, 0, X_RSI); // cmovb edx, esi
	_byte(e, 0x0F);
	_byte(e, 0x42);
	_modrmReg(e, X_RDX, X_RSI);
	// lastPrefetchedPc = pc + 2 * (loads + previous - 1)
	_rr(e, OP_MOV, X_RSI, X_RDX);
	_rr(e, OP_ADD, X_RSI, X_R8);
	_rr(e, OP_ADD, X_RSI, X_RSI);
	_ri(e, 0, X_RSI, pc - 2);
	_store(e, X_RSI, X_R11, offsetof(struct GBAMemory, lastPrefetchedPc));
	// stall = s * loads + 1; wait = max(wait, stall) - stall - (n - s)
	_rex(e, false, X_RSI, 0, X_RDX); // imul esi, edx, s
	_byte(e, 0x69);
	_modrmReg(e, X_RSI, X_RDX);
	_imm32(e, s);
	_ri(e, 0, X_RSI, 1);
	_movImm(e, X_R10, wait - (n - s));
	_rr(e, OP_SUB, X_R10, X_RSI);
	_ri(e, 7, X_RSI, wait);
	uint8_t* fits = _jcc(e, CC_LE);
	_movImm(e, X_R10, s - n);
	_patch(fits, e->p);
}

// r10 = the wait of a data access to IWRAM or EWRAM
static void _dataWait(struct Compiler* c, unsigned i, int32_t wait) {
	if (c->romCode && c->prefetch) {
		_romStall(c, i, wait);
	} else {
		_movImm(&c->e, X_R10, wait);
	}
}

static void _memResult(struct Compiler* c, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	if (mem->signExtend) {
		uint8_t* halfword = NULL;
		if (mem->size == 2) {
			// LDRSH of an odd address sign-extends the rotated byte
			_byte(e, 0xF6); // test byte [rsp + 4], 1
			_modrmMem(e, 0, X_RSP, 4);
			_byte(e, 1);
			halfword = _jcc(e, CC_E);
		}
		_byte(e, 0x0F); // movsx eax, al
		_byte(e, 0xBE);
		_modrmReg(e, X_RAX, X_RAX);
		if (halfword) {
			uint8_t* extended = _jmp(e);
			_patch(halfword, e->p);
			_byte(e, 0x0F); // movsx eax, ax
			_byte(e, 0xBF);
			_modrmReg(e, X_RAX, X_RAX);
			_patch(extended, e->p);
		}
	}
	if (mem->load) {
		_store(e, X_RAX, X_CPU, 4 * mem->rd);
	} else if (mem->writeback) {
		_store(e, X_WB, X_CPU, 4 * mem->base.reg);
	}
	_rr(e, OP_ADD, X_CYCLES, X_R10);
	_addCycles(c, c->memCycles);
}

static void _emitMem(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned size = mem->size;

	uint8_t* fail = NULL;
	if (mem->cond != 0xE) {
		fail = _condJump(c, mem->cond, false);
	}
	if (!load) {
		_loadSource(c, X_R9, _reg(mem->rd));
	}
	if (mem->runtimeOffset) {
		_load(e, X_RDX, X_JIT, JIT_FETCHED + 4 * mem->fetchedIndex);
		_rr(e, OP_MOV, X_RCX, X_RDX);
		_ri(e, 4, X_RDX, 0xFFF);
		_byte(e, 0x0F); // bt ecx, 23
		_byte(e, 0xBA);
		_modrmReg(e, 4, X_RCX);
		_byte(e, 23);
		uint8_t* up = _jcc(e, CC_B);
		_byte(e, 0xF7); // neg edx
		_modrmReg(e, 3, X_RDX);
		_patch(up, e->p);
	} else if (mem->immediateOffset) {
		_movImm(e, X_RDX, mem->offset);
	} else {
		_loadSource(c, X_R11, mem->m);
		_shiftImm(c, mem->shiftType, mem->shiftAmount, false);
	}
	_loadSource(c, X_RSI, mem->base);
	_rr(e, OP_MOV, X_R10, X_RSI);
	_rr(e, mem->up || mem->runtimeOffset ? OP_ADD : OP_SUB, X_R10, X_RDX);
	if (mem->writeback) {
		_rr(e, OP_MOV, X_WB, X_R10);
	}
	_rr(e, OP_MOV, X_RDI, mem->pre ? X_R10 : X_RSI);
	if (load && mem->writeback) {
		_store(e, X_WB, X_CPU, 4 * mem->base.reg);
	}
	if (mem->signExtend) {
		_store(e, X_RDI, X_RSP, 4); // the address decides how LDRSH extends
	}

	uint8_t* done[4] = { NULL, NULL, NULL, NULL };
	uint8_t* toSlow[3] = { NULL, NULL, NULL };
	uint8_t* checked[2] = { NULL, NULL };
	uint8_t* toCart = NULL;
	_rr(e, OP_MOV, X_RAX, X_RDI);
	_shift(e, SH_SHR, X_RAX, 24);
	_ri(e, 7, X_RAX, 3);
	uint8_t* toEwram = _jcc(e, CC_NE);
	_rr(e, OP_MOV, X_RCX, X_RDI);
	_ri(e, 4, X_RCX, size == 4 ? 0x7FFC : size == 2 ? 0x7FFE : 0x7FFF);
	_memAccess(e, load, size, X_IWRAM);
	if (!load) {
		// Writes into compiled code
		_rr(e, OP_MOV, X_RAX, X_RCX);
		_shift(e, SH_SHR, X_RAX, 2);
		_rex(e, false, 0, X_RAX, X_JIT);
		_byte(e, 0x80); // cmp byte [r12 + rax + cover], 0
		_modrmIndexDisp(e, 7, X_JIT, X_RAX, JIT_COVER);
		_byte(e, 0);
		uint8_t* noCode = _jcc(e, CC_E);
		_movImm64(e, X_RDI, (uintptr_t) c->jit);
		_rr(e, OP_MOV, X_RSI, X_RAX);
		_movImm64(e, X_RAX, (uintptr_t) ARMJitInvalidateWord);
		_callC(e, X_RAX);
		_dataWait(c, i, 1);
		checked[0] = _jmp(e);
		_patch(noCode, e->p);
	}
	_dataWait(c, i, load ? 2 : 1);
	done[0] = _jmp(e);

	_patch(toEwram, e->p);
	int32_t ewramWait = (size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16)[GBA_REGION_EWRAM];
	_ri(e, 7, X_RAX, 2);
	toCart = _jcc(e, CC_NE);
	_rr(e, OP_MOV, X_RCX, X_RDI);
	_ri(e, 4, X_RCX, size == 4 ? 0x3FFFC : size == 2 ? 0x3FFFE : 0x3FFFF);
	_memAccess(e, load, size, X_EWRAM);
	if (!load) {
		_rr(e, OP_MOV, X_RAX, X_RCX);
		_shift(e, SH_SHR, X_RAX, 2);
		_ri(e, 0, X_RAX, ARM_JIT_IWRAM_WORDS);
		_rex(e, false, 0, X_RAX, X_JIT);
		_byte(e, 0x80); // cmp byte [r12 + rax + cover], 0
		_modrmIndexDisp(e, 7, X_JIT, X_RAX, JIT_COVER);
		_byte(e, 0);
		uint8_t* noCode = _jcc(e, CC_E);
		_movImm64(e, X_RDI, (uintptr_t) c->jit);
		_rr(e, OP_MOV, X_RSI, X_RAX);
		_movImm64(e, X_RAX, (uintptr_t) ARMJitInvalidateWord);
		_callC(e, X_RAX);
		_dataWait(c, i, ewramWait + 1);
		checked[1] = _jmp(e);
		_patch(noCode, e->p);
	}
	_dataWait(c, i, ewramWait + (load ? 2 : 1));
	done[1] = _jmp(e);

	// VRAM: stores tell the renderer about changed halfwords; byte stores stay with the handlers
	_patch(toCart, e->p);
	toCart = NULL;
	if (!c->romCode && (load || size > 1)) {
		_ri(e, 7, X_RAX, GBA_REGION_VRAM);
		toCart = _jcc(e, CC_NE);
		_rr(e, OP_MOV, X_RCX, X_RDI);
		_ri(e, 4, X_RCX, 0x1FFFF);
		_ri(e, 7, X_RCX, GBA_SIZE_VRAM);
		toSlow[2] = _jcc(e, CC_AE);
		_ri(e, 4, X_RCX, size == 4 ? 0x1FFFC : size == 2 ? 0x1FFFE : 0x1FFFF);
		_movImm64(e, X_R11, (uintptr_t) c->gba->video.vram);
		_store(e, X_RDI, X_RSP, 4);
		if (load) {
			_memAccess(e, true, size, X_R11);
			_rr(e, OP_MOV, X_WB, X_RAX); // writeback is already stored for loads
		} else {
			if (size == 4) {
				_rex(e, false, X_RDX, X_RCX, X_R11); // mov edx, [r11 + rcx]
				_byte(e, 0x8B);
				_modrmIndex(e, X_RDX, X_R11, X_RCX);
				_rr(e, OP_CMP, X_RDX, X_R9);
			} else {
				_rex(e, false, X_RDX, X_RCX, X_R11); // movzx edx, word [r11 + rcx]
				_byte(e, 0x0F);
				_byte(e, 0xB7);
				_modrmIndex(e, X_RDX, X_R11, X_RCX);
				_rex(e, false, X_R10, 0, X_R9); // movzx r10d, r9w
				_byte(e, 0x0F);
				_byte(e, 0xB7);
				_modrmReg(e, X_R10, X_R9);
				_rr(e, OP_CMP, X_RDX, X_R10);
			}
			uint8_t* unchanged = _jcc(e, CC_E);
			_memAccess(e, false, size, X_R11);
			unsigned call;
			for (call = 0; call < (size == 4 ? 2 : 1); ++call) {
				_movImm64(e, X_RAX, (uintptr_t) &c->gba->video.renderer);
				_load64(e, X_RDI, X_RAX, 0);
				_load(e, X_RSI, X_RSP, 4);
				_ri(e, 4, X_RSI, size == 4 ? 0x1FFFC : 0x1FFFE);
				if (size == 4 && call == 0) {
					_ri(e, 0, X_RSI, 2);
				}
				_load64(e, X_RAX, X_RDI, offsetof(struct GBAVideoRenderer, writeVRAM));
				_callC(e, X_RAX);
			}
			_load(e, X_RDI, X_RSP, 4);
			_patch(unchanged, e->p);
		}
		_movImm64(e, X_RAX, (uintptr_t) &c->gba->video.stallMask);
		_load(e, X_RAX, X_RAX, 0);
		_rr(e, OP_TEST, X_RAX, X_RAX);
		uint8_t* noStall = _jcc(e, CC_E);
		_rr(e, OP_MOV, X_RSI, X_RDI);
		_movImm64(e, X_RDI, (uintptr_t) c->gba);
		_movImm(e, X_RDX, size);
		_movImm64(e, X_RAX, (uintptr_t) GBAMemoryVRAMWait);
		_callC(e, X_RAX);
		_rr(e, OP_MOV, X_R10, X_RAX);
		_load(e, X_RDI, X_RSP, 4);
		uint8_t* waited = _jmp(e);
		_patch(noStall, e->p);
		_movImm(e, X_R10, size == 4 ? 1 : 0);
		_patch(waited, e->p);
		_ri(e, 0, X_R10, load ? 2 : 1);
		if (load) {
			_rr(e, OP_MOV, X_RAX, X_WB);
		}
		done[3] = _jmp(e);
	}
	if (toCart) {
		_patch(toCart, e->p);
	}
	if (load) {
		// Cartridge reads: no prefetch buffer stall for these addresses; region 0x0D is EEPROM
		_rr(e, OP_MOV, X_RCX, X_RAX);
		_ri(e, 5, X_RCX, GBA_REGION_ROM0);
		_ri(e, 7, X_RCX, GBA_REGION_ROM2 - GBA_REGION_ROM0);
		toSlow[0] = _jcc(e, 0x7); // ja
		_rr(e, OP_MOV, X_RCX, X_RDI);
		_ri(e, 4, X_RCX, size == 4 ? GBA_SIZE_ROM0 - 4 : size == 2 ? GBA_SIZE_ROM0 - 2 : GBA_SIZE_ROM0 - 1);
		_ri(e, 7, X_RCX, c->gba->memory.romSize);
		toSlow[1] = _jcc(e, CC_AE);
		_movImm64(e, X_R11, (uintptr_t) c->gba->memory.rom);
		_memAccess(e, true, size, X_R11);
		char* cartWaits = size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16;
		_movImm64(e, X_R11, (uintptr_t) cartWaits);
		_rr(e, OP_MOV, X_RCX, X_RDI);
		_shift(e, SH_SHR, X_RCX, 24);
		_rex(e, false, X_R10, X_RCX, X_R11); // movzx r10d, byte [r11 + rcx]
		_byte(e, 0x0F);
		_byte(e, 0xB6);
		_modrmIndex(e, X_R10, X_R11, X_RCX);
		_ri(e, 0, X_R10, 2);
		done[2] = _jmp(e);
	}

	unsigned t;
	for (t = 0; t < 3; ++t) {
		if (toSlow[t]) {
			_patch(toSlow[t], e->p);
		}
	}
	_storeState(c, i + 1);
	_storeImm(e, X_RSP, 0, 0);
	_rr(e, OP_MOV, X_RSI, X_RDI);
	_mov64(e, X_RDI, X_CPU);
	unsigned offset;
	if (load) {
		_lea64(e, X_RDX, X_RSP, 0);
		offset = size == 4 ? offsetof(struct ARMMemory, load32) : size == 2 ? offsetof(struct ARMMemory, load16) : offsetof(struct ARMMemory, load8);
	} else {
		_rr(e, OP_MOV, X_RDX, X_R9);
		_lea64(e, X_RCX, X_RSP, 0);
		offset = size == 4 ? offsetof(struct ARMMemory, store32) : size == 2 ? offsetof(struct ARMMemory, store16) : offsetof(struct ARMMemory, store8);
	}
	_load64(e, X_RAX, X_CPU, OFF_MEMORY + offset);
	_callC(e, X_RAX);
	_load(e, X_R10, X_RSP, 0);

	// Only stores that called out can have hit compiled code
	uint8_t* toEvent = NULL;
	if (!load) {
		unsigned k;
		for (k = 0; k < 2; ++k) {
			if (checked[k]) {
				_patch(checked[k], e->p);
			}
		}
		_memResult(c, mem);
		_smcCheck(c, i + 1);
		toEvent = _jmp(e);
	}
	unsigned d;
	for (d = 0; d < 4; ++d) {
		if (done[d]) {
			_patch(done[d], e->p);
		}
	}
	_memResult(c, mem);
	if (fail) {
		uint8_t* after = _jmp(e);
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

// A patched load or store whose patches only change the offset runs inline; anything else
// goes to the handler
#define PATCHED_SHAPE 0xFF7FF000

static bool _patchedMem(struct Compiler* c, unsigned i, struct MemOp* mem) {
	uint32_t address = c->pc + c->width * i;
	if (c->thumb || (c->ops[i] & 0x0C000000) != 0x04000000 || !_decodeArmMem(c->ops[i], address, mem) || !mem->immediateOffset) {
		return false;
	}
	mem->runtimeOffset = true;
	mem->fetchedIndex = i;
	return true;
}

static void _emitDynamicHandler(struct Compiler* c, unsigned i);

// A patched instruction: run its handler with the opcode the pipeline fetched
static void _emitDynamic(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	struct MemOp mem;
	if (!_patchedMem(c, i, &mem)) {
		_emitDynamicHandler(c, i);
		return;
	}
	_load(e, X_RAX, X_JIT, JIT_FETCHED + 4 * i);
	_ri(e, 4, X_RAX, PATCHED_SHAPE);
	_ri(e, 7, X_RAX, c->ops[i] & PATCHED_SHAPE);
	uint8_t* other = _jcc(e, CC_NE);
	_emitMem(c, i, &mem);
	uint8_t* done = _jmp(e);
	_patch(other, e->p);
	_emitDynamicHandler(c, i);
	_patch(done, e->p);
}

static void _emitDynamicHandler(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t address = c->pc + c->width * i;

	_storeState(c, i + 1);
	_load(e, X_RSI, X_JIT, JIT_FETCHED + 4 * i);
	uint8_t* toCheck = NULL;
	if (c->thumb) {
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_shift(e, SH_SHR, X_RAX, 6);
		_movImm64(e, X_RDX, (uintptr_t) _thumbTable);
		_loadScaled64(e, X_RAX, X_RDX, X_RAX, 0);
	} else {
		_load(e, X_RAX, X_CPU, OFF_CPSR);
		_shift(e, SH_SHR, X_RAX, 28);
		_rr(e, OP_MOV, X_RCX, X_RSI);
		_shift(e, SH_SHR, X_RCX, 28);
		_movImm64(e, X_RDX, (uintptr_t) _conditionLut32);
		_loadScaled(e, false, X_RDX, X_RDX, X_RCX, 2, 0);
		_byte(e, 0x0F); // bt edx, eax
		_byte(e, 0xA3);
		_modrmReg(e, X_RAX, X_RDX);
		uint8_t* toExec = _jcc(e, CC_B);
		_addCycles(c, c->aluCycles);
		toCheck = _jmp(e);
		_patch(toExec, e->p);
		_rr(e, OP_MOV, X_RAX, X_RSI);
		_shift(e, SH_SHR, X_RAX, 16);
		_ri(e, 4, X_RAX, 0xFF0);
		_rr(e, OP_MOV, X_RCX, X_RSI);
		_shift(e, SH_SHR, X_RCX, 4);
		_ri(e, 4, X_RCX, 0xF);
		_rr(e, OP_OR, X_RAX, X_RCX);
		_movImm64(e, X_RDX, (uintptr_t) _armTable);
		_loadScaled64(e, X_RAX, X_RDX, X_RAX, 0);
	}
	_mov64(e, X_RDI, X_CPU);
	_callC(e, X_RAX);
	_load(e, X_RAX, X_CPU, OFF_PC);
	_ri(e, 7, X_RAX, address + 2 * c->width);
	_exitAt(c, _jcc(e, CC_NE), EXIT_DIRECT);
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
		uint8_t* toExec = _condJump(c, cond, true);
		_addCycles(c, c->aluCycles);
		toCheck = _jmp(e);
		_patch(toExec, e->p);
	}

	_mov64(e, X_RDI, X_CPU);
	_movImm(e, X_RSI, op);
	_movImm64(e, X_RAX, (uintptr_t) _handler(c, op));
	_callC(e, X_RAX);
	_load(e, X_RAX, X_CPU, OFF_PC);
	_ri(e, 7, X_RAX, address + 2 * c->width);
	uint8_t* branched = _jcc(e, CC_NE);
	uint8_t* sequential = _jmp(e);

	// A taken branch back to the start of this block keeps running here
	_patch(branched, e->p);
	if (_loopsToStart(c, i)) {
		_ri(e, 7, X_RAX, c->pc + c->width);
		_exitAt(c, _jcc(e, CC_NE), EXIT_DIRECT);
		_rex(e, false, 0, 0, X_JIT);
		_byte(e, 0x80); // cmp byte [r12 + smcHit], 0
		_modrmMem(e, 7, X_JIT, JIT_SMC_HIT);
		_byte(e, 0);
		_exitAt(c, _jcc(e, CC_NE), EXIT_DIRECT);
		_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
		_exitAt(c, _jcc(e, CC_GE), EXIT_TO_C);
		c->loops[c->nLoops++] = _jmp(e);
	} else {
		uint32_t target;
		if (_branchTarget(c, i, &target)) {
			_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
			_exitAt(c, _jcc(e, CC_GE), EXIT_TO_C);
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
	_storeImm(e, X_CPU, OFF_PC, target + c->width);
	unsigned k;
	for (k = 0; k < 2; ++k) {
		uint32_t address = target + c->width * k;
		int32_t offset = k ? OFF_PREFETCH1 : OFF_PREFETCH0;
		if (ARMJitRamWord(address) < 0) {
			_storeImm(e, X_CPU, offset, _opAt(c, address));
			continue;
		}
		_movImm64(e, X_RAX, (uintptr_t) _hostAddress(c, address));
		if (c->thumb) {
			_byte(e, 0x0F); // movzx ecx, word [rax]
			_byte(e, 0xB7);
			_byte(e, 0x08);
		} else {
			_byte(e, 0x8B); // mov ecx, [rax]
			_byte(e, 0x08);
		}
		_store(e, X_RCX, X_CPU, offset);
	}
}

// B, BL and Thumb conditional branches in the same region, as GBASetActiveRegion and
// ARMWritePC or ThumbWritePC would run them; anything else goes to the handler
static void _emitBranch(struct Compiler* c, unsigned i, const struct BranchOp* b) {
	struct Emitter* e = &c->e;
	uint8_t* notTaken = NULL;
	if (b->cond != 0xE) {
		notTaken = _condJump(c, b->cond, false);
	}
	uint8_t* slow[4];
	unsigned nSlow = 0;
	unsigned region = b->target >> 24;
	_movImm64(e, X_RAX, (uintptr_t) c->gba);
	_memImm(e, 7, X_RAX, offsetof(struct GBA, memory.activeRegion), region);
	slow[nSlow++] = _jcc(e, CC_NE);
	if (region != GBA_REGION_BIOS) {
		_memImm(e, 7, X_RAX, offsetof(struct GBA, idleOptimization), IDLE_LOOP_DETECT);
		slow[nSlow++] = _jcc(e, CC_GE);
		_memImm(e, 7, X_RAX, offsetof(struct GBA, idleLoop), b->target);
		slow[nSlow++] = _jcc(e, CC_E);
	}
	if (b->thumbLink) {
		_memImm(e, 7, X_CPU, 4 * ARM_LR, b->lr);
		slow[nSlow++] = _jcc(e, CC_NE);
	}
	_storeImm(e, X_RAX, offsetof(struct GBA, lastJump), b->target);
	_storeImm(e, X_RAX, offsetof(struct GBA, memory.lastPrefetchedPc), 0);
	if (c->thumb) {
		_memImm(e, 1, X_CPU, OFF_ACTIVE_MASK, WORD_SIZE_THUMB);
	} else {
		_memImm(e, 4, X_CPU, OFF_ACTIVE_MASK, -WORD_SIZE_ARM);
	}
	uint32_t address = c->pc + c->width * i;
	if (b->link) {
		_storeImm(e, X_CPU, 4 * ARM_LR, address + WORD_SIZE_ARM);
	} else if (b->thumbLink) {
		_storeImm(e, X_CPU, 4 * ARM_LR, address + 3);
	}
	_addCycles(c, 2 * c->aluCycles + c->memCycles);
	if (b->index >= 0) {
		_fetchWord(c, b->index);
		_fetchWord(c, b->index + 1);
		_eventCheck(c, b->index);
		_jumpTo(c, c->fast[b->index]);
	} else {
		_storeTargetPipeline(c, b->target);
		_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
		_exitAt(c, _jcc(e, CC_GE), EXIT_TO_C);
		_linkJump(c);
	}

	uint8_t* next = NULL;
	if (notTaken) {
		_patch(notTaken, e->p);
		_addCycles(c, c->aluCycles);
		_eventCheck(c, i + 1);
		next = _jmp(e);
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

// Transfers of a multiple load or store in one RAM region, from the start address in rdi
static void _multiTransfers(struct Compiler* c, const struct MultiOp* m, int base, uint32_t mask) {
	struct Emitter* e = &c->e;
	unsigned k = 0;
	unsigned r;
	for (r = 0; r < 16; ++r) {
		if (!(m->list & (1 << r))) {
			continue;
		}
		_rr(e, OP_MOV, X_RCX, X_RDI);
		if (k) {
			_ri(e, 0, X_RCX, 4 * k);
		}
		_ri(e, 4, X_RCX, mask);
		if (m->load) {
			_rex(e, false, X_RDX, X_RCX, base); // mov edx, [base + rcx]
			_byte(e, 0x8B);
			_modrmIndex(e, X_RDX, base, X_RCX);
			_store(e, X_RDX, X_CPU, 4 * r);
		} else {
			_load(e, X_RDX, X_CPU, 4 * r);
			_rex(e, false, X_RDX, X_RCX, base); // mov [base + rcx], edx
			_byte(e, 0x89);
			_modrmIndex(e, X_RDX, base, X_RCX);
		}
		++k;
	}
}

// Whether any word a multiple store writes holds compiled code
static uint8_t* _multiCoverCheck(struct Compiler* c, const struct MultiOp* m, uint32_t mask, unsigned coverBase) {
	struct Emitter* e = &c->e;
	unsigned n = __builtin_popcount(m->list);
	_rr(e, OP_XOR, X_RAX, X_RAX);
	unsigned k;
	for (k = 0; k < n; ++k) {
		_rr(e, OP_MOV, X_RCX, X_RDI);
		if (k) {
			_ri(e, 0, X_RCX, 4 * k);
		}
		_ri(e, 4, X_RCX, mask);
		_shift(e, SH_SHR, X_RCX, 2);
		if (coverBase) {
			_ri(e, 0, X_RCX, coverBase);
		}
		_rex(e, false, X_RAX, X_RCX, X_JIT); // or al, [r12 + rcx + cover]
		_byte(e, 0x0A);
		_modrmIndexDisp(e, X_RAX, X_JIT, X_RCX, JIT_COVER);
	}
	_rr(e, OP_TEST, X_RAX, X_RAX);
	return _jcc(e, CC_NE);
}

// LDM and STM on IWRAM or EWRAM as GBALoadMultiple and GBAStoreMultiple time them;
// other regions and stores into compiled code go to the handler
static void _emitMulti(struct Compiler* c, unsigned i, const struct MultiOp* m) {
	struct Emitter* e = &c->e;
	unsigned n = __builtin_popcount(m->list);
	uint8_t* fail = NULL;
	if (m->cond != 0xE) {
		fail = _condJump(c, m->cond, false);
	}
	_load(e, X_RSI, X_CPU, 4 * m->rn);
	_rr(e, OP_MOV, X_RDI, X_RSI);
	int32_t start = m->up ? (m->pre ? 4 : 0) : (m->pre ? -4 * (int32_t) n : -4 * (int32_t) n + 4);
	if (start) {
		_ri(e, 0, X_RDI, start);
	}
	_rr(e, OP_MOV, X_WB, X_RSI);
	_ri(e, 0, X_WB, m->up ? 4 * n : -4 * n);
	_rr(e, OP_MOV, X_RAX, X_RDI);
	_shift(e, SH_SHR, X_RAX, 24);

	uint8_t* slow[3];
	unsigned nSlow = 0;
	uint8_t* done[2];
	unsigned region;
	for (region = 0; region < 2; ++region) {
		bool iwram = region == 0;
		_ri(e, 7, X_RAX, iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM);
		uint8_t* other = _jcc(e, CC_NE);
		uint32_t mask = iwram ? GBA_SIZE_IWRAM - 4 : GBA_SIZE_EWRAM - 4;
		if (!m->load) {
			slow[nSlow++] = _multiCoverCheck(c, m, mask, iwram ? 0 : ARM_JIT_IWRAM_WORDS);
		}
		_multiTransfers(c, m, iwram ? X_IWRAM : X_EWRAM, mask);
		const struct GBAMemory* memory = &c->gba->memory;
		unsigned dataRegion = iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM;
		int32_t wait = memory->waitstatesSeq32[dataRegion] - memory->waitstatesNonseq32[dataRegion];
		wait += n * (1 + (iwram ? 0 : memory->waitstatesSeq32[GBA_REGION_EWRAM])) + (m->load ? 1 : 0);
		_dataWait(c, i, wait);
		done[region] = _jmp(e);
		_patch(other, e->p);
		if (!iwram) {
			slow[nSlow++] = _jmp(e);
		}
	}
	_patch(done[0], e->p);
	_patch(done[1], e->p);
	if (m->writeback) {
		_store(e, X_WB, X_CPU, 4 * m->rn);
	}
	_rr(e, OP_ADD, X_CYCLES, X_R10);
	_addCycles(c, c->memCycles);
	if (fail) {
		uint8_t* after = _jmp(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
	_eventCheck(c, i + 1);
	uint8_t* next = _jmp(e);
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
		fail = _condJump(c, m->cond, false);
	}
	// One more cycle for each significant byte past the first, counting leading ones as zeros
	_load(e, X_RCX, X_CPU, 4 * m->rs);
	_rr(e, OP_MOV, X_RAX, X_RCX);
	_shift(e, SH_SAR, X_RAX, 31);
	_rr(e, OP_XOR, X_RCX, X_RAX);
	_movImm(e, X_R10, m->rn >= 0 ? 2 : 1);
	unsigned k;
	for (k = 1; k < 4; ++k) {
		_ri(e, 7, X_RCX, 1 << (8 * k));
		_ri(e, 3, X_R10, -1); // sbb r10d, -1
	}
	if (c->romCode && c->prefetch) {
		_storeImm(e, X_CPU, OFF_PC, c->pc + c->width * (i + 2));
		_mov64(e, X_RDI, X_CPU);
		_rr(e, OP_MOV, X_RSI, X_R10);
		_load64(e, X_RAX, X_CPU, OFF_MEMORY + offsetof(struct ARMMemory, stall));
		_callC(e, X_RAX);
		_rr(e, OP_MOV, X_R10, X_RAX);
	}
	_load(e, X_RAX, X_CPU, 4 * m->rm);
	_rex(e, false, X_RAX, 0, X_CPU); // imul eax, [rbx + rs]
	_byte(e, 0x0F);
	_byte(e, 0xAF);
	_modrmMem(e, X_RAX, X_CPU, 4 * m->rs);
	if (m->rn >= 0) {
		_rex(e, false, X_RAX, 0, X_CPU); // add eax, [rbx + rn]
		_byte(e, 0x03);
		_modrmMem(e, X_RAX, X_CPU, 4 * m->rn);
	}
	_store(e, X_RAX, X_CPU, 4 * m->rd);
	if (m->s) {
		// N and Z from the result; ARM takes C from the last shifter result
		_load(e, X_R9, X_CPU, OFF_CPSR);
		_ri(e, 4, X_R9, c->thumb ? 0x3FFFFFFF : 0x1FFFFFFF);
		_rr(e, OP_MOV, X_RCX, X_RAX);
		_ri(e, 4, X_RCX, 0x80000000);
		_rr(e, OP_OR, X_R9, X_RCX);
		_rr(e, OP_TEST, X_RAX, X_RAX);
		_setcc(e, CC_E, X_RCX);
		_movzxByte(e, X_RCX, X_RCX);
		_shift(e, SH_SHL, X_RCX, 30);
		_rr(e, OP_OR, X_R9, X_RCX);
		if (!c->thumb) {
			_load(e, X_RCX, X_CPU, OFF_SHIFTER_CARRY);
			_ri(e, 4, X_RCX, 1);
			_shift(e, SH_SHL, X_RCX, 29);
			_rr(e, OP_OR, X_R9, X_RCX);
		}
		_store(e, X_R9, X_CPU, OFF_CPSR);
	}
	_rr(e, OP_ADD, X_CYCLES, X_R10);
	_addCycles(c, c->memCycles);
	if (fail) {
		uint8_t* after = _jmp(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
	_eventCheck(c, i + 1);
}
