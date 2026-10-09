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
// A transfer's base writeback, or a value kept across C calls
#define X_WB X_R15
// The cycle count lives here while generated code runs
#define X_CYCLES X_RBP
// Stack slots: a C call's cycles and the address of the access in flight
#define CYCLE_SLOT 0
#define ADDRESS_SLOT 4

enum {
	CC_O = 0x0,
	CC_B = 0x2,
	CC_AE = 0x3,
	CC_E = 0x4,
	CC_NE = 0x5,
	CC_A = 0x7,
	CC_S = 0x8,
	CC_GE = 0xD,
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

// A ModRM operand: a register, or memory at [base + (index << scale) + disp]
struct X86Operand {
	int reg;
	int base;
	int index;
	unsigned scale;
	int32_t disp;
};

static struct X86Operand _xr(int reg) {
	struct X86Operand operand = { reg, 0, -1, 0, 0 };
	return operand;
}

static struct X86Operand _xm(int base, int32_t disp) {
	struct X86Operand operand = { -1, base, -1, 0, disp };
	return operand;
}

static struct X86Operand _xmi(int base, int index, unsigned scale, int32_t disp) {
	struct X86Operand operand = { -1, base, index, scale, disp };
	return operand;
}

enum {
	X_W = 1,
	X_16 = 2,
};

// Opcodes taking a ModRM operand; two-byte ones start with 0F
enum {
	XO_ADD = 0x01, XO_OR = 0x09, XO_SUB = 0x29, XO_XOR = 0x31,
	XO_CMP = 0x39, XO_ADD_RM = 0x03, XO_OR8_RM = 0x0A, XO_OR_RM = 0x0B, XO_ADC_RM = 0x13, XO_SBB_RM = 0x1B,
	XO_SUB_RM = 0x2B, XO_AND_RM = 0x23, XO_XOR_RM = 0x33, XO_CMP_RM = 0x3B,
	XO_GROUP1_8 = 0x80, XO_GROUP1 = 0x81, XO_GROUP1_IMM8 = 0x83, XO_TEST8 = 0x84, XO_TEST = 0x85,
	XO_STORE8 = 0x88, XO_STORE = 0x89, XO_LOAD = 0x8B, XO_LEA = 0x8D, XO_SHIFT = 0xC1, XO_MOV8_IMM = 0xC6,
	XO_MOV_IMM = 0xC7, XO_SHIFT_CL = 0xD3, XO_GROUP3_8 = 0xF6, XO_GROUP3 = 0xF7, XO_GROUP5 = 0xFF,
	XO_SETCC = 0x0F90, XO_BT = 0x0FA3, XO_BTS = 0x0FAB, XO_IMUL = 0x0FAF,
	XO_MOVZX8 = 0x0FB6, XO_MOVZX16 = 0x0FB7, XO_GROUP8 = 0x0FBA, XO_MOVSX8 = 0x0FBE, XO_MOVSX16 = 0x0FBF,
};

// /digit forms of the group opcodes
enum {
	G1_ADD = 0, G1_OR = 1, G1_ADC = 2, G1_SBB = 3, G1_AND = 4, G1_SUB = 5, G1_XOR = 6, G1_CMP = 7,
	G3_TEST = 0, G3_NOT = 2, G3_NEG = 3, G5_CALL = 2, G5_JMP = 4, G8_BT = 4,
};

enum {
	SH_ROR = 1,
	SH_SHL = 4,
	SH_SHR = 5,
	SH_SAR = 7,
};

static void _modrm(struct Emitter* e, int reg, struct X86Operand rm) {
	if (rm.reg >= 0) {
		_byte(e, 0xC0 | ((reg & 7) << 3) | (rm.reg & 7));
		return;
	}
	bool sib = rm.index >= 0 || (rm.base & 7) == X_RSP;
	int mod = !rm.disp && (rm.base & 7) != X_RBP ? 0 : rm.disp >= -128 && rm.disp < 128 ? 1 : 2;
	_byte(e, (mod << 6) | ((reg & 7) << 3) | (sib ? 4 : rm.base & 7));
	if (sib) {
		_byte(e, (rm.scale << 6) | (((rm.index >= 0 ? rm.index : X_RSP) & 7) << 3) | (rm.base & 7));
	}
	if (mod == 1) {
		_byte(e, rm.disp);
	} else if (mod == 2) {
		_imm32(e, rm.disp);
	}
}

// [66] [REX] opcode ModRM [SIB] [disp]; reg is a register or the opcode's /digit
static void _insn(struct Emitter* e, unsigned flags, unsigned opcode, int reg, struct X86Operand rm) {
	if (flags & X_16) {
		_byte(e, 0x66);
	}
	uint8_t rex = 0x40 | ((flags & X_W) ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm.index >= 0 && (rm.index & 8)) ? 2 : 0) | (((rm.reg >= 0 ? rm.reg : rm.base) & 8) ? 1 : 0);
	if (rex != 0x40) {
		_byte(e, rex);
	}
	if (opcode > 0xFF) {
		_byte(e, opcode >> 8);
	}
	_byte(e, opcode);
	_modrm(e, reg, rm);
}

// op r/m32, r32
static void _rr(struct Emitter* e, unsigned opcode, int dst, int src) {
	_insn(e, 0, opcode, src, _xr(dst));
}

static void _mov(struct Emitter* e, int dst, int src) {
	_insn(e, 0, XO_STORE, src, _xr(dst));
}

static void _mov64(struct Emitter* e, int dst, int src) {
	_insn(e, X_W, XO_STORE, src, _xr(dst));
}

// Group 1 (G1_*) with an immediate, sign-extended from a byte when it fits
static void _groupImm(struct Emitter* e, unsigned flags, int digit, struct X86Operand rm, uint32_t imm) {
	bool small = (int32_t) imm == (int8_t) imm;
	_insn(e, flags, small ? XO_GROUP1_IMM8 : XO_GROUP1, digit, rm);
	if (small) {
		_byte(e, imm);
	} else {
		_imm32(e, imm);
	}
}

static void _ri(struct Emitter* e, int digit, int dst, uint32_t imm) {
	_groupImm(e, 0, digit, _xr(dst), imm);
}

static void _movImm(struct Emitter* e, int dst, uint32_t imm) {
	if (dst & 8) {
		_byte(e, 0x41);
	}
	_byte(e, 0xB8 | (dst & 7));
	_imm32(e, imm);
}

static void _movImm64(struct Emitter* e, int dst, uint64_t imm) {
	_byte(e, (dst & 8) ? 0x49 : 0x48);
	_byte(e, 0xB8 | (dst & 7));
	memcpy(e->p, &imm, 8);
	e->p += 8;
}

static void _load(struct Emitter* e, int dst, int base, int32_t disp) {
	_insn(e, 0, XO_LOAD, dst, _xm(base, disp));
}

static void _load64(struct Emitter* e, int dst, int base, int32_t disp) {
	_insn(e, X_W, XO_LOAD, dst, _xm(base, disp));
}

