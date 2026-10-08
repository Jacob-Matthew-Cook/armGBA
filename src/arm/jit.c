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
#include <stdio.h>
#include <sys/mman.h>

// Blocks run ARM code from GBA IWRAM. Every guest instruction keeps the interpreter's
// exact state: PC, the two prefetched words, cycles, and an event check afterwards.

#define MAX_BLOCK 64
#define MAX_SPAN (MAX_BLOCK + 2)
#define HOT_THRESHOLD 2
#define PATCH_LIMIT 4
#define CODE_SIZE (8 * 1024 * 1024)
#define MAX_INSN_BYTES 640
#define EXIT_DIRECT -1
#define MAX_EXITS (MAX_BLOCK * 6)

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
	OFF_SHIFTER_CARRY = offsetof(struct ARMCore, shifterCarryOut),
	OFF_MEMORY = offsetof(struct ARMCore, memory),
};

enum {
	ALU_AND, ALU_EOR, ALU_SUB, ALU_RSB, ALU_ADD, ALU_ADC, ALU_SBC, ALU_RSC,
	ALU_TST, ALU_TEQ, ALU_CMP, ALU_CMN, ALU_ORR, ALU_MOV, ALU_BIC, ALU_MVN
};

struct Emitter {
	uint8_t* p;
};

struct Compiler {
	struct Emitter e;
	struct ARMJit* jit;
	struct ARMCore* cpu;
	struct GBA* gba;
	uint32_t pc;
	const uint32_t* ops;
	unsigned count;
	uint8_t* body;
	uint32_t aluCycles;
	uint32_t memCycles;
	struct {
		uint8_t* at;
		int index;
	} exits[MAX_EXITS];
	unsigned nExits;
};

static void _exitAt(struct Compiler* c, uint8_t* at, int index) {
	c->exits[c->nExits].at = at;
	c->exits[c->nExits].index = index;
	++c->nExits;
}

static bool _isLogical(unsigned opcode) {
	return opcode == ALU_AND || opcode == ALU_EOR || opcode == ALU_TST || opcode == ALU_TEQ ||
	       opcode == ALU_ORR || opcode == ALU_MOV || opcode == ALU_BIC || opcode == ALU_MVN;
}

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
	return ((op >> 12) & 0xF) != ARM_PC;
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

// Each backend provides _prologue, _epilogue, _storeState, _addCycles, _segmentCheck,
// _eventCheck, _exitJump, _patch, _emitAlu, _emitMem and _emitFallback
#if defined(__aarch64__)
#include "jit-a64.h"
#else
#include "jit-x64.h"
#endif

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
}

void ARMJitFlush(struct ARMJit* jit) {
	_dropBlocks(jit);
	memset(jit->hits, 0, sizeof(jit->hits));
	memset(jit->patched, 0, sizeof(jit->patched));
}

void ARMJitInvalidateWord(struct ARMJit* jit, unsigned word) {
	if (jit->patched[word] < 255) {
		++jit->patched[word];
	}
	unsigned i;
	for (i = 0; i < MAX_SPAN && jit->cover[word]; ++i) {
		unsigned start = (word - i) & (ARM_JIT_IWRAM_WORDS - 1);
		struct ARMJitBlock* block = jit->blocks[start];
		if (block && i < block->span) {
			_removeBlock(jit, block);
		}
	}
}

// Words that keep getting patched stay out of blocks, along with the two words before
// them, whose prefetch would bake in the patched word
static bool _isPatched(struct ARMJit* jit, unsigned word) {
	return jit->patched[word & (ARM_JIT_IWRAM_WORDS - 1)] >= PATCH_LIMIT;
}

