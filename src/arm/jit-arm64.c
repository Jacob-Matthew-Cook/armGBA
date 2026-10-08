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
#define MAX_INSN_BYTES 256

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

	if (jit->codeUsed + count * MAX_INSN_BYTES + 128 > jit->codeSize) {
		// Keep the self-modification history so hot patched code stays interpreted
		_dropBlocks(jit);
	}

	struct ARMJitBlock* block = malloc(sizeof(*block));
	if (!block) {
		return NULL;
	}
	uint32_t* code = (uint32_t*) &jit->code[jit->codeUsed];
	struct Emitter e = { code };
	uint32_t* exits[MAX_BLOCK * 3];
	unsigned nExits = 0;

	_emit(&e, 0xA9BE7BFD); // stp x29, x30, [sp, #-32]!
	_emit(&e, 0xA90153F3); // stp x19, x20, [sp, #16]
	_movX(&e, 19, 0);
	_movImm64(&e, 20, (uintptr_t) &jit->smcHit);

	unsigned i;
	for (i = 0; i < count; ++i) {
		uint32_t address = pc + 4 * i;
		uint32_t op = ops[i];
		unsigned cond = op >> 28;

		_movImm32(&e, 0, address + 8);
		_strW(&e, 0, 19, OFF_PC);
		_movImm32(&e, 0, ops[i + 1]);
		_strW(&e, 0, 19, OFF_PREFETCH0);
		_movImm32(&e, 0, ops[i + 2]);
		_strW(&e, 0, 19, OFF_PREFETCH1);

		uint32_t* toCheck = NULL;
		if (cond != 0xE) {
			_ldrW(&e, 0, 19, OFF_CPSR);
			_lsrWImm(&e, 0, 0, 28);
			_movImm32(&e, 1, _conditionLut[cond]);
			_lsrvW(&e, 1, 1, 0);
			uint32_t* toExec = _tbnz(&e, 1, 0);
			_ldrW(&e, 0, 19, OFF_SEQ32);
			_ldrW(&e, 1, 19, OFF_CYCLES);
			_addW(&e, 1, 1, 0);
			_addWImm(&e, 1, 1, 1);
			_strW(&e, 1, 19, OFF_CYCLES);
			toCheck = _b(&e);
			_patch(toExec, e.p);
		}

		_movX(&e, 0, 19);
		_movImm32(&e, 1, op);
		_movImm64(&e, 16, (uintptr_t) _armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)]);
		_blr(&e, 16);
		_ldrW(&e, 0, 19, OFF_PC);
		_movImm32(&e, 1, address + 8);
		_cmpW(&e, 0, 1);
		exits[nExits++] = _bCond(&e, A64_NE);

		if (toCheck) {
			_patch(toCheck, e.p);
		}
		_ldrbW(&e, 0, 20, 0);
		exits[nExits++] = _cbnzW(&e, 0);
		_ldrW(&e, 0, 19, OFF_CYCLES);
		_ldrW(&e, 1, 19, OFF_NEXT_EVENT);
		_cmpW(&e, 0, 1);
		exits[nExits++] = _bCond(&e, A64_GE);
	}

	uint32_t* exit = e.p;
	_emit(&e, 0xA94153F3); // ldp x19, x20, [sp, #16]
	_emit(&e, 0xA8C27BFD); // ldp x29, x30, [sp], #32
	_emit(&e, 0xD65F03C0); // ret
	for (i = 0; i < nExits; ++i) {
		_patch(exits[i], exit);
	}
	__builtin___clear_cache((char*) code, (char*) e.p);
	jit->codeUsed += (uint8_t*) e.p - (uint8_t*) code;

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
	block->entry(cpu);
	jit->current = NULL;
	return true;
}

#endif