static void _store(struct Emitter* e, int src, int base, int32_t disp) {
	_insn(e, 0, XO_STORE, src, _xm(base, disp));
}

static void _storeImm(struct Emitter* e, int base, int32_t disp, uint32_t imm) {
	_insn(e, 0, XO_MOV_IMM, 0, _xm(base, disp));
	_imm32(e, imm);
}

// Group 1 on dword [base + disp] with an immediate
static void _memImm(struct Emitter* e, int digit, int base, int32_t disp, uint32_t imm) {
	_groupImm(e, 0, digit, _xm(base, disp), imm);
}

// cmp byte [operand], imm8
static void _cmpByte(struct Emitter* e, struct X86Operand rm, uint8_t imm) {
	_insn(e, 0, XO_GROUP1_8, G1_CMP, rm);
	_byte(e, imm);
}

static void _cmpRegMem(struct Emitter* e, int reg, int base, int32_t disp) {
	_insn(e, 0, XO_CMP_RM, reg, _xm(base, disp));
}

static void _lea64(struct Emitter* e, int dst, int base, int32_t disp) {
	_insn(e, X_W, XO_LEA, dst, _xm(base, disp));
}

static void _shift(struct Emitter* e, int digit, int dst, unsigned amount) {
	_insn(e, 0, XO_SHIFT, digit, _xr(dst));
	_byte(e, amount);
}

static void _shiftCl(struct Emitter* e, int digit, int dst) {
	_insn(e, 0, XO_SHIFT_CL, digit, _xr(dst));
}

static void _not(struct Emitter* e, int dst) {
	_insn(e, 0, XO_GROUP3, G3_NOT, _xr(dst));
}

static void _setcc(struct Emitter* e, int cc, int dst) {
	_insn(e, 0, XO_SETCC | cc, 0, _xr(dst));
}

static void _movzxByte(struct Emitter* e, int dst, int src) {
	_insn(e, 0, XO_MOVZX8, dst, _xr(src));
}

// CF = bit 29 of the guest CPSR
static void _carryToCf(struct Emitter* e) {
	_insn(e, 0, XO_GROUP8, G8_BT, _xm(X_CPU, OFF_CPSR));
	_byte(e, 29);
}

static void _call(struct Emitter* e, int reg) {
	_insn(e, 0, XO_GROUP5, G5_CALL, _xr(reg));
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
	if (reg & 8) {
		_byte(e, 0x41);
	}
	_byte(e, 0x50 | (reg & 7));
}

static void _pop(struct Emitter* e, int reg) {
	if (reg & 8) {
		_byte(e, 0x41);
	}
	_byte(e, 0x58 | (reg & 7));
}

// rsp += bytes
static void _adjustStack(struct Emitter* e, int8_t bytes) {
	_groupImm(e, X_W, G1_ADD, _xr(X_RSP), bytes);
}

static void _ret(struct Emitter* e) {
	_byte(e, 0xC3);
}

static void _cmc(struct Emitter* e) {
	_byte(e, 0xF5);
}

// call rel32, returning the site to patch
static uint8_t* _callSite(struct Emitter* e) {
	_byte(e, 0xE8);
	uint8_t* at = e->p;
	_imm32(e, 0);
	return at;
}

// Runs the due events; ZF is set when the frame is done
static void _callEvents(struct Emitter* e) {
	_mov64(e, X_RDI, X_CPU);
	_movImm64(e, X_RAX, (uintptr_t) ARMJitEvents);
	_callC(e, X_RAX);
	_insn(e, 0, XO_TEST8, X_RAX, _xr(X_RAX));
}

static void _prologue(struct Compiler* c) {
	struct Emitter* e = &c->e;
	_push(e, X_RBX);
	_push(e, X_RBP);
	_push(e, X_R12);
	_push(e, X_R13);
	_push(e, X_R14);
	_push(e, X_R15);
	_adjustStack(e, -8); // keeps calls aligned and holds the stack slots
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
	_adjustStack(e, 8);
	_pop(e, X_R15);
	_pop(e, X_R14);
	_pop(e, X_R13);
	_pop(e, X_R12);
	_pop(e, X_RBP);
	_pop(e, X_RBX);
	_ret(e);
}

static void _jumpTo(struct Compiler* c, const uint8_t* target) {
	_patch(_jmp(&c->e), target);
}

// mov dst, qword [base + index * 8 + disp]
static void _loadScaled64(struct Emitter* e, int dst, int base, int index, int32_t disp) {
	_insn(e, X_W, XO_LOAD, dst, _xmi(base, index, 3, disp));
}