static struct ARMJitBlock* _compile(struct ARMJit* jit, struct ARMCore* cpu, uint32_t pc) {
	const uint32_t* region = cpu->memory.activeRegion;
	uint32_t mask = cpu->memory.activeMask;
	uint32_t ops[MAX_SPAN];
	unsigned start = (pc & 0x7FFF) >> 2;
	unsigned count = 0;
	while (count < MAX_BLOCK) {
		if (_isPatched(jit, start + count) || _isPatched(jit, start + count + 1) || _isPatched(jit, start + count + 2)) {
			break;
		}
		LOAD_32(ops[count], (pc + 4 * count) & mask, region);
		++count;
		if (_endsBlock(ops[count - 1])) {
			break;
		}
	}
	if (!count) {
		return NULL;
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
	uint8_t* code = &jit->code[jit->codeUsed];
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

	_prologue(c);
	c->body = c->e.p;

	unsigned i = 0;
	while (i < count) {
		if (_isInlineAlu(ops[i])) {
			unsigned run = 1;
			while (i + run < count && _isInlineAlu(ops[i + run])) {
				++run;
			}
			// The interpreter checks for events after every instruction, so only run the
			// whole segment when none can come due before its last instruction
			if (run > 1) {
				_segmentCheck(c, i, c->aluCycles * (run - 1));
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
			_addCycles(c, c->aluCycles * run);
			i += run;
			_eventCheck(c, i);
		} else if (_isInlineMem(ops[i])) {
			_emitMem(c, i);
			++i;
		} else {
			_emitFallback(c, i);
			++i;
		}
	}
	_exitJump(c, count);

	// Exit stubs set the interpreter state for the next instruction
	uint8_t* stubs[MAX_SPAN];
	memset(stubs, 0, sizeof(stubs));
	unsigned x;
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		if (index != EXIT_DIRECT && !stubs[index]) {
			stubs[index] = c->e.p;
			_storeState(c, index);
			_exitJump(c, EXIT_DIRECT);
		}
	}
	uint8_t* epilogue = c->e.p;
	_epilogue(c);
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		_patch(c->exits[x].at, index == EXIT_DIRECT ? epilogue : stubs[index]);
	}

	__builtin___clear_cache((char*) code, (char*) c->e.p);
	jit->codeUsed += c->e.p - code;

	block->entry = (void (*)(struct ARMCore*)) code;
	block->pc = pc;
	block->op0 = ops[0];
	block->op1 = ops[1];
	block->start = start;
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
			seed = seed * 1103515245 + 12345;
			op = seed;
			seed = seed * 1103515245 + 12345;
			op ^= seed << 16;
		} while (!_isInlineAlu(op));
		struct ARMCore a;
		memset(&a, 0, sizeof(a));
		unsigned r;
		for (r = 0; r < 15; ++r) {
			seed = seed * 1103515245 + 12345;
			a.gprs[r] = (seed >> 3) ^ (seed << 29);
			if ((seed & 7) == 0) {
				a.gprs[r] = 0;
			} else if ((seed & 7) == 1) {
				a.gprs[r] = 0x80000000;
			} else if ((seed & 7) == 2) {
				a.gprs[r] = 0xFFFFFFFF;
			}
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
		uint8_t* code = &jit->code[jit->codeUsed];
		c->e.p = code;
		c->jit = jit;
		c->gba = NULL;
		c->pc = address;
		c->ops = ops;
		c->count = 1;
		c->nExits = 0;
		c->aluCycles = 1;
		c->memCycles = 1;
		_prologue(c);
		_emitAlu(c, 0, true);
		_addCycles(c, 1);
		_epilogue(c);
		__builtin___clear_cache((char*) code, (char*) c->e.p);
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

bool ARMJitRun(struct ARMCore* cpu) {
	struct ARMJit* jit = cpu->jit;
	uint32_t pc = cpu->gprs[ARM_PC] - WORD_SIZE_ARM;
	if ((pc >> 24) != 3) {
		return false;
	}
	unsigned word = (pc & 0x7FFF) >> 2;
	struct ARMJitBlock* block = jit->blocks[word];
	if (!block || block->pc != pc) {
		if (jit->hits[word] < HOT_THRESHOLD) {
			++jit->hits[word];
			return false;
		}
		if (block) {
			_removeBlock(jit, block);
		}
		block = _compile(jit, cpu, pc);
		if (!block) {
			jit->hits[word] = 0;
			return false;
		}
	}
	if (cpu->prefetch[0] != block->op0 || cpu->prefetch[1] != block->op1) {
		return false;
	}
	jit->current = block;
	jit->smcHit = 0;
	int32_t cycles = cpu->cycles;
	block->entry(cpu);
	jit->current = NULL;
	// A block exits before its first segment when an event is due inside it
	return cpu->cycles != cycles;
}

#endif
