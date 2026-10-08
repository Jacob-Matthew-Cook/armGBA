/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/arm/jit.h>

#ifdef M_ARM_JIT

#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-arm.h>
#include <mgba/internal/arm/isa-thumb.h>
#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/memory.h>

#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>

// Blocks run ARM and Thumb code from BIOS, EWRAM, IWRAM and the cartridge. Every guest
// instruction keeps the interpreter's
// exact state: PC, the two prefetched words, cycles, and an event check afterwards.

#define MAX_BLOCK 64
#define MAX_SPAN ARM_JIT_MAX_SPAN
#define HOT_THRESHOLD 2
#define PATCH_LIMIT 4
#define CODE_SIZE (32 * 1024 * 1024)
#define MAX_INSN_BYTES 640
#define EXIT_DIRECT -1
#define EXIT_TO_C -2
#define MAX_EXITS (MAX_BLOCK * 6)

struct ARMJitBlock {
	void* entry;
	uint32_t pc;
	uint32_t op0;
	uint32_t op1;
	bool thumb;
	int coverStart;
	unsigned coverWords;
	// Patched words are read at runtime, so writes to them leave the block alone
	uint32_t coverMask[3];
};

struct ARMJitPage {
	struct ARMJitBlock* blocks[0x800];
	uint8_t hits[0x800];
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
	OFF_EXECUTION_MODE = offsetof(struct ARMCore, executionMode),
	JIT_CURRENT = offsetof(struct ARMJit, current),
	JIT_SMC_HIT = offsetof(struct ARMJit, smcHit),
	JIT_PAGES = offsetof(struct ARMJit, pages),
	JIT_FETCHED = offsetof(struct ARMJit, fetched),
	BLOCK_ENTRY = offsetof(struct ARMJitBlock, entry),
	BLOCK_PC = offsetof(struct ARMJitBlock, pc),
	BLOCK_OP0 = offsetof(struct ARMJitBlock, op0),
	BLOCK_OP1 = offsetof(struct ARMJitBlock, op1),
	BLOCK_THUMB = offsetof(struct ARMJitBlock, thumb),
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
	// Patched instructions and prefetch words, decoded at runtime from jit->fetched
	const bool* hot;
	unsigned count;
	bool thumb;
	unsigned width;
	// Data accesses from cartridge code go through the memory handlers for the prefetch buffer
	bool romCode;
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

static void* _handler(struct Compiler* c, uint32_t op) {
	if (c->thumb) {
		return (void*) _thumbTable[op >> 6];
	}
	return (void*) _armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)];
}

// Whether a taken branch at ops[i] goes back to the start of this block
static bool _loopsToStart(struct Compiler* c, unsigned i) {
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + c->width * i;
	int32_t offset;
	if (c->thumb) {
		if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {
			offset = (int8_t) op << 1;
		} else if ((op & 0xF800) == 0xE000) {
			offset = (int32_t) (op << 21) >> 20;
		} else {
			return false;
		}
	} else if ((op & 0x0E000000) == 0x0A000000) {
		offset = (int32_t) (op << 8) >> 6;
	} else {
		return false;
	}
	return address + 2 * c->width + offset == c->pc;
}

// Host address of a RAM word compiled from, for reading patched words at runtime
static void* _hostAddress(struct Compiler* c, uint32_t address) {
	if ((address >> 24) == GBA_REGION_IWRAM) {
		return (uint8_t*) c->gba->memory.iwram + (address & (GBA_SIZE_IWRAM - 1));
	}
	return (uint8_t*) c->gba->memory.wram + (address & (GBA_SIZE_EWRAM - 1));
}

// Each backend provides _prologue, _epilogue, _storeState, _addCycles, _segmentCheck,
// _eventCheck, _exitJump, _patch, _emitAlu, _emitMem and _emitFallback
#if defined(__aarch64__)
#include "jit-a64.h"
#else
#include "jit-x64.h"
#endif