static void _test64(struct Emitter* e, int reg) {
	_insn(e, X_W, XO_TEST, reg, _xr(reg));
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
	_insn(e, 0, XO_GROUP5, G5_JMP, _xr(X_RSI));

	// A link stub passes its link in rdx so the exit gets patched to the block found
	c->jit->linkDispatch = e->p;
	_insn(e, X_W, XO_STORE, X_RDX, _xm(X_JIT, JIT_PENDING_LINK));
	uint8_t* toLookup = _jmp(e);

	// Next block for the PC and mode in the CPU state, if it is compiled and nothing is due
	c->jit->dispatch = e->p;
	_insn(e, X_W, XO_MOV_IMM, 0, _xm(X_JIT, JIT_PENDING_LINK));
	_imm32(e, 0);
	uint8_t* toLookup2 = _jmp(e);

	// Due events run here; the frame loop decides whether to come back
	c->jit->events = e->p;
	_callEvents(e);
	uint8_t* stop = _jcc(e, CC_E);
	_insn(e, X_W, XO_MOV_IMM, 0, _xm(X_JIT, JIT_PENDING_LINK));
	_imm32(e, 0);

	_patch(toLookup, e->p);
	_patch(toLookup2, e->p);
	_insn(e, 0, XO_MOV8_IMM, 0, _xm(X_JIT, JIT_SMC_HIT));
	_byte(e, 0);
	_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
	_patch(_jcc(e, CC_GE), c->jit->events);
	_load(e, X_RAX, X_CPU, OFF_EXECUTION_MODE);
	_load(e, X_RCX, X_CPU, OFF_PC);
	_ri(e, G1_SUB, X_RCX, 4);
	_rr(e, XO_ADD, X_RCX, X_RAX);
	_rr(e, XO_ADD, X_RCX, X_RAX);
	_mov(e, X_RDX, X_RCX);
	_shift(e, SH_SHR, X_RDX, 12);
	_ri(e, G1_AND, X_RDX, ARM_JIT_PAGES - 1);
	_loadScaled64(e, X_RDX, X_JIT, X_RDX, JIT_PAGES);
	_test64(e, X_RDX);
	uint8_t* noPage = _jcc(e, CC_E);
	_mov(e, X_RSI, X_RCX);
	_ri(e, G1_AND, X_RSI, 0xFFF);
	_shift(e, SH_SHR, X_RSI, 1);
	_loadScaled64(e, X_RDX, X_RDX, X_RSI, 0);
	_test64(e, X_RDX);
	uint8_t* noBlock = _jcc(e, CC_E);
	_cmpRegMem(e, X_RCX, X_RDX, ENTRY_PC);
	uint8_t* otherPc = _jcc(e, CC_NE);
	_insn(e, 0, XO_MOVZX8, X_RSI, _xm(X_RDX, ENTRY_THUMB));
	_rr(e, XO_CMP, X_RSI, X_RAX);
	uint8_t* otherMode = _jcc(e, CC_NE);
	_load(e, X_RSI, X_CPU, OFF_PREFETCH0);
	_insn(e, 0, XO_XOR_RM, X_RSI, _xm(X_RDX, ENTRY_OP0));
	_insn(e, 0, XO_AND_RM, X_RSI, _xm(X_RDX, ENTRY_OP_MASK0));
	uint8_t* otherOp0 = _jcc(e, CC_NE);
	_load(e, X_RSI, X_CPU, OFF_PREFETCH1);
	_insn(e, 0, XO_XOR_RM, X_RSI, _xm(X_RDX, ENTRY_OP1));
	_insn(e, 0, XO_AND_RM, X_RSI, _xm(X_RDX, ENTRY_OP_MASK1));
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
	_insn(e, 0, XO_GROUP5, G5_JMP, _xm(X_RDX, ENTRY_CODE));

	c->jit->toC = e->p;
	_patch(stop, e->p);
	_patch(noPage, e->p);
	_patch(noBlock, e->p);
	_patch(otherPc, e->p);
	_patch(otherMode, e->p);
	_patch(otherOp0, e->p);
	_patch(otherOp1, e->p);
	_epilogue(c);

	// Runs a due exit's events, then returns to the block unless the PC moved, its code changed or the frame ended
	c->jit->resume = e->p;
	_load(e, X_WB, X_CPU, OFF_PC);
	_adjustStack(e, -8);
	_callEvents(e);
	uint8_t* frameDone = _jcc(e, CC_E);
	_cmpRegMem(e, X_WB, X_CPU, OFF_PC);
	uint8_t* moved = _jcc(e, CC_NE);
	_cmpByte(e, _xm(X_JIT, JIT_SMC_HIT), 0);
	uint8_t* written = _jcc(e, CC_NE);
	_adjustStack(e, 8);
	_ret(e);
	// Leaving: drop the padding and the return address
	_patch(frameDone, e->p);
	_adjustStack(e, 16);
	_jumpTo(c, c->jit->toC);
	_patch(moved, e->p);
	_patch(written, e->p);
	_adjustStack(e, 16);
	_jumpTo(c, c->jit->dispatch);
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
	_insn(e, 0, c->thumb ? XO_MOVZX16 : XO_LOAD, X_RCX, _xm(X_RAX, 0));
	_store(e, X_RCX, X_JIT, JIT_FETCHED + 4 * index);
}

// A patched word two instructions ahead, when the GBA's pipeline would fetch it
static void _fetchAhead(struct Compiler* c, unsigned i) {
	_fetchWord(c, i + 2);
}

static void _addCycles(struct Compiler* c, uint32_t constant) {
	_ri(&c->e, G1_ADD, X_CYCLES, constant);
}

static void _eventCheck(struct Compiler* c, unsigned index) {
	struct Emitter* e = &c->e;
	_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
	_exitAt(c, _jcc(e, CC_GE), index | EXIT_DUE);
}

// A conditional instruction that does not run still takes its fetch
static void _skipped(struct Compiler* c, uint8_t* fail) {
	struct Emitter* e = &c->e;
	if (fail) {
		uint8_t* after = _jmp(e);
		_patch(fail, e->p);
		_addCycles(c, c->aluCycles);
		_patch(after, e->p);
	}
}

// Jumps to the returned site when an event comes due before the last instruction of a run
static uint8_t* _runCheck(struct Compiler* c, uint32_t cycles) {
	struct Emitter* e = &c->e;
	_mov(e, X_RAX, X_CYCLES);
	_ri(e, G1_ADD, X_RAX, cycles);
	_cmpRegMem(e, X_RAX, X_CPU, OFF_NEXT_EVENT);
	return _jcc(e, CC_GE);
}

static void _smcCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_cmpByte(e, _xm(X_JIT, JIT_SMC_HIT), 0);
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
	_insn(e, 0, XO_BT, X_RAX, _xr(X_RCX));
	return _jcc(e, taken ? CC_B : CC_AE);
}

static void _loadCarry(struct Compiler* c, int dst) {
	_load(&c->e, dst, X_CPU, OFF_CPSR);
	_shift(&c->e, SH_SHR, dst, 29);
	_ri(&c->e, G1_AND, dst, 1);
}

// Barrel shifter for an immediate shift amount on r11d: value in edx, carry out in r8d if wanted; RRX also uses r10d
static void _shiftImm(struct Compiler* c, unsigned type, unsigned amount, bool carry) {
	struct Emitter* e = &c->e;
	switch (type) {
	case SHIFT_LSL:
		_mov(e, X_RDX, X_R11);
		if (amount) {
			_shift(e, SH_SHL, X_RDX, amount);
		}
		if (carry) {
			if (amount) {
				_mov(e, X_R8, X_R11);
				_shift(e, SH_SHR, X_R8, 32 - amount);
				_ri(e, G1_AND, X_R8, 1);
			} else {
				_loadCarry(c, X_R8);
			}
		}
		break;
	case SHIFT_LSR:
		if (amount) {
			_mov(e, X_RDX, X_R11);
			_shift(e, SH_SHR, X_RDX, amount);
		} else {
			_movImm(e, X_RDX, 0);
		}
		if (carry) {
			_mov(e, X_R8, X_R11);
			_shift(e, SH_SHR, X_R8, amount ? amount - 1 : 31);
			_ri(e, G1_AND, X_R8, 1);
		}
		break;
	case SHIFT_ASR:
		_mov(e, X_RDX, X_R11);
		_shift(e, SH_SAR, X_RDX, amount ? amount : 31);
		if (carry) {
			_mov(e, X_R8, X_R11);
			_shift(e, SH_SHR, X_R8, amount ? amount - 1 : 31);
			_ri(e, G1_AND, X_R8, 1);
		}
		break;
	case SHIFT_ROR:
		_mov(e, X_RDX, X_R11);
		if (amount) {
			_shift(e, SH_ROR, X_RDX, amount);
		} else {
			_loadCarry(c, X_R10);
			_shift(e, SH_SHR, X_RDX, 1);
			_shift(e, SH_SHL, X_R10, 31);
			_rr(e, XO_OR, X_RDX, X_R10);
		}
		if (carry) {
			_mov(e, X_R8, X_R11);
			if (amount) {
				_shift(e, SH_SHR, X_R8, amount - 1);
			}
			_ri(e, G1_AND, X_R8, 1);
		}
		break;
	}
}

