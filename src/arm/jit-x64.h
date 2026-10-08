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
#define X_COVER X_RBP

enum {
	CC_O = 0x0,
	CC_B = 0x2,
	CC_AE = 0x3,
	CC_E = 0x4,
	CC_NE = 0x5,
	CC_S = 0x8,
	CC_GE = 0xD,
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

static void _addMemImm(struct Emitter* e, int base, int32_t disp, uint32_t imm) {
	_rex(e, false, 0, 0, base);
	_byte(e, 0x81);
	_modrmMem(e, 0, base, disp);
	_imm32(e, imm);
}

static void _addMemReg(struct Emitter* e, int base, int32_t disp, int src) {
	_rex(e, false, src, 0, base);
	_byte(e, 0x01);
	_modrmMem(e, src, base, disp);
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
		_movImm64(e, X_COVER, (uintptr_t) c->jit->cover);
	}
}

static void _epilogue(struct Compiler* c) {
	struct Emitter* e = &c->e;
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

static void _emitTrampoline(struct Compiler* c) {
	struct Emitter* e = &c->e;
	c->jit->enter = (void (*)(struct ARMCore*, void*)) e->p;
	_prologue(c);
	_byte(e, 0xFF); // jmp rsi
	_byte(e, 0xE6);

	// Next block for the PC and mode in the CPU state, if it is compiled and nothing is due
	c->jit->dispatch = e->p;
	_load(e, X_RAX, X_CPU, OFF_CYCLES);
	_cmpRegMem(e, X_RAX, X_CPU, OFF_NEXT_EVENT);
	uint8_t* due = _jcc(e, CC_GE);
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
	_cmpRegMem(e, X_RCX, X_RDX, BLOCK_PC);
	uint8_t* otherPc = _jcc(e, CC_NE);
	_rex(e, false, X_RSI, 0, X_RDX); // movzx esi, byte [rdx + thumb]
	_byte(e, 0x0F);
	_byte(e, 0xB6);
	_modrmMem(e, X_RSI, X_RDX, BLOCK_THUMB);
	_rr(e, OP_CMP, X_RSI, X_RAX);
	uint8_t* otherMode = _jcc(e, CC_NE);
	_load(e, X_RSI, X_CPU, OFF_PREFETCH0);
	_cmpRegMem(e, X_RSI, X_RDX, BLOCK_OP0);
	uint8_t* otherOp0 = _jcc(e, CC_NE);
	_load(e, X_RSI, X_CPU, OFF_PREFETCH1);
	_cmpRegMem(e, X_RSI, X_RDX, BLOCK_OP1);
	uint8_t* otherOp1 = _jcc(e, CC_NE);
	_rex(e, true, X_RDX, 0, X_JIT); // mov [r12 + current], rdx
	_byte(e, 0x89);
	_modrmMem(e, X_RDX, X_JIT, JIT_CURRENT);
	_rex(e, false, 0, 0, X_JIT); // mov byte [r12 + smcHit], 0
	_byte(e, 0xC6);
	_modrmMem(e, 0, X_JIT, JIT_SMC_HIT);
	_byte(e, 0);
	_rex(e, false, 0, 0, X_RDX); // jmp [rdx + entry]
	_byte(e, 0xFF);
	_modrmMem(e, 4, X_RDX, BLOCK_ENTRY);

	c->jit->toC = e->p;
	_patch(due, e->p);
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

// Copy a patched word two instructions ahead, when the GBA's pipeline would fetch it
static void _fetchAhead(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	unsigned index = i + 2;
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

static void _addCycles(struct Compiler* c, uint32_t constant) {
	_addMemImm(&c->e, X_CPU, OFF_CYCLES, constant);
}

static void _eventCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_load(e, X_RAX, X_CPU, OFF_CYCLES);
	_cmpRegMem(e, X_RAX, X_CPU, OFF_NEXT_EVENT);
	_exitAt(c, _jcc(e, CC_GE), index);
}

static void _segmentCheck(struct Compiler* c, unsigned index, uint32_t cycles) {
	struct Emitter* e = &c->e;
	_load(e, X_RAX, X_CPU, OFF_CYCLES);
	_ri(e, 0, X_RAX, cycles);
	_cmpRegMem(e, X_RAX, X_CPU, OFF_NEXT_EVENT);
	_exitAt(c, _jcc(e, CC_GE), index);
}

static void _smcCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_rex(e, false, 0, 0, X_JIT);
	_byte(e, 0x80); // cmp byte [r12 + smcHit], 0
	_modrmMem(e, 7, X_JIT, JIT_SMC_HIT);
	_byte(e, 0);
	_exitAt(c, _jcc(e, CC_NE), index);
}

static void _loadReg(struct Compiler* c, int dst, unsigned reg, uint32_t address) {
	if (reg == ARM_PC) {
		_movImm(&c->e, dst, address + 8);
	} else {
		_load(&c->e, dst, X_CPU, 4 * reg);
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
	bool borrow = opcode == ALU_SUB || opcode == ALU_RSB || opcode == ALU_CMP || opcode == ALU_SBC || opcode == ALU_RSC;

	uint8_t* skip = NULL;
	if (cond != 0xE) {
		skip = _condJump(c, cond, false);
	}

	if (op & 0x02000000) {
		unsigned rotate = (op >> 7) & 0x1E;
		uint32_t value = op & 0xFF;
		if (rotate) {
			value = (value >> rotate) | (value << (32 - rotate));
		}
		_movImm(e, X_RDX, value);
		if (carryOut) {
			if (rotate) {
				_movImm(e, X_R8, value >> 31);
			} else {
				_loadCarry(c, X_R8);
			}
		}
	} else {
		_loadReg(c, X_R11, op & 0xF, address);
		_shiftImm(c, (op >> 5) & 3, (op >> 7) & 0x1F, carryOut);
	}
	if (opcode != ALU_MOV && opcode != ALU_MVN) {
		_loadReg(c, X_RSI, rn, address);
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

	if (s) {
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
			// _additionS and _subtractionS clear the whole flags byte
			_ri(e, 4, X_R9, carryIn ? 0x0FFFFFFF : 0x00FFFFFF);
			_rr(e, OP_OR, X_R9, X_RCX);
		}
		_store(e, X_R9, X_CPU, OFF_CPSR);
	}
	if (opcode < ALU_TST || opcode > ALU_CMN) {
		_store(e, X_RAX, X_CPU, 4 * rd);
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
		fail = _condJump(c, cond, false);
	}
	if (!load) {
		_loadReg(c, X_R9, rd, address);
	}
	if (halfword) {
		if (op & (1 << 22)) {
			_movImm(e, X_RDX, ((op >> 4) & 0xF0) | (op & 0xF));
		} else {
			_loadReg(c, X_RDX, op & 0xF, address);
		}
	} else if (op & 0x02000000) {
		// Addressing mode 2 shifts: LSR and ASR #0 mean #32, ROR #0 is RRX
		_loadReg(c, X_R11, op & 0xF, address);
		_shiftImm(c, (op >> 5) & 3, (op >> 7) & 0x1F, false);
	} else {
		_movImm(e, X_RDX, op & 0xFFF);
	}
	_loadReg(c, X_RSI, rn, address);
	_rr(e, OP_MOV, X_R10, X_RSI);
	_rr(e, u ? OP_ADD : OP_SUB, X_R10, X_RDX);
	if (writeback) {
		_rr(e, OP_MOV, X_WB, X_R10);
	}
	_rr(e, OP_MOV, X_RDI, p ? X_R10 : X_RSI);
	if (load && writeback) {
		_store(e, X_WB, X_CPU, 4 * rn);
	}

	uint8_t* done[2] = { NULL, NULL };
	uint8_t* toSlow = NULL;
	if (c->romCode) {
		goto slow;
	}
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
		_rex(e, false, 0, X_RAX, X_COVER);
		_byte(e, 0x80); // cmp byte [rbp + rax], 0
		_modrmIndex(e, 7, X_COVER, X_RAX);
		_byte(e, 0);
		uint8_t* noCode = _jcc(e, CC_E);
		_movImm64(e, X_RDI, (uintptr_t) c->jit);
		_rr(e, OP_MOV, X_RSI, X_RAX);
		_movImm64(e, X_RAX, (uintptr_t) ARMJitInvalidateWord);
		_call(e, X_RAX);
		_patch(noCode, e->p);
	}
	_movImm(e, X_R10, load ? 2 : 1);
	done[0] = _jmp(e);

	_patch(toEwram, e->p);
	_ri(e, 7, X_RAX, 2);
	toSlow = _jcc(e, CC_NE);
	_rr(e, OP_MOV, X_RCX, X_RDI);
	_ri(e, 4, X_RCX, size == 4 ? 0x3FFFC : size == 2 ? 0x3FFFE : 0x3FFFF);
	_memAccess(e, load, size, X_EWRAM);
	if (!load) {
		_rr(e, OP_MOV, X_RAX, X_RCX);
		_shift(e, SH_SHR, X_RAX, 2);
		_ri(e, 0, X_RAX, ARM_JIT_IWRAM_WORDS);
		_rex(e, false, 0, X_RAX, X_COVER);
		_byte(e, 0x80); // cmp byte [rbp + rax], 0
		_modrmIndex(e, 7, X_COVER, X_RAX);
		_byte(e, 0);
		uint8_t* noCode = _jcc(e, CC_E);
		_movImm64(e, X_RDI, (uintptr_t) c->jit);
		_rr(e, OP_MOV, X_RSI, X_RAX);
		_movImm64(e, X_RAX, (uintptr_t) ARMJitInvalidateWord);
		_call(e, X_RAX);
		_patch(noCode, e->p);
	}
	char* waits = size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16;
	_movImm64(e, X_R11, (uintptr_t) &waits[GBA_REGION_EWRAM]);
	_rex(e, false, X_R10, 0, X_R11); // movzx r10d, byte [r11]
	_byte(e, 0x0F);
	_byte(e, 0xB6);
	_modrmMem(e, X_R10, X_R11, 0);
	_ri(e, 0, X_R10, load ? 2 : 1);
	done[1] = _jmp(e);

	_patch(toSlow, e->p);
slow:
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
	_call(e, X_RAX);
	_load(e, X_R10, X_RSP, 0);

	if (done[0]) {
		_patch(done[0], e->p);
		_patch(done[1], e->p);
	}
	if (load) {
		_store(e, X_RAX, X_CPU, 4 * rd);
	} else if (writeback) {
		_store(e, X_WB, X_CPU, 4 * rn);
	}
	_addMemReg(e, X_CPU, OFF_CYCLES, X_R10);
	_addCycles(c, c->memCycles);
	if (fail) {
		uint8_t* after = _jmp(e);
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
	_call(e, X_RAX);
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
	unsigned cond = c->thumb ? 0xE : op >> 28;

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
	_call(e, X_RAX);
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
		_load(e, X_RAX, X_CPU, OFF_CYCLES);
		_cmpRegMem(e, X_RAX, X_CPU, OFF_NEXT_EVENT);
		_exitAt(c, _jcc(e, CC_GE), EXIT_DIRECT);
		_patch(_jmp(e), c->body);
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