static bool _endsBlock(uint32_t op, bool thumb) {
	if (thumb) {
		if ((op & 0xF000) == 0xD000 || (op & 0xF000) == 0xE000 || (op & 0xF800) == 0xF800) {
			return true; // B, conditional B, SWI, BL suffix, undefined
		}
		if ((op & 0xFF00) == 0x4700 || (op & 0xFF00) == 0xBD00 || (op & 0xFF00) == 0xBE00) {
			return true; // BX, POP with PC, BKPT
		}
		return ((op & 0xFF00) == 0x4400 || (op & 0xFF00) == 0x4600) && (op & 0x87) == 0x87; // ADD or MOV to PC
	}
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

static struct ARMJitPage* _page(struct ARMJit* jit, uint32_t address, bool create) {
	struct ARMJitPage** page = &jit->pages[(address >> 12) & (ARM_JIT_PAGES - 1)];
	if (!*page && create) {
		*page = calloc(1, sizeof(**page));
	}
	return *page;
}

static bool _covers(const struct ARMJitBlock* block, unsigned word) {
	unsigned i = word - block->coverStart;
	return i < block->coverWords && (block->coverMask[i / 32] & (1u << (i & 31)));
}

static void _removeBlock(struct ARMJit* jit, struct ARMJitBlock* block) {
	struct ARMJitPage* page = _page(jit, block->pc, false);
	page->blocks[(block->pc & 0xFFF) >> 1] = NULL;
	unsigned i;
	for (i = 0; i < block->coverWords; ++i) {
		if (block->coverMask[i / 32] & (1u << (i & 31))) {
			--jit->cover[block->coverStart + i];
		}
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
	unsigned i;
	for (i = 0; i < ARM_JIT_PAGES; ++i) {
		free(jit->pages[i]);
	}
	munmap(jit->code, jit->codeSize);
	free(jit);
}

// Waitstate changes and a full code buffer drop every block but keep the patch history
void ARMJitDropBlocks(struct ARMJit* jit) {
	unsigned i, j;
	for (i = 0; i < ARM_JIT_PAGES; ++i) {
		struct ARMJitPage* page = jit->pages[i];
		if (!page) {
			continue;
		}
		for (j = 0; j < 0x800; ++j) {
			if (page->blocks[j]) {
				if (page->blocks[j] == jit->current) {
					jit->smcHit = 1;
					jit->current = NULL;
				}
				free(page->blocks[j]);
				page->blocks[j] = NULL;
			}
		}
	}
	memset(jit->cover, 0, sizeof(jit->cover));
	jit->codeUsed = jit->codeStart;
}

void ARMJitFlush(struct ARMJit* jit) {
	ARMJitDropBlocks(jit);
	unsigned i;
	for (i = 0; i < ARM_JIT_PAGES; ++i) {
		if (jit->pages[i]) {
			memset(jit->pages[i]->hits, 0, sizeof(jit->pages[i]->hits));
		}
	}
	memset(jit->patched, 0, sizeof(jit->patched));
}

static uint32_t _ramWordAddress(unsigned word) {
	if (word < ARM_JIT_IWRAM_WORDS) {
		return GBA_BASE_IWRAM + 4 * word;
	}
	return GBA_BASE_EWRAM + 4 * (word - ARM_JIT_IWRAM_WORDS);
}

void ARMJitInvalidateWord(struct ARMJit* jit, unsigned word) {
	if (jit->patched[word] < 255) {
		++jit->patched[word];
	}
	// A block can start up to MAX_SPAN ARM words before the word it covers
	uint32_t address = _ramWordAddress(word);
	unsigned back;
	for (back = 0; back < 4 * MAX_SPAN + 4 && jit->cover[word]; back += 2) {
		uint32_t start = address + 2 - back;
		struct ARMJitPage* page = _page(jit, start, false);
		struct ARMJitBlock* block = page ? page->blocks[(start & 0xFFF) >> 1] : NULL;
		if (block && block->pc == start && _covers(block, word)) {
			_removeBlock(jit, block);
		}
	}
}

// Words that keep getting patched are read when the pipeline fetches them instead of being
// compiled in
static bool _isPatched(struct ARMJit* jit, uint32_t address) {
	int word = ARMJitRamWord(address);
	return word >= 0 && jit->patched[word] >= PATCH_LIMIT;
}

// End of the region a block at this address may run through, or 0 if it can't be compiled
static uint32_t _regionEnd(struct ARMCore* cpu, uint32_t pc) {
	struct GBA* gba = (struct GBA*) cpu->master;
	switch (pc >> 24) {
	case GBA_REGION_BIOS:
		return pc < GBA_SIZE_BIOS ? GBA_SIZE_BIOS : 0;
	case GBA_REGION_EWRAM:
		return pc < GBA_BASE_EWRAM + GBA_SIZE_EWRAM ? GBA_BASE_EWRAM + GBA_SIZE_EWRAM : 0;
	case GBA_REGION_IWRAM:
		return pc < GBA_BASE_IWRAM + GBA_SIZE_IWRAM ? GBA_BASE_IWRAM + GBA_SIZE_IWRAM : 0;
	case GBA_REGION_ROM0:
	case GBA_REGION_ROM0_EX:
	case GBA_REGION_ROM1:
	case GBA_REGION_ROM1_EX:
	case GBA_REGION_ROM2:
	case GBA_REGION_ROM2_EX:
		if ((pc & (GBA_SIZE_ROM0 - 1)) < gba->memory.romSize) {
			return (pc & ~(GBA_SIZE_ROM0 - 1)) + gba->memory.romSize;
		}
		return 0;
	default:
		return 0;
	}
}

static struct ARMJitBlock* _compile(struct ARMJit* jit, struct ARMCore* cpu, uint32_t pc, bool thumb) {
	const uint32_t* region = cpu->memory.activeRegion;
	uint32_t mask = cpu->memory.activeMask;
	unsigned width = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	uint32_t end = _regionEnd(cpu, pc);
	uint32_t ops[MAX_SPAN];
	bool hot[MAX_SPAN];
	unsigned count = 0;
	// The dispatcher checks the first two words against the pipeline, so they can't be patched
	if (_isPatched(jit, pc) || _isPatched(jit, pc + width)) {
		return NULL;
	}
	while (count < MAX_BLOCK && pc + width * (count + 3) <= end) {
		uint32_t address = pc + width * count;
		hot[count] = _isPatched(jit, address);
		if (thumb) {
			LOAD_16(ops[count], address & mask, region);
		} else {
			LOAD_32(ops[count], address & mask, region);
		}
		++count;
		if (!hot[count - 1] && _endsBlock(ops[count - 1], thumb)) {
			break;
		}
	}
	if (!count) {
		return NULL;
	}
	hot[count] = _isPatched(jit, pc + width * count);
	hot[count + 1] = _isPatched(jit, pc + width * (count + 1));
	if (thumb) {
		LOAD_16(ops[count], (pc + width * count) & mask, region);
		LOAD_16(ops[count + 1], (pc + width * (count + 1)) & mask, region);
	} else {
		LOAD_32(ops[count], (pc + width * count) & mask, region);
		LOAD_32(ops[count + 1], (pc + width * (count + 1)) & mask, region);
	}
	if (ops[0] != cpu->prefetch[0] || ops[1] != cpu->prefetch[1]) {
		return NULL;
	}

	if (jit->codeUsed + count * MAX_INSN_BYTES + 1024 > jit->codeSize) {
		ARMJitDropBlocks(jit);
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
	c->hot = hot;
	c->count = count;
	c->thumb = thumb;
	c->width = width;
	c->romCode = (pc >> 24) >= GBA_REGION_ROM0;
	c->nExits = 0;
	if (thumb) {
		c->aluCycles = 1 + cpu->memory.activeSeqCycles16;
		c->memCycles = 1 + cpu->memory.activeNonseqCycles16;
	} else {
		c->aluCycles = 1 + cpu->memory.activeSeqCycles32;
		c->memCycles = 1 + cpu->memory.activeNonseqCycles32;
	}

	c->body = c->e.p;

	unsigned i = 0;
	while (i < count) {
		if (hot[i]) {
			_fetchAhead(c, i);
			_emitDynamic(c, i);
			++i;
		} else if (!thumb && _isInlineAlu(ops[i])) {
			unsigned run = 1;
			while (i + run < count && !hot[i + run] && _isInlineAlu(ops[i + run])) {
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
				_fetchAhead(c, i + j);
				_emitAlu(c, i + j, j >= lastAlways);
			}
			_addCycles(c, c->aluCycles * run);
			i += run;
			_eventCheck(c, i);
		} else if (!thumb && _isInlineMem(ops[i])) {
			_fetchAhead(c, i);
			_emitMem(c, i);
			++i;
		} else {
			_fetchAhead(c, i);
			_emitFallback(c, i);
			++i;
		}
	}
	_exitJump(c, count);

	// Exit stubs set the interpreter state for the next instruction, then go to the
	// dispatcher. Leaving before the first instruction goes back to C, which steps it.
	uint8_t* stubs[MAX_SPAN];
	memset(stubs, 0, sizeof(stubs));
	unsigned x;
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		if (index >= 0 && !stubs[index]) {
			stubs[index] = c->e.p;
			_storeState(c, index);
			_exitJump(c, index ? EXIT_DIRECT : EXIT_TO_C);
		}
	}
	uint8_t* direct = c->e.p;
	_jumpTo(c, jit->dispatch);
	uint8_t* toC = c->e.p;
	_jumpTo(c, jit->toC);
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		_patch(c->exits[x].at, index == EXIT_DIRECT ? direct : index == EXIT_TO_C ? toC : stubs[index]);
	}

	__builtin___clear_cache((char*) code, (char*) c->e.p);
	jit->codeUsed += c->e.p - code;

	block->entry = code;
	block->pc = pc;
	block->op0 = ops[0];
	block->op1 = ops[1];
	block->thumb = thumb;
	block->coverStart = ARMJitRamWord(pc);
	block->coverWords = 0;
	memset(block->coverMask, 0, sizeof(block->coverMask));
	if (block->coverStart >= 0) {
		block->coverWords = ARMJitRamWord(pc + width * (count + 2) - 1) - block->coverStart + 1;
		for (i = 0; i < block->coverWords; ++i) {
			if (jit->patched[block->coverStart + i] < PATCH_LIMIT) {
				block->coverMask[i / 32] |= 1u << (i & 31);
				++jit->cover[block->coverStart + i];
			}
		}
	}
	_page(jit, pc, true)->blocks[(pc & 0xFFF) >> 1] = block;
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
		static const bool cold[3];
		c->ops = ops;
		c->hot = cold;
		c->count = 1;
		c->thumb = false;
		c->width = WORD_SIZE_ARM;
		c->romCode = false;
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

static void _buildTrampoline(struct ARMJit* jit, struct ARMCore* cpu) {
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint8_t* code = jit->code;
	c->e.p = code;
	c->jit = jit;
	c->cpu = cpu;
	c->gba = (struct GBA*) cpu->master;
	_emitTrampoline(c);
	__builtin___clear_cache((char*) code, (char*) c->e.p);
	jit->codeStart = c->e.p - code;
	jit->codeUsed = jit->codeStart;
}

bool ARMJitRun(struct ARMCore* cpu) {
	struct ARMJit* jit = cpu->jit;
	if (!jit->enter) {
		_buildTrampoline(jit, cpu);
	}
	bool thumb = cpu->executionMode == MODE_THUMB;
	uint32_t pc = cpu->gprs[ARM_PC] - (thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
	struct ARMJitPage* page = _page(jit, pc, false);
	unsigned index = (pc & 0xFFF) >> 1;
	struct ARMJitBlock* block = page ? page->blocks[index] : NULL;
	if (!block || block->pc != pc || block->thumb != thumb) {
		if (!_regionEnd(cpu, pc)) {
			return false;
		}
		if (!page) {
			page = _page(jit, pc, true);
		}
		if (page->hits[index] < HOT_THRESHOLD) {
			++page->hits[index];
			return false;
		}
		if (block) {
			_removeBlock(jit, block);
		}
		block = _compile(jit, cpu, pc, thumb);
		if (!block) {
			page->hits[index] = 0;
			return false;
		}
	}
	if (cpu->prefetch[0] != block->op0 || cpu->prefetch[1] != block->op1) {
		return false;
	}
	jit->current = block;
	jit->smcHit = 0;
	int32_t cycles = cpu->cycles;
	jit->enter(cpu, block->entry);
	jit->current = NULL;
	// A block exits before its first segment when an event is due inside it
	return cpu->cycles != cycles;
}

#endif