// Combines eax with a constant or an operand, as the ALU operation does
static void _combine(struct Emitter* e, unsigned opcode, bool constant, uint32_t value, struct X86Operand rm) {
	static const struct {
		uint8_t opcode;
		uint8_t digit;
	} forms[16] = {
		[ALU_AND] = { XO_AND_RM, G1_AND }, [ALU_TST] = { XO_AND_RM, G1_AND }, [ALU_EOR] = { XO_XOR_RM, G1_XOR },
		[ALU_TEQ] = { XO_XOR_RM, G1_XOR }, [ALU_ORR] = { XO_OR_RM, G1_OR }, [ALU_BIC] = { XO_AND_RM, G1_AND },
		[ALU_ADD] = { XO_ADD_RM, G1_ADD }, [ALU_CMN] = { XO_ADD_RM, G1_ADD }, [ALU_ADC] = { XO_ADC_RM, G1_ADC },
		[ALU_SUB] = { XO_SUB_RM, G1_SUB }, [ALU_CMP] = { XO_SUB_RM, G1_SUB }, [ALU_RSB] = { XO_SUB_RM, G1_SUB },
		[ALU_SBC] = { XO_SBB_RM, G1_SBB }, [ALU_RSC] = { XO_SBB_RM, G1_SBB },
	};
	if (constant) {
		_ri(e, forms[opcode].digit, X_RAX, value);
	} else {
		_insn(e, 0, forms[opcode].opcode, X_RAX, rm);
	}
}

// r9 = CPSR with N and Z from eax, keeping the bits in keep
static void _mergeNZ(struct Emitter* e, uint32_t keep) {
	_load(e, X_R9, X_CPU, OFF_CPSR);
	_ri(e, G1_AND, X_R9, keep);
	_mov(e, X_RCX, X_RAX);
	_ri(e, G1_AND, X_RCX, 0x80000000);
	_rr(e, XO_OR, X_R9, X_RCX);
	_rr(e, XO_TEST, X_RAX, X_RAX);
	_setcc(e, CC_E, X_RCX);
	_movzxByte(e, X_RCX, X_RCX);
	_shift(e, SH_SHL, X_RCX, 30);
	_rr(e, XO_OR, X_R9, X_RCX);
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

	// The second operand: an immediate, a guest register in memory, or a shifted value in edx
	bool direct = !alu->immediate && !alu->m.constant && alu->shiftType == SHIFT_LSL && !alu->shiftAmount;
	if (alu->immediate) {
		if (carryOut) {
			if (alu->immCarry >= 0) {
				_movImm(e, X_R8, alu->immCarry);
			} else {
				_loadCarry(c, X_R8);
			}
		}
	} else if (direct) {
		if (carryOut) {
			_loadCarry(c, X_R8);
		}
	} else {
		_loadSource(c, X_R11, alu->m);
		_shiftImm(c, alu->shiftType, alu->shiftAmount, carryOut);
	}

	// eax starts as the first operand, or the second for the reversed operations, and takes the other in place
	bool reverse = opcode == ALU_RSB || opcode == ALU_RSC || opcode == ALU_BIC || opcode == ALU_MOV || opcode == ALU_MVN;
	if (!reverse) {
		_loadSource(c, X_RAX, alu->n);
	} else if (alu->immediate) {
		_movImm(e, X_RAX, alu->imm);
	} else if (direct) {
		_load(e, X_RAX, X_CPU, 4 * alu->m.reg);
	} else {
		_mov(e, X_RAX, X_RDX);
	}
	if (opcode == ALU_MVN || opcode == ALU_BIC) {
		_not(e, X_RAX);
	}
	if (carryIn) {
		// x86 subtracts CF as a borrow, which is the inverse of the ARM carry
		_carryToCf(e);
		if (opcode != ALU_ADC) {
			_cmc(e);
		}
	}
	if (!reverse) {
		_combine(e, opcode, alu->immediate, alu->imm, direct ? _xm(X_CPU, 4 * alu->m.reg) : _xr(X_RDX));
	} else if (opcode != ALU_MOV && opcode != ALU_MVN) {
		_combine(e, opcode, alu->n.constant, alu->n.value, _xm(X_CPU, 4 * alu->n.reg));
	}

	if (setFlags) {
		if (logical) {
			// N and Z from the result, C from the shifter, V and bits 24-27 kept
			_mergeNZ(e, 0x1FFFFFFF);
			_mov(e, X_RCX, X_R8);
			_shift(e, SH_SHL, X_RCX, 29);
			_rr(e, XO_OR, X_R9, X_RCX);
		} else {
			_setcc(e, CC_S, X_RCX);
			_setcc(e, CC_E, X_R9);
			_setcc(e, CC_B, X_R10);
			_setcc(e, CC_O, X_R11);
			_movzxByte(e, X_RCX, X_RCX);
			_shift(e, SH_SHL, X_RCX, 31);
			_movzxByte(e, X_R9, X_R9);
			_shift(e, SH_SHL, X_R9, 30);
			_rr(e, XO_OR, X_RCX, X_R9);
			_movzxByte(e, X_R10, X_R10);
			if (borrow) {
				_ri(e, G1_XOR, X_R10, 1);
			}
			_shift(e, SH_SHL, X_R10, 29);
			_rr(e, XO_OR, X_RCX, X_R10);
			_movzxByte(e, X_R11, X_R11);
			_shift(e, SH_SHL, X_R11, 28);
			_rr(e, XO_OR, X_RCX, X_R11);
			_load(e, X_R9, X_CPU, OFF_CPSR);
			// Add and subtract clear the whole flags byte; SBC and RSC keep bits 24-27
			_ri(e, G1_AND, X_R9, carryIn && opcode != ALU_ADC ? 0x0FFFFFFF : 0x00FFFFFF);
			_rr(e, XO_OR, X_R9, X_RCX);
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

// Access at [base + rcx]; loads land in eax, rotated like the GBA by the address in edi; stores take r9
static void _memAccess(struct Emitter* e, bool load, unsigned size, int base) {
	if (load) {
		_insn(e, 0, size == 4 ? XO_LOAD : size == 2 ? XO_MOVZX16 : XO_MOVZX8, X_RAX, _xmi(base, X_RCX, 0, 0));
		if (size > 1) {
			_mov(e, X_RCX, X_RDI);
			_ri(e, G1_AND, X_RCX, size == 4 ? 3 : 1);
			_shift(e, SH_SHL, X_RCX, 3);
			_shiftCl(e, SH_ROR, X_RAX);
		}
	} else {
		_insn(e, size == 2 ? X_16 : 0, size == 1 ? XO_STORE8 : XO_STORE, X_R9, _xmi(base, X_RCX, 0, 0));
	}
}

// r10 = what GBAMemoryStall makes of a data access wait from cartridge code with prefetch on
static void _romStall(struct Compiler* c, unsigned i, int32_t wait) {
	struct Emitter* e = &c->e;
	int32_t s = c->seq16;
	int32_t n = c->nonseq16;
	uint32_t pc = c->pc + c->width * (i + 2);
	uint64_t advances;
	uint64_t stalls;
	_stallTables(s, wait, &advances, &stalls);
	_movImm64(e, X_R11, (uintptr_t) &c->gba->memory);
	_load(e, X_RSI, X_R11, offsetof(struct GBAMemory, lastPrefetchedPc));
	_ri(e, G1_SUB, X_RSI, pc);
	_rr(e, XO_XOR, X_RCX, X_RCX);
	_ri(e, G1_CMP, X_RSI, 16);
	uint8_t* far = _jcc(e, CC_AE);
	_mov(e, X_RCX, X_RSI);
	_ri(e, G1_AND, X_RCX, 14);
	_shift(e, SH_SHL, X_RCX, 2);
	_patch(far, e->p);
	// lastPrefetchedPc = pc + 2 * (loads + previous - 1)
	_movImm64(e, X_RDX, advances);
	_insn(e, X_W, XO_SHIFT_CL, SH_SHR, _xr(X_RDX));
	_movzxByte(e, X_RDX, X_RDX);
	_insn(e, 0, XO_LEA, X_RDX, _xmi(X_RDX, X_RDX, 0, pc));
	_store(e, X_RDX, X_R11, offsetof(struct GBAMemory, lastPrefetchedPc));
	// max(wait, stall) - stall - (n - s), as wait - (n - s) - min(wait, stall)
	_movImm64(e, X_R10, stalls);
	_insn(e, X_W, XO_SHIFT_CL, SH_SHR, _xr(X_R10));
	_insn(e, 0, XO_MOVSX8, X_R10, _xr(X_R10));
	_ri(e, G1_ADD, X_R10, wait - (n - s));
}

// r10 = a data access wait, through GBAMemoryStall when cartridge code runs with prefetch on
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
			_insn(e, 0, XO_GROUP3_8, G3_TEST, _xm(X_RSP, ADDRESS_SLOT));
			_byte(e, 1);
			halfword = _jcc(e, CC_E);
		}
		_insn(e, 0, XO_MOVSX8, X_RAX, _xr(X_RAX));
		if (halfword) {
			uint8_t* extended = _jmp(e);
			_patch(halfword, e->p);
			_insn(e, 0, XO_MOVSX16, X_RAX, _xr(X_RAX));
			_patch(extended, e->p);
		}
	}
	if (mem->load) {
		_store(e, X_RAX, X_CPU, 4 * mem->rd);
	} else if (mem->writeback) {
		_store(e, X_WB, X_CPU, 4 * mem->base.reg);
	}
	_rr(e, XO_ADD, X_CYCLES, X_R10);
	_addCycles(c, c->memCycles);
}

