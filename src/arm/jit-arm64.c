/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/arm/jit.h>

#ifdef M_ARM_JIT

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/memory.h>

#include <stddef.h>
#include <sys/mman.h>

// Blocks run ARM code from GBA IWRAM. Every guest instruction keeps the interpreter's
// exact state: PC, the two prefetched words, cycles, and an event check afterwards.

#define MAX_BLOCK 64
#define MAX_SPAN (MAX_BLOCK + 2)
#define HOT_THRESHOLD 2
#define INVALIDATION_LIMIT 8
#define CODE_SIZE (8 * 1024 * 1024)
#define MAX_INSN_BYTES 640

#include <stdio.h>
static struct {
	uint64_t runs, compiles, compileFails, flushes, invalidations, prefetchMisses, interp;
} _stats;
static int _statsEnabled = -1;

static void _statsTick(void) {
	if (_statsEnabled < 0) {
		_statsEnabled = getenv("MGBA_JIT_STATS") != NULL;
	}
	if (_statsEnabled && !(_stats.runs & 0xFFFFF)) {
		fprintf(stderr, "jit: runs %llu compiles %llu fails %llu flushes %llu inval %llu pfmiss %llu interp %llu\n",
		        (unsigned long long) _stats.runs, (unsigned long long) _stats.compiles, (unsigned long long) _stats.compileFails,
		        (unsigned long long) _stats.flushes, (unsigned long long) _stats.invalidations, (unsigned long long) _stats.prefetchMisses,
		        (unsigned long long) _stats.interp);
	}
}

struct ARMJitBlock {
	void (*entry)(struct ARMCore*);
	uint32_t pc;
	uint32_t op0;
	uint32_t op1;
	unsigned start;
	unsigned span;
};

static const uint16_t _conditionLut[16] = {
	0xF0F0, 0x0F0F, 0xCCCC, 0x3333, 0xFF00, 0x00FF, 0xAAAA, 0x5555,
	0x0C0C, 0xF3F3, 0xAA55, 0x55AA, 0x0A05, 0xF5FA, 0xFFFF, 0x0000
};

enum {
	OFF_PC = offsetof(struct ARMCore, gprs) + 4 * ARM_PC,
	OFF_CPSR = offsetof(struct ARMCore, cpsr),
	OFF_CYCLES = offsetof(struct ARMCore, cycles),
	OFF_NEXT_EVENT = offsetof(struct ARMCore, nextEvent),
	OFF_PREFETCH0 = offsetof(struct ARMCore, prefetch),
	OFF_PREFETCH1 = offsetof(struct ARMCore, prefetch) + 4,
	OFF_SEQ32 = offsetof(struct ARMCore, memory) + offsetof(struct ARMMemory, activeSeqCycles32),
	OFF_SHIFTER_CARRY = offsetof(struct ARMCore, shifterCarryOut),
};

enum {
	A64_EQ = 0,
	A64_NE = 1,
	A64_GE = 10,
};

struct Emitter {
	uint32_t* p;
};

static void _emit(struct Emitter* e, uint32_t insn) {
	*e->p++ = insn;
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

static void _addWImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_emit(e, 0x11000000 | (imm << 10) | (rn << 5) | rd);
}

static void _addW(struct Emitter* e, int rd, int rn, int rm) {
	_emit(e, 0x0B000000 | (rm << 16) | (rn << 5) | rd);
}

static void _cmpW(struct Emitter* e, int rn, int rm) {
	_emit(e, 0x6B00001F | (rm << 16) | (rn << 5));
}

static void _lsrWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x53007C00 | (shift << 16) | (rn << 5) | rd);
}

static void _lsrvW(struct Emitter* e, int rd, int rn, int rm) {
	_emit(e, 0x1AC02400 | (rm << 16) | (rn << 5) | rd);
}

static void _movX(struct Emitter* e, int rd, int rm) {
	_emit(e, 0xAA0003E0 | (rm << 16) | rd);
}

static void _blr(struct Emitter* e, int rn) {
	_emit(e, 0xD63F0000 | (rn << 5));
}

static uint32_t* _bCond(struct Emitter* e, int cond) {
	uint32_t* at = e->p;
	_emit(e, 0x54000000 | cond);
	return at;
}

static uint32_t* _cbnzW(struct Emitter* e, int rt) {
	uint32_t* at = e->p;
	_emit(e, 0x35000000 | rt);
	return at;
}

static uint32_t* _tbnz(struct Emitter* e, int rt, unsigned bit) {
	uint32_t* at = e->p;
	_emit(e, 0x37000000 | (bit << 19) | rt);
	return at;
}

static uint32_t* _b(struct Emitter* e) {
	uint32_t* at = e->p;
	_emit(e, 0x14000000);
	return at;
}

static void _patch(uint32_t* at, const uint32_t* target) {
	int32_t offset = target - at;
	uint32_t insn = *at;
	if ((insn & 0xFC000000) == 0x14000000) {
		*at = insn | (offset & 0x3FFFFFF);
	} else if ((insn & 0x7E000000) == 0x36000000) {
		*at = insn | ((offset & 0x3FFF) << 5);
	} else {
		*at = insn | ((offset & 0x7FFFF) << 5);
	}
}

static void _movW(struct Emitter* e, int rd, int rm) {
	_emit(e, 0x2A0003E0 | (rm << 16) | rd);
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

static void _asrWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x13007C00 | (shift << 16) | (rn << 5) | rd);
}

static void _rorWImm(struct Emitter* e, int rd, int rn, unsigned shift) {
	_emit(e, 0x13800000 | (rn << 16) | (shift << 10) | (rn << 5) | rd);
}

static void _orrWShift(struct Emitter* e, int rd, int rn, int rm, unsigned lsl) {
	_emit(e, 0x2A000000 | (rm << 16) | (lsl << 10) | (rn << 5) | rd);
}