// IWRAM or EWRAM, wait in r10; a store into compiled code takes the returned site with its word in eax
static uint8_t* _memRam(struct Compiler* c, unsigned i, const struct MemOp* mem, bool ewram) {
	struct Emitter* e = &c->e;
	_mov(e, X_RCX, X_RDI);
	_ri(e, G1_AND, X_RCX, (ewram ? GBA_SIZE_EWRAM : GBA_SIZE_IWRAM) - mem->size);
	_memAccess(e, mem->load, mem->size, ewram ? X_EWRAM : X_IWRAM);
	uint8_t* smc = NULL;
	if (!mem->load) {
		_mov(e, X_RAX, X_RCX);
		_shift(e, SH_SHR, X_RAX, 2);
		if (ewram) {
			_ri(e, G1_ADD, X_RAX, ARM_JIT_IWRAM_WORDS);
		}
		_cmpByte(e, _xmi(X_JIT, X_RAX, 0, JIT_COVER), 0);
		smc = _jcc(e, CC_NE);
	}
	_dataWait(c, i, _ramWait(c, mem, ewram) + (mem->load ? 2 : 1));
	return smc;
}

static void _memInvalidate(struct Compiler* c, unsigned i, const struct MemOp* mem, bool ewram) {
	struct Emitter* e = &c->e;
	_mov64(e, X_RDI, X_JIT);
	_mov(e, X_RSI, X_RAX);
	_movImm64(e, X_RAX, (uintptr_t) ARMJitInvalidateWord);
	_callC(e, X_RAX);
	_dataWait(c, i, _ramWait(c, mem, ewram) + 1);
}

// Timer counters, without the generic load and I/O dispatch
static void _memTimer(struct Compiler* c, unsigned i, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	_ri(e, G1_CMP, X_RAX, GBA_REGION_IO);
	*miss = _jcc(e, CC_NE);
	_mov(e, X_RCX, X_RDI);
	_ri(e, G1_AND, X_RCX, 0x00FFFFF3);
	_ri(e, G1_CMP, X_RCX, GBA_REG_TM0CNT_LO);
	*slow = _jcc(e, CC_NE);
	_mov(e, X_RSI, X_RDI);
	_movImm64(e, X_RDI, (uintptr_t) c->gba);
	_movImm64(e, X_RAX, (uintptr_t) _readTimer);
	_callC(e, X_RAX);
	_dataWait(c, i, 2);
}

// VRAM: stores tell the renderer about changed halfwords; byte stores stay with the handlers
static void _memVram(struct Compiler* c, const struct MemOp* mem, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned size = mem->size;
	_ri(e, G1_CMP, X_RAX, GBA_REGION_VRAM);
	*miss = _jcc(e, CC_NE);
	_mov(e, X_RCX, X_RDI);
	_ri(e, G1_AND, X_RCX, 0x1FFFF);
	_ri(e, G1_CMP, X_RCX, GBA_SIZE_VRAM);
	*slow = _jcc(e, CC_AE);
	_ri(e, G1_AND, X_RCX, size == 4 ? 0x1FFFC : size == 2 ? 0x1FFFE : 0x1FFFF);
	_movImm64(e, X_R11, (uintptr_t) c->gba->video.vram);
	_store(e, X_RDI, X_RSP, ADDRESS_SLOT);
	if (load) {
		_memAccess(e, true, size, X_R11);
		_mov(e, X_WB, X_RAX); // writeback is already stored for loads
	} else {
		if (size == 4) {
			_insn(e, 0, XO_LOAD, X_RDX, _xmi(X_R11, X_RCX, 0, 0));
			_rr(e, XO_CMP, X_RDX, X_R9);
		} else {
			_insn(e, 0, XO_MOVZX16, X_RDX, _xmi(X_R11, X_RCX, 0, 0));
			_insn(e, 0, XO_MOVZX16, X_R10, _xr(X_R9));
			_rr(e, XO_CMP, X_RDX, X_R10);
		}
		uint8_t* unchanged = _jcc(e, CC_E);
		_memAccess(e, false, size, X_R11);
		// Once per 4 KiB of VRAM between events, unless a tile cache wants every address
		_movImm64(e, X_RAX, (uintptr_t) &c->gba->video.renderer);
		_load64(e, X_RDI, X_RAX, 0);
		_groupImm(e, X_W, G1_CMP, _xm(X_RDI, offsetof(struct GBAVideoRenderer, cache)), 0);
		uint8_t* cached = _jcc(e, CC_NE);
		_load(e, X_RAX, X_RSP, ADDRESS_SLOT);
		_shift(e, SH_SHR, X_RAX, 12);
		_ri(e, G1_AND, X_RAX, 0x1F);
		_insn(e, 0, XO_BT, X_RAX, _xm(X_JIT, JIT_VRAM_NOTIFIED));
		uint8_t* notified = _jcc(e, CC_B);
		_insn(e, 0, XO_BTS, X_RAX, _xm(X_JIT, JIT_VRAM_NOTIFIED));
		_patch(cached, e->p);
		unsigned call;
		for (call = 0; call < (size == 4 ? 2 : 1); ++call) {
			_movImm64(e, X_RAX, (uintptr_t) &c->gba->video.renderer);
			_load64(e, X_RDI, X_RAX, 0);
			_load(e, X_RSI, X_RSP, ADDRESS_SLOT);
			_ri(e, G1_AND, X_RSI, size == 4 ? 0x1FFFC : 0x1FFFE);
			if (size == 4 && call == 0) {
				_ri(e, G1_ADD, X_RSI, 2);
			}
			_load64(e, X_RAX, X_RDI, offsetof(struct GBAVideoRenderer, writeVRAM));
			_callC(e, X_RAX);
		}
		_patch(notified, e->p);
		_load(e, X_RDI, X_RSP, ADDRESS_SLOT);
		_patch(unchanged, e->p);
	}
	_movImm64(e, X_RAX, (uintptr_t) &c->gba->video.stallMask);
	_load(e, X_RAX, X_RAX, 0);
	_rr(e, XO_TEST, X_RAX, X_RAX);
	uint8_t* noStall = _jcc(e, CC_E);
	_mov(e, X_RSI, X_RDI);
	_movImm64(e, X_RDI, (uintptr_t) c->gba);
	_movImm(e, X_RDX, size);
	_movImm64(e, X_RAX, (uintptr_t) GBAMemoryVRAMWait);
	_callC(e, X_RAX);
	_mov(e, X_R10, X_RAX);
	_load(e, X_RDI, X_RSP, ADDRESS_SLOT);
	uint8_t* waited = _jmp(e);
	_patch(noStall, e->p);
	_movImm(e, X_R10, size == 4 ? 1 : 0);
	_patch(waited, e->p);
	_ri(e, G1_ADD, X_R10, load ? 2 : 1);
	if (load) {
		_mov(e, X_RAX, X_WB);
	}
}

// Cartridge reads: no prefetch buffer stall for these addresses; region 0x0D is EEPROM
static void _memCart(struct Compiler* c, const struct MemOp* mem, uint8_t** miss, uint8_t** slow) {
	struct Emitter* e = &c->e;
	unsigned size = mem->size;
	_mov(e, X_RCX, X_RAX);
	_ri(e, G1_SUB, X_RCX, GBA_REGION_ROM0);
	_ri(e, G1_CMP, X_RCX, GBA_REGION_ROM2 - GBA_REGION_ROM0);
	*miss = _jcc(e, CC_A);
	_mov(e, X_RCX, X_RDI);
	_ri(e, G1_AND, X_RCX, GBA_SIZE_ROM0 - size);
	_ri(e, G1_CMP, X_RCX, c->gba->memory.romSize);
	*slow = _jcc(e, CC_AE);
	_movImm64(e, X_R11, (uintptr_t) c->gba->memory.rom);
	_memAccess(e, true, size, X_R11);
	_movImm64(e, X_R11, (uintptr_t) (size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16));
	_mov(e, X_RCX, X_RDI);
	_shift(e, SH_SHR, X_RCX, 24);
	_insn(e, 0, XO_MOVZX8, X_R10, _xmi(X_R11, X_RCX, 0, 0));
	_ri(e, G1_ADD, X_R10, 2);
}

// Everything else goes through the memory handlers
static void _memSlow(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	unsigned size = mem->size;
	_storeState(c, i + 1);
	_storeImm(e, X_RSP, CYCLE_SLOT, 0);
	_mov(e, X_RSI, X_RDI);
	_mov64(e, X_RDI, X_CPU);
	unsigned offset;
	if (mem->load) {
		_lea64(e, X_RDX, X_RSP, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, load32) : size == 2 ? offsetof(struct ARMMemory, load16) : offsetof(struct ARMMemory, load8);
	} else {
		// Narrow values go sign-extended, as clang expects of int16_t and int8_t arguments
		_insn(e, 0, size == 4 ? XO_LOAD : size == 2 ? XO_MOVSX16 : XO_MOVSX8, X_RDX, _xr(X_R9));
		_lea64(e, X_RCX, X_RSP, CYCLE_SLOT);
		offset = size == 4 ? offsetof(struct ARMMemory, store32) : size == 2 ? offsetof(struct ARMMemory, store16) : offsetof(struct ARMMemory, store8);
	}
	_load64(e, X_RAX, X_CPU, OFF_MEMORY + offset);
	_callC(e, X_RAX);
	_load(e, X_R10, X_RSP, CYCLE_SLOT);
	if (!mem->load) {
		// The interpreter charges a store N - S as the store leaves them, not as compiled
		unsigned nonseq;
		unsigned seq;
		_fetchWaits(c, &nonseq, &seq);
		_insn(e, 0, XO_ADD_RM, X_R10, _xm(X_CPU, nonseq));
		_insn(e, 0, XO_SUB_RM, X_R10, _xm(X_CPU, seq));
		_ri(e, G1_SUB, X_R10, c->memCycles - c->aluCycles);
	}
}