static void _addXImm(struct Emitter* e, int rd, int rn, unsigned imm) {
	_emit(e, 0x91000000 | (imm << 10) | (rn << 5) | rd);
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

static uint32_t* _cbzW(struct Emitter* e, int rt) {
	uint32_t* at = e->p;
	_emit(e, 0x34000000 | rt);
	return at;
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

static bool _endsBlock(uint32_t op) {
	if ((op & 0x0E000000) == 0x0A000000) {
		return true; // B, BL
	}
	if ((op & 0x0FFFFFF0) == 0x012FFF10) {
		return true; // BX
	}
	if ((op & 0x0C000000) == 0x0C000000) {
		return true; // Coprocessor, SWI
	}
	if ((op & 0x0C000000) == 0x00000000 && ((op >> 12) & 0xF) == ARM_PC) {
		return true;
	}
	if ((op & 0x0C100000) == 0x04100000 && ((op >> 12) & 0xF) == ARM_PC) {
		return true;
	}
	if ((op & 0x0E108000) == 0x08108000) {
		return true; // LDM with PC
	}
	return false;
}

static void _removeBlock(struct ARMJit* jit, struct ARMJitBlock* block) {
	jit->blocks[block->start] = NULL;
	unsigned i;
	for (i = 0; i < block->span; ++i) {
		--jit->cover[(block->start + i) & (ARM_JIT_IWRAM_WORDS - 1)];
	}
	if (block == jit->current) {
		jit->smcHit = 1;
		jit->current = NULL;
	}
	free(block);
}

static void _selfTestAlu(struct ARMJit* jit, unsigned iterations);

struct ARMJit* ARMJitCreate(void) {
	struct ARMJit* jit = calloc(1, sizeof(*jit));
	if (!jit) {
		return NULL;
	}
	jit->code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (jit->code == MAP_FAILED) {
		free(jit);
		return NULL;
	}
	jit->codeSize = CODE_SIZE;
	if (getenv("MGBA_JIT_SELFTEST")) {
		_selfTestAlu(jit, atoi(getenv("MGBA_JIT_SELFTEST")));
	}
	return jit;
}

void ARMJitDestroy(struct ARMJit* jit) {
	ARMJitFlush(jit);
	munmap(jit->code, jit->codeSize);
	free(jit);
}

static void _dropBlocks(struct ARMJit* jit) {
	unsigned i;
	for (i = 0; i < ARM_JIT_IWRAM_WORDS; ++i) {
		if (jit->blocks[i]) {
			if (jit->blocks[i] == jit->current) {
				jit->smcHit = 1;
				jit->current = NULL;
			}
			free(jit->blocks[i]);
		}
	}
	memset(jit->cover, 0, sizeof(jit->cover));
	memset(jit->blocks, 0, sizeof(jit->blocks));
	jit->codeUsed = 0;
	++_stats.flushes;
}

void ARMJitFlush(struct ARMJit* jit) {
	_dropBlocks(jit);
	memset(jit->hits, 0, sizeof(jit->hits));
	memset(jit->invalidations, 0, sizeof(jit->invalidations));
}

void ARMJitInvalidateWord(struct ARMJit* jit, unsigned word) {
	unsigned i;
	for (i = 0; i < MAX_SPAN && jit->cover[word]; ++i) {
		unsigned start = (word - i) & (ARM_JIT_IWRAM_WORDS - 1);
		struct ARMJitBlock* block = jit->blocks[start];
		if (block && i < block->span) {
			_removeBlock(jit, block);
			++_stats.invalidations;
			if (jit->invalidations[start] < 255) {
				++jit->invalidations[start];
			}
		}
	}
}

// Host registers that hold block-wide values
#define R_CPU 19
#define R_SMC 20
#define R_IWRAM 21
#define R_EWRAM 22
#define R_WB 23
#define R_COVER 24
#define FRAME_SIZE 80
#define CYCLE_SLOT 64
#define EXIT_DIRECT -1
#define MAX_EXITS (MAX_BLOCK * 6)

struct Compiler {
	struct Emitter e;
	struct ARMJit* jit;
	struct ARMCore* cpu;
	struct GBA* gba;
	uint32_t pc;
	const uint32_t* ops;
	unsigned count;
	uint32_t* body;
	uint32_t aluCycles;
	uint32_t memCycles;
	struct {
		uint32_t* at;
		int index;
	} exits[MAX_EXITS];
	unsigned nExits;
};

static void _exitAt(struct Compiler* c, uint32_t* at, int index) {
	c->exits[c->nExits].at = at;
	c->exits[c->nExits].index = index;
	++c->nExits;
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

static void _addCycles(struct Compiler* c, int reg, uint32_t constant) {
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

static void _eventCheck(struct Compiler* c, int index) {
	struct Emitter* e = &c->e;
	_ldrW(e, 0, R_CPU, OFF_CYCLES);
	_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
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

// Barrel shifter for an immediate shift amount: value in w1, carry out in w2 if wanted
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

enum {
	ALU_AND, ALU_EOR, ALU_SUB, ALU_RSB, ALU_ADD, ALU_ADC, ALU_SBC, ALU_RSC,
	ALU_TST, ALU_TEQ, ALU_CMP, ALU_CMN, ALU_ORR, ALU_MOV, ALU_BIC, ALU_MVN
};

static bool _isInlineAlu(uint32_t op) {
	if ((op & 0x0C000000) || (op >> 28) == 0xF) {
		return false;
	}
	if (!(op & 0x02000000) && (op & 0x10)) {
		return false; // Register-specified shift, multiply, halfword transfers, BX
	}
	unsigned opcode = (op >> 21) & 0xF;
	if (opcode >= ALU_TST && opcode <= ALU_CMN && !(op & 0x00100000)) {
		return false; // MRS, MSR
	}
	static int mask = -2;
	if (mask == -2) {
		mask = getenv("MGBA_JIT_ALUMASK") ? (int) strtol(getenv("MGBA_JIT_ALUMASK"), NULL, 16) : 0xFFFF;
	}
	if (!(mask & (1 << opcode))) {
		return false;
	}
	return ((op >> 12) & 0xF) != ARM_PC;
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
	bool logical = opcode == ALU_AND || opcode == ALU_EOR || opcode == ALU_TST || opcode == ALU_TEQ ||
	               opcode == ALU_ORR || opcode == ALU_MOV || opcode == ALU_BIC || opcode == ALU_MVN;
	bool carryIn = opcode == ALU_ADC || opcode == ALU_SBC || opcode == ALU_RSC;
	bool carryOut = (s && logical) || storeCarry;

	uint32_t* skip = NULL;
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

static bool _isInlineMem(uint32_t op) {
	unsigned cond = op >> 28;
	if (cond == 0xF) {
		return false;
	}
	bool p = op & (1 << 24);
	bool w = op & (1 << 21);
	unsigned rn = (op >> 16) & 0xF;
	unsigned rd = (op >> 12) & 0xF;
	if (rd == ARM_PC || (rn == ARM_PC && (!p || w))) {
		return false;
	}
	if ((op & 0x0C000000) == 0x04000000) {
		if ((op & 0x02000010) == 0x02000010) {
			return false; // Undefined
		}
		return p || !w; // LDRT and STRT stay in the interpreter
	}
	if ((op & 0x0E000090) == 0x00000090 && (op & 0x60) == 0x20) {
		return p || !w; // LDRH, STRH
	}
	return false;
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

	uint32_t* fail = NULL;
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
		unsigned type = (op >> 5) & 3;
		unsigned amount = (op >> 7) & 0x1F;
		_loadReg(c, 3, op & 0xF, address);
		_shiftImm(c, type, amount, false);
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

	uint32_t* toEwram;
	uint32_t* toSlow;
	uint32_t* done[2];
	_lsrWImm(e, 7, 4, 24);
	_emit(e, 0x7100001F | (3 << 10) | (7 << 5)); // cmp w7, #3
	toEwram = _bCond(e, A64_NE);
	_andImm(e, 8, 4, size == 4 ? 2 : size == 2 ? 1 : 0, size == 4 ? 13 : size == 2 ? 14 : 15);
	_memAccess(e, load, size, R_IWRAM);
	if (!load) {
		// Writes into compiled code
		_lsrWImm(e, 9, 8, 2);
		_emit(e, 0x38604800 | (9 << 16) | (R_COVER << 5) | 10); // ldrb w10, [x24, w9, uxtw]
		uint32_t* noCode = _cbzW(e, 10);
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
	_ldrX(e, 16, R_CPU, offsetof(struct ARMCore, memory) + offset);
	_blr(e, 16);
	_ldrW(e, 3, 31, CYCLE_SLOT);

	_patch(done[0], e->p);
	_patch(done[1], e->p);
	if (load) {
		_strW(e, 0, R_CPU, 4 * rd);
	} else if (writeback) {
		_strW(e, R_WB, R_CPU, 4 * rn);
	}
	_addCycles(c, 3, c->memCycles);
	if (fail) {
		uint32_t* after = _b(e);
		_patch(fail, e->p);
		_addCycles(c, -1, c->aluCycles);
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
	uint32_t* toCheck = NULL;
	if (cond != 0xE) {
		_ldrW(e, 0, R_CPU, OFF_CPSR);
		_lsrWImm(e, 0, 0, 28);
		_movImm32(e, 1, _conditionLut[cond]);
		_lsrvW(e, 1, 1, 0);
		uint32_t* toExec = _tbnz(e, 1, 0);
		_addCycles(c, -1, c->aluCycles);
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
	uint32_t* branched = _bCond(e, A64_NE);
	uint32_t* sequential = _b(e);

	// A taken branch back to the start of this block keeps running here
	_patch(branched, e->p);
	bool loops = false;
	if ((op & 0x0E000000) == 0x0A000000) {
		int32_t offset = (int32_t) (op << 8) >> 6;
		loops = address + 8 + offset == c->pc;
	}
	if (loops && getenv("MGBA_JIT_NOLOOP")) {
		loops = false;
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
		_exitAt(c, _b(e), EXIT_DIRECT);
	}

	_patch(sequential, e->p);
	if (toCheck) {
		_patch(toCheck, e->p);
	}
	_smcCheck(c, i + 1);
	_eventCheck(c, i + 1);
}

static struct ARMJitBlock* _compile(struct ARMJit* jit, struct ARMCore* cpu, uint32_t pc) {
	const uint32_t* region = cpu->memory.activeRegion;
	uint32_t mask = cpu->memory.activeMask;
	uint32_t ops[MAX_SPAN];
	unsigned count = 0;
	while (count < MAX_BLOCK) {
		LOAD_32(ops[count], (pc + 4 * count) & mask, region);
		++count;
		if (_endsBlock(ops[count - 1])) {
			break;
		}
	}
	LOAD_32(ops[count], (pc + 4 * count) & mask, region);
	LOAD_32(ops[count + 1], (pc + 4 * (count + 1)) & mask, region);
	if (ops[0] != cpu->prefetch[0] || ops[1] != cpu->prefetch[1]) {
		return NULL;
	}

	if (jit->codeUsed + count * MAX_INSN_BYTES + 512 > jit->codeSize) {
		// Keep the self-modification history so hot patched code stays interpreted
		_dropBlocks(jit);
	}

	struct ARMJitBlock* block = malloc(sizeof(*block));
	if (!block) {
		return NULL;
	}
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint32_t* code = (uint32_t*) &jit->code[jit->codeUsed];
	c->e.p = code;
	c->jit = jit;
	c->cpu = cpu;
	c->gba = (struct GBA*) cpu->master;
	c->pc = pc;
	c->ops = ops;
	c->count = count;
	c->nExits = 0;
	c->aluCycles = 1 + cpu->memory.activeSeqCycles32;
	c->memCycles = 1 + cpu->memory.activeNonseqCycles32;
	struct Emitter* e = &c->e;

	_emit(e, 0xA9800000 | ((-FRAME_SIZE / 8) & 0x7F) << 15 | (30 << 10) | (31 << 5) | 29); // stp x29, x30, [sp, #-FRAME_SIZE]!
	_stpX(e, 19, 20, 16);
	_stpX(e, 21, 22, 32);
	_stpX(e, 23, 24, 48);
	_movX(e, R_CPU, 0);
	_movImm64(e, R_SMC, (uintptr_t) &jit->smcHit);
	_movImm64(e, R_IWRAM, (uintptr_t) c->gba->memory.iwram);
	_movImm64(e, R_EWRAM, (uintptr_t) c->gba->memory.wram);
	_movImm64(e, R_COVER, (uintptr_t) jit->cover);
	c->body = e->p;

	static int noAlu = -1, noMem = -1;
	if (noAlu < 0) {
		noAlu = getenv("MGBA_JIT_NOALU") != NULL;
		noMem = getenv("MGBA_JIT_NOMEM") != NULL;
	}
	unsigned i = 0;
	while (i < count) {
		if (!noAlu && _isInlineAlu(ops[i])) {
			unsigned run = 1;
			while (i + run < count && !noAlu && !getenv("MGBA_JIT_RUN1") && _isInlineAlu(ops[i + run])) {
				++run;
			}
			// The interpreter checks for events after every instruction, so only run the
			// whole segment when none can come due before its last instruction
			if (run > 1) {
				_ldrW(e, 0, R_CPU, OFF_CYCLES);
				_ldrW(e, 1, R_CPU, OFF_NEXT_EVENT);
				_addWImm(e, 0, 0, c->aluCycles * (run - 1));
				_cmpW(e, 0, 1);
				_exitAt(c, _bCond(e, A64_GE), i);
			}
			unsigned lastAlways = 0;
			unsigned j;
			for (j = 0; j < run; ++j) {
				if ((ops[i + j] >> 28) == 0xE) {
					lastAlways = j;
				}
			}
			for (j = 0; j < run; ++j) {
				_emitAlu(c, i + j, j >= lastAlways);
			}
			_addCycles(c, -1, c->aluCycles * run);
			i += run;
			_eventCheck(c, i);
		} else if (!noMem && _isInlineMem(ops[i])) {
			_emitMem(c, i);
			++i;
		} else {
			_emitFallback(c, i);
			++i;
		}
	}
	_exitAt(c, _b(e), count);

	// Exit stubs set the interpreter state for the next instruction
	uint32_t* epilogue;
	uint32_t* stubs[MAX_SPAN];
	memset(stubs, 0, sizeof(stubs));
	unsigned x;
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		if (index != EXIT_DIRECT && !stubs[index]) {
			stubs[index] = e->p;
			_storeState(c, index);
			_exitAt(c, _b(e), EXIT_DIRECT);
		}
	}
	epilogue = e->p;
	_ldpX(e, 23, 24, 48);
	_ldpX(e, 21, 22, 32);
	_ldpX(e, 19, 20, 16);
	_emit(e, 0xA8C00000 | ((FRAME_SIZE / 8) << 15) | (30 << 10) | (31 << 5) | 29); // ldp x29, x30, [sp], #FRAME_SIZE
	_emit(e, 0xD65F03C0); // ret
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		_patch(c->exits[x].at, index == EXIT_DIRECT ? epilogue : stubs[index]);
	}

	if (getenv("MGBA_JIT_DUMP")) {
		static int dumped;
		if (dumped++ < 4) {
			char name[64];
			snprintf(name, sizeof(name), "%s/block%d-%08x.bin", getenv("MGBA_JIT_DUMP"), dumped, pc);
			FILE* f = fopen(name, "wb");
			fwrite(code, 1, (uint8_t*) e->p - (uint8_t*) code, f);
			fclose(f);
		}
	}
	__builtin___clear_cache((char*) code, (char*) e->p);
	jit->codeUsed += (uint8_t*) e->p - (uint8_t*) code;

	block->entry = (void (*)(struct ARMCore*)) code;
	block->pc = pc;
	block->op0 = ops[0];
	block->op1 = ops[1];
	block->start = (pc & 0x7FFF) >> 2;
	block->span = count + 2;
	jit->blocks[block->start] = block;
	for (i = 0; i < block->span; ++i) {
		++jit->cover[(block->start + i) & (ARM_JIT_IWRAM_WORDS - 1)];
	}
	return block;
}


// Debug: compare inline ALU code with the interpreter on random inputs
static void _selfTestAlu(struct ARMJit* jit, unsigned iterations) {
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint32_t seed = 12345;
	unsigned failures = 0, tested = 0;
	unsigned n;
	for (n = 0; n < iterations && failures < 20; ++n) {
		uint32_t op;
		do {
			seed = seed * 1103515245 + 12345; op = seed;
			seed = seed * 1103515245 + 12345; op ^= seed << 16;
		} while (!_isInlineAlu(op));
		struct ARMCore a;
		memset(&a, 0, sizeof(a));
		unsigned r;
		for (r = 0; r < 15; ++r) {
			seed = seed * 1103515245 + 12345;
			a.gprs[r] = (seed >> 3) ^ (seed << 29);
			if ((seed & 7) == 0) a.gprs[r] = 0;
			if ((seed & 7) == 1) a.gprs[r] = 0x80000000;
			if ((seed & 7) == 2) a.gprs[r] = 0xFFFFFFFF;
		}
		uint32_t address = 0x03001000;
		a.gprs[ARM_PC] = address + 8;
		seed = seed * 1103515245 + 12345;
		a.cpsr.packed = (seed & 0xF0000000) | 0x1F;
		struct ARMCore b = a;

		unsigned cond = op >> 28;
		if (_conditionLut[cond] & (1 << ((uint32_t) a.cpsr.packed >> 28))) {
			_armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)](&a, op);
		} else {
			a.cycles += 1;
		}

		uint32_t ops[3] = { op, 0, 0 };
		uint32_t* code = (uint32_t*) &jit->code[jit->codeUsed];
		c->e.p = code;
		c->jit = jit;
		c->pc = address;
		c->ops = ops;
		c->count = 1;
		c->nExits = 0;
		c->aluCycles = 1;
		c->memCycles = 1;
		struct Emitter* e = &c->e;
		_emit(e, 0xA9800000 | ((-FRAME_SIZE / 8) & 0x7F) << 15 | (30 << 10) | (31 << 5) | 29);
		_stpX(e, 19, 20, 16);
		_stpX(e, 21, 22, 32);
		_stpX(e, 23, 24, 48);
		_movX(e, R_CPU, 0);
		_emitAlu(c, 0, true);
		_addCycles(c, -1, 1);
		_ldpX(e, 23, 24, 48);
		_ldpX(e, 21, 22, 32);
		_ldpX(e, 19, 20, 16);
		_emit(e, 0xA8C00000 | ((FRAME_SIZE / 8) << 15) | (30 << 10) | (31 << 5) | 29);
		_emit(e, 0xD65F03C0);
		__builtin___clear_cache((char*) code, (char*) e->p);
		((void (*)(struct ARMCore*)) code)(&b);
		++tested;

		bool bad = a.cpsr.packed != b.cpsr.packed || a.cycles != b.cycles || (a.shifterCarryOut & 1) != (b.shifterCarryOut & 1);
		for (r = 0; r < 16; ++r) {
			bad = bad || a.gprs[r] != b.gprs[r];
		}
		if (bad) {
			++failures;
			fprintf(stderr, "ALU mismatch op %08X: cpsr %08X/%08X cycles %d/%d", op, a.cpsr.packed, b.cpsr.packed, a.cycles, b.cycles);
			for (r = 0; r < 16; ++r) {
				if (a.gprs[r] != b.gprs[r]) {
					fprintf(stderr, " r%u %08X/%08X", r, a.gprs[r], b.gprs[r]);
				}
			}
			fprintf(stderr, "\n");
		}
	}
	fprintf(stderr, "ALU self-test: %u tested, %u failures\n", tested, failures);
}

void ARMJitTrace(struct ARMCore* cpu, int kind) {
	static FILE* trace = NULL;
	static int64_t from = -1;
	static unsigned lines = 0;
	if (from == -1) {
		const char* env = getenv("MGBA_JIT_TRACE");
		from = env ? strtoll(env, NULL, 10) : -2;
		if (env) {
			trace = fopen(getenv("MGBA_JIT_TRACE_FILE"), "w");
		}
	}
	if (!trace || lines > 2000000) {
		return;
	}
	struct GBA* gba = (struct GBA*) cpu->master;
	int64_t now = (int64_t) gba->timing.masterCycles + cpu->cycles;
	if (now < from) {
		return;
	}
	uint32_t hash = 2166136261u;
	unsigned r;
	for (r = 0; r < 16; ++r) {
		hash = (hash ^ (uint32_t) cpu->gprs[r]) * 16777619u;
	}
	hash = (hash ^ (uint32_t) cpu->cpsr.packed) * 16777619u;
	fprintf(trace, "%lld %08X %08X %d", (long long) now, cpu->gprs[ARM_PC], hash, kind);
	for (r = 0; r < 16; ++r) {
		fprintf(trace, " %08X", cpu->gprs[r]);
	}
	fprintf(trace, " %08X %08X %08X\n", cpu->cpsr.packed, cpu->prefetch[0], cpu->prefetch[1]);
	++lines;
}

bool ARMJitRun(struct ARMCore* cpu) {
	struct ARMJit* jit = cpu->jit;
	uint32_t pc = cpu->gprs[ARM_PC] - WORD_SIZE_ARM;
	if ((pc >> 24) != 3) {
		return false;
	}
	unsigned word = (pc & 0x7FFF) >> 2;
	struct ARMJitBlock* block = jit->blocks[word];
	if (!block || block->pc != pc) {
		if (jit->invalidations[word] >= INVALIDATION_LIMIT) {
			return false;
		}
		if (jit->hits[word] < HOT_THRESHOLD) {
			++jit->hits[word];
			return false;
		}
		if (block) {
			_removeBlock(jit, block);
		}
		block = _compile(jit, cpu, pc);
		if (!block) {
			++_stats.compileFails;
			return false;
		}
		++_stats.compiles;
	}
	if (cpu->prefetch[0] != block->op0 || cpu->prefetch[1] != block->op1) {
		++_stats.prefetchMisses;
		return false;
	}
	++_stats.runs;
	_statsTick();
	jit->current = block;
	jit->smcHit = 0;
	int32_t cycles = cpu->cycles;
	block->entry(cpu);
	jit->current = NULL;
	// A block exits before its first segment when an event is due inside it
	return cpu->cycles != cycles;
}

#endif