static void _emitMem(struct Compiler* c, unsigned i, const struct MemOp* mem) {
	struct Emitter* e = &c->e;
	bool load = mem->load;
	unsigned k = c->nCold;
	bool cold = false;

	uint8_t* fail = NULL;
	if (mem->cond != 0xE) {
		fail = _condJump(c, mem->cond, false);
	}
	uint32_t literal;
	void* literalHost;
	int32_t literalWait;
	switch (_literal(c, mem, &literal, &literalHost, &literalWait)) {
	case LITERAL_CONSTANT:
		_movImm(e, X_RAX, literal);
		_movImm(e, X_R10, literalWait);
		goto tail;
	case LITERAL_RAM:
		_movImm64(e, X_RAX, (uintptr_t) literalHost);
		_load(e, X_RAX, X_RAX, 0);
		_dataWait(c, i, literalWait);
		goto tail;
	}
	if (!load) {
		_loadSource(c, X_R9, _reg(mem->rd));
	}
	if (mem->runtimeOffset) {
		_load(e, X_RDX, X_JIT, JIT_FETCHED + 4 * mem->fetchedIndex);
		_mov(e, X_RCX, X_RDX);
		_ri(e, G1_AND, X_RDX, 0xFFF);
		_insn(e, 0, XO_GROUP8, G8_BT, _xr(X_RCX));
		_byte(e, 23);
		uint8_t* up = _jcc(e, CC_B);
		_insn(e, 0, XO_GROUP3, G3_NEG, _xr(X_RDX));
		_patch(up, e->p);
	} else if (mem->immediateOffset) {
		_movImm(e, X_RDX, mem->offset);
	} else {
		_loadSource(c, X_R11, mem->m);
		_shiftImm(c, mem->shiftType, mem->shiftAmount, false);
	}
	_loadSource(c, X_RSI, mem->base);
	_mov(e, X_R10, X_RSI);
	_rr(e, mem->up || mem->runtimeOffset ? XO_ADD : XO_SUB, X_R10, X_RDX);
	if (mem->writeback) {
		_mov(e, X_WB, X_R10);
	}
	_mov(e, X_RDI, mem->pre ? X_R10 : X_RSI);
	if (load && mem->writeback) {
		_store(e, X_WB, X_CPU, 4 * mem->base.reg);
	}
	if (mem->signExtend && mem->size == 2) {
		_store(e, X_RDI, X_RSP, ADDRESS_SLOT); // the address decides how LDRSH extends
	}

	cold = true;
	c->cold[k].index = i;
	c->cold[k].mem = *mem;
	c->cold[k].miss[0] = NULL;
	c->cold[k].miss[1] = NULL;
	c->cold[k].smc = NULL;
	_mov(e, X_RAX, X_RDI);
	_shift(e, SH_SHR, X_RAX, 24);
	int path = _memPath(c, mem);
	switch (path) {
	case PATH_IWRAM:
	case PATH_EWRAM:
		c->cold[k].smcEwram = path == PATH_EWRAM;
		_ri(e, G1_CMP, X_RAX, c->cold[k].smcEwram ? GBA_REGION_EWRAM : GBA_REGION_IWRAM);
		c->cold[k].miss[0] = _jcc(e, CC_NE);
		c->cold[k].smc = _memRam(c, i, mem, c->cold[k].smcEwram);
		break;
	case PATH_VRAM:
		_memVram(c, mem, &c->cold[k].miss[0], &c->cold[k].miss[1]);
		break;
	case PATH_CART:
		_memCart(c, mem, &c->cold[k].miss[0], &c->cold[k].miss[1]);
		break;
	default:
		c->cold[k].miss[0] = _jmp(e);
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
		toChecked[nChecked++] = _jmp(e);
	}
	for (x = 0; x < 2; ++x) {
		if (c->cold[k].miss[x]) {
			_patch(c->cold[k].miss[x], e->p);
		}
	}
	for (x = 0; x < 2; ++x) {
		_ri(e, G1_CMP, X_RAX, x ? GBA_REGION_EWRAM : GBA_REGION_IWRAM);
		miss = _jcc(e, CC_NE);
		smc = _memRam(c, i, mem, x);
		_jumpTo(c, c->cold[k].done);
		if (smc) {
			_patch(smc, e->p);
			_memInvalidate(c, i, mem, x);
			toChecked[nChecked++] = _jmp(e);
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

// Jumps to the returned site unless the word the pipeline fetched for ops[i], masked, is value
static uint8_t* _fetchedMismatch(struct Compiler* c, unsigned i, uint32_t mask, uint32_t value) {
	struct Emitter* e = &c->e;
	_load(e, X_RAX, X_JIT, JIT_FETCHED + 4 * i);
	if (mask != 0xFFFFFFFF) {
		_ri(e, G1_AND, X_RAX, mask);
	}
	_ri(e, G1_CMP, X_RAX, value);
	return _jcc(e, CC_NE);
}

static uint8_t* _forwardJump(struct Compiler* c) {
	return _jmp(&c->e);
}

// A patched instruction through its handler, with the opcode the pipeline fetched
static void _emitDynamicHandler(struct Compiler* c, unsigned i) {
	struct Emitter* e = &c->e;
	uint32_t address = c->pc + c->width * i;

	_storeState(c, i + 1);
	_load(e, X_RSI, X_JIT, JIT_FETCHED + 4 * i);
	uint8_t* toCheck = NULL;
	if (c->thumb) {
		_mov(e, X_RAX, X_RSI);
		_shift(e, SH_SHR, X_RAX, 6);
		_movImm64(e, X_RDX, (uintptr_t) _thumbTable);
		_loadScaled64(e, X_RAX, X_RDX, X_RAX, 0);
	} else {
		_load(e, X_RAX, X_CPU, OFF_CPSR);
		_shift(e, SH_SHR, X_RAX, 28);
		_mov(e, X_RCX, X_RSI);
		_shift(e, SH_SHR, X_RCX, 28);
		_movImm64(e, X_RDX, (uintptr_t) _conditionLut);
		_insn(e, 0, XO_MOVZX16, X_RDX, _xmi(X_RDX, X_RCX, 1, 0));
		_insn(e, 0, XO_BT, X_RAX, _xr(X_RDX));
		uint8_t* toExec = _jcc(e, CC_B);
		_addCycles(c, c->aluCycles);
		toCheck = _jmp(e);
		_patch(toExec, e->p);
		_mov(e, X_RAX, X_RSI);
		_shift(e, SH_SHR, X_RAX, 16);
		_ri(e, G1_AND, X_RAX, 0xFF0);
		_mov(e, X_RCX, X_RSI);
		_shift(e, SH_SHR, X_RCX, 4);
		_ri(e, G1_AND, X_RCX, 0xF);
		_rr(e, XO_OR, X_RAX, X_RCX);
		_movImm64(e, X_RDX, (uintptr_t) _armTable);
		_loadScaled64(e, X_RAX, X_RDX, X_RAX, 0);
	}
	_mov64(e, X_RDI, X_CPU);
	_callC(e, X_RAX);
	_load(e, X_RAX, X_CPU, OFF_PC);
	_ri(e, G1_CMP, X_RAX, address + 2 * c->width);
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
	unsigned cond = _condition(op, c->thumb);

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
	_movImm64(e, X_RAX, (uintptr_t) _handler(op, c->thumb));
	_callC(e, X_RAX);
	_load(e, X_RAX, X_CPU, OFF_PC);
	_ri(e, G1_CMP, X_RAX, address + 2 * c->width);
	uint8_t* sequential = _jcc(e, CC_E);
	struct BranchOp branch;
	if (!_branchTarget(c, i, &branch)) {
		_exitJump(c, EXIT_DIRECT);
	} else if (branch.target == c->pc) {
		// A taken branch back to the start of this block keeps running here
		_ri(e, G1_CMP, X_RAX, c->pc + c->width);
		_exitAt(c, _jcc(e, CC_NE), EXIT_DIRECT);
		_cmpByte(e, _xm(X_JIT, JIT_SMC_HIT), 0);
		_exitAt(c, _jcc(e, CC_NE), EXIT_DIRECT);
		_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
		_exitAt(c, _jcc(e, CC_GE), EXIT_EVENTS);
		c->loops[c->nLoops++] = _jmp(e);
	} else {
		_cmpRegMem(e, X_CYCLES, X_CPU, OFF_NEXT_EVENT);
		_exitAt(c, _jcc(e, CC_GE), EXIT_EVENTS);
		_linkJump(c);
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
		_insn(e, 0, c->thumb ? XO_MOVZX16 : XO_LOAD, X_RCX, _xm(X_RAX, 0));
		_store(e, X_RCX, X_CPU, offset);
	}
}

// Branches within a region, as GBASetActiveRegion and ARMWritePC or ThumbWritePC run them
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
	_memImm(e, G1_CMP, X_RAX, offsetof(struct GBA, memory.activeRegion), region);
	slow[nSlow++] = _jcc(e, CC_NE);
	if (region != GBA_REGION_BIOS) {
		_memImm(e, G1_CMP, X_RAX, offsetof(struct GBA, idleOptimization), IDLE_LOOP_DETECT);
		slow[nSlow++] = _jcc(e, CC_GE);
		_memImm(e, G1_CMP, X_RAX, offsetof(struct GBA, idleLoop), b->target);
		slow[nSlow++] = _jcc(e, CC_E);
	}
	if (b->thumbLink) {
		_memImm(e, G1_CMP, X_CPU, 4 * ARM_LR, b->lr);
		slow[nSlow++] = _jcc(e, CC_NE);
	}
	_storeImm(e, X_RAX, offsetof(struct GBA, lastJump), b->target);
	_storeImm(e, X_RAX, offsetof(struct GBA, memory.lastPrefetchedPc), 0);
	if (c->thumb) {
		_memImm(e, G1_OR, X_CPU, OFF_ACTIVE_MASK, WORD_SIZE_THUMB);
	} else {
		_memImm(e, G1_AND, X_CPU, OFF_ACTIVE_MASK, -WORD_SIZE_ARM);
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
		_exitAt(c, _jcc(e, CC_GE), EXIT_EVENTS);
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
		_mov(e, X_RCX, X_RDI);
		if (k) {
			_ri(e, G1_ADD, X_RCX, 4 * k);
		}
		_ri(e, G1_AND, X_RCX, mask);
		if (m->load) {
			_insn(e, 0, XO_LOAD, X_RDX, _xmi(base, X_RCX, 0, 0));
			_store(e, X_RDX, X_CPU, 4 * r);
		} else {
			_load(e, X_RDX, X_CPU, 4 * r);
			_insn(e, 0, XO_STORE, X_RDX, _xmi(base, X_RCX, 0, 0));
		}
		++k;
	}
}

// Jumps to the returned site when a word the store writes holds compiled code
static uint8_t* _multiCoverCheck(struct Compiler* c, const struct MultiOp* m, uint32_t mask, unsigned coverBase) {
	struct Emitter* e = &c->e;
	unsigned n = __builtin_popcount(m->list);
	_rr(e, XO_XOR, X_RAX, X_RAX);
	unsigned k;
	for (k = 0; k < n; ++k) {
		_mov(e, X_RCX, X_RDI);
		if (k) {
			_ri(e, G1_ADD, X_RCX, 4 * k);
		}
		_ri(e, G1_AND, X_RCX, mask);
		_shift(e, SH_SHR, X_RCX, 2);
		if (coverBase) {
			_ri(e, G1_ADD, X_RCX, coverBase);
		}
		_insn(e, 0, XO_OR8_RM, X_RAX, _xmi(X_JIT, X_RCX, 0, JIT_COVER));
	}
	_rr(e, XO_TEST, X_RAX, X_RAX);
	return _jcc(e, CC_NE);
}

// LDM and STM on IWRAM or EWRAM, timed as GBALoadMultiple and GBAStoreMultiple do
static void _emitMulti(struct Compiler* c, unsigned i, const struct MultiOp* m) {
	struct Emitter* e = &c->e;
	unsigned n = __builtin_popcount(m->list);
	uint8_t* fail = NULL;
	if (m->cond != 0xE) {
		fail = _condJump(c, m->cond, false);
	}
	_load(e, X_RSI, X_CPU, 4 * m->rn);
	_mov(e, X_RDI, X_RSI);
	int32_t start = m->up ? (m->pre ? 4 : 0) : (m->pre ? -4 * (int32_t) n : -4 * (int32_t) n + 4);
	if (start) {
		_ri(e, G1_ADD, X_RDI, start);
	}
	_mov(e, X_WB, X_RSI);
	_ri(e, G1_ADD, X_WB, m->up ? 4 * n : -4 * n);
	_mov(e, X_RAX, X_RDI);
	_shift(e, SH_SHR, X_RAX, 24);

	uint8_t* slow[3];
	unsigned nSlow = 0;
	uint8_t* done[2];
	unsigned pass;
	for (pass = 0; pass < 2; ++pass) {
		bool iwram = pass == 0;
		_ri(e, G1_CMP, X_RAX, iwram ? GBA_REGION_IWRAM : GBA_REGION_EWRAM);
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
		done[pass] = _jmp(e);
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
	_rr(e, XO_ADD, X_CYCLES, X_R10);
	_addCycles(c, c->memCycles);
	_skipped(c, fail);
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
	_mov(e, X_RAX, X_RCX);
	_shift(e, SH_SAR, X_RAX, 31);
	_rr(e, XO_XOR, X_RCX, X_RAX);
	_movImm(e, X_R10, m->rn >= 0 ? 2 : 1);
	unsigned k;
	for (k = 1; k < 4; ++k) {
		_ri(e, G1_CMP, X_RCX, 1 << (8 * k));
		_ri(e, G1_SBB, X_R10, -1);
	}
	if (c->romCode && c->prefetch) {
		_storeImm(e, X_CPU, OFF_PC, c->pc + c->width * (i + 2));
		_mov64(e, X_RDI, X_CPU);
		_mov(e, X_RSI, X_R10);
		_load64(e, X_RAX, X_CPU, OFF_MEMORY + offsetof(struct ARMMemory, stall));
		_callC(e, X_RAX);
		_mov(e, X_R10, X_RAX);
	}
	_load(e, X_RAX, X_CPU, 4 * m->rm);
	_insn(e, 0, XO_IMUL, X_RAX, _xm(X_CPU, 4 * m->rs));
	if (m->rn >= 0) {
		_insn(e, 0, XO_ADD_RM, X_RAX, _xm(X_CPU, 4 * m->rn));
	}
	_store(e, X_RAX, X_CPU, 4 * m->rd);
	if (m->s) {
		// N and Z from the result; ARM takes C from the last shifter result
		_mergeNZ(e, c->thumb ? 0x3FFFFFFF : 0x1FFFFFFF);
		if (!c->thumb) {
			_load(e, X_RCX, X_CPU, OFF_SHIFTER_CARRY);
			_ri(e, G1_AND, X_RCX, 1);
			_shift(e, SH_SHL, X_RCX, 29);
			_rr(e, XO_OR, X_R9, X_RCX);
		}
		_store(e, X_R9, X_CPU, OFF_CPSR);
	}
	_rr(e, XO_ADD, X_CYCLES, X_R10);
	_addCycles(c, c->memCycles);
	_skipped(c, fail);
	_eventCheck(c, i + 1);
}

// Runs the due events, then comes back to target unless the resume routine goes elsewhere
static void _resumeAfterEvents(struct Compiler* c, const uint8_t* target) {
	_patch(_callSite(&c->e), c->jit->resume);
	_jumpTo(c, target);
}

static void _emitColdPaths(struct Compiler* c) {
	unsigned k;
	for (k = 0; k < c->nCold; ++k) {
		_emitMemCold(c, k);
	}
}
