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
#include <mgba/internal/gba/io.h>

#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>

#include "jit-ops.h"

// Blocks keep the interpreter's exact state after every instruction: PC, pipeline, cycles and events

#define MAX_BLOCK 64
#define MAX_SPAN ARM_JIT_MAX_SPAN
#define HOT_THRESHOLD 2
#define PATCH_LIMIT 4
#define CODE_SIZE (32 * 1024 * 1024)
#define MAX_INSN_BYTES 2048
// Exits go to the dispatcher, to the due events, or to a stub that stores the state before an instruction
#define EXIT_DIRECT -1
#define EXIT_EVENTS -2
#define EXIT_DUE 0x1000
#define MAX_EXITS (MAX_BLOCK * 16)

// An exit with a known target, patched to jump straight there while the target is compiled
struct ARMJitLink {
	uint8_t* site;
	uint8_t* stub;
	struct ARMJitBlock* target;
	struct ARMJitLink* next;
	struct ARMJitLink** prev;
};

// Every boundary a block can be left at is an entry, so resuming after an event jumps back in
struct ARMJitEntry {
	void* code;
	uint32_t pc;
	uint32_t op0;
	uint32_t op1;
	// Patched words are taken from the pipeline instead of checked against it
	uint32_t opMask0;
	uint32_t opMask1;
	bool thumb;
	struct ARMJitBlock* block;
};

struct ARMJitBlock {
	uint32_t pc;
	int coverStart;
	unsigned coverWords;
	// Patched words are read at runtime, so writes to them leave the block alone
	uint32_t coverMask[3];
	struct ARMJitEntry* entries;
	unsigned nEntries;
	struct ARMJitLink* links;
	unsigned nLinks;
	struct ARMJitLink* incoming;
	struct ARMJitBlock* next;
	struct ARMJitBlock** prev;
};

struct ARMJitPage {
	struct ARMJitEntry* entries[0x800];
	uint8_t hits[0x800];
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
	OFF_ACTIVE_MASK = offsetof(struct ARMCore, memory) + offsetof(struct ARMMemory, activeMask),
	JIT_SMC_HIT = offsetof(struct ARMJit, smcHit),
	JIT_VRAM_NOTIFIED = offsetof(struct ARMJit, vramNotified),
	JIT_COVER = offsetof(struct ARMJit, cover),
	JIT_PAGES = offsetof(struct ARMJit, pages),
	JIT_PENDING_LINK = offsetof(struct ARMJit, pendingLink),
	JIT_FETCHED = offsetof(struct ARMJit, fetched),
	ENTRY_CODE = offsetof(struct ARMJitEntry, code),
	ENTRY_PC = offsetof(struct ARMJitEntry, pc),
	ENTRY_OP0 = offsetof(struct ARMJitEntry, op0),
	ENTRY_OP1 = offsetof(struct ARMJitEntry, op1),
	ENTRY_OP_MASK0 = offsetof(struct ARMJitEntry, opMask0),
	ENTRY_OP_MASK1 = offsetof(struct ARMJitEntry, opMask1),
	ENTRY_THUMB = offsetof(struct ARMJitEntry, thumb),
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
	const uint32_t* region;
	uint32_t mask;
	uint32_t ops[MAX_SPAN];
	// Patched instructions and prefetch words, read at runtime from jit->fetched
	bool hot[MAX_SPAN];
	unsigned count;
	bool thumb;
	unsigned width;
	// Cartridge code adds prefetch stalls to its data waits and leaves VRAM to the handlers
	bool romCode;
	uint32_t aluCycles;
	uint32_t memCycles;
	// For the prefetch stalls of data accesses from cartridge code
	int32_t seq16;
	int32_t nonseq16;
	bool prefetch;
	struct {
		uint8_t* at;
		int index;
	} exits[MAX_EXITS];
	unsigned nExits;
	struct ARMJitBlock* block;
	unsigned nLinks;
	// Event checks inside ALU runs go to the careful copy; loops go back through entry 0
	struct {
		uint8_t* site;
		unsigned index;
	} runs[MAX_BLOCK];
	unsigned nRuns;
	uint8_t* loops[MAX_BLOCK];
	unsigned nLoops;
	uint8_t* fast[MAX_SPAN];
	uint8_t* careful[MAX_SPAN];
	unsigned runLength[MAX_SPAN];
	bool isTarget[MAX_SPAN];
	// The region each register pointed into when compiled, or -1
	int regRegion[16];
	// Memory access paths that go after the block
	struct {
		unsigned index;
		struct MemOp mem;
		uint8_t* miss[2];
		uint8_t* smc;
		bool smcEwram;
		uint8_t* done;
		uint8_t* toEvent;
	} cold[2 * MAX_SPAN];
	unsigned nCold;
};

static void _exitAt(struct Compiler* c, uint8_t* at, int index) {
	c->exits[c->nExits].at = at;
	c->exits[c->nExits].index = index;
	++c->nExits;
}

static bool _decodeAlu(struct Compiler* c, unsigned i, struct AluOp* alu) {
	uint32_t address = c->pc + c->width * i;
	if (c->thumb) {
		return _decodeThumbAlu(c->ops[i], address, c->aluCycles, alu);
	}
	return _decodeArmAlu(c->ops[i], address, c->aluCycles, alu);
}

static bool _decodeMem(struct Compiler* c, unsigned i, struct MemOp* mem) {
	uint32_t address = c->pc + c->width * i;
	if (c->thumb) {
		return _decodeThumbMem(c->ops[i], address, mem);
	}
	return _decodeArmMem(c->ops[i], address, mem);
}

static uint32_t _regionEnd(struct ARMCore* cpu, uint32_t pc);

struct BranchOp {
	uint32_t target;
	unsigned cond;
	// ARM BL sets LR; Thumb BL checks the LR its first half set, then sets it
	bool link;
	bool thumbLink;
	uint32_t lr;
	// Target instruction in this block, or -1
	int index;
};

// B, BL and conditional branches whose target is known when compiling
static bool _branchTarget(struct Compiler* c, unsigned i, struct BranchOp* b) {
	uint32_t op = c->ops[i];
	uint32_t address = c->pc + c->width * i;
	if (c->hot[i]) {
		return false;
	}
	b->link = false;
	b->thumbLink = false;
	b->lr = 0;
	if (!c->thumb) {
		if ((op & 0x0E000000) != 0x0A000000) {
			return false;
		}
		b->link = op & 0x01000000;
		b->target = address + 8 + ((int32_t) (op << 8) >> 6);
	} else if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {
		b->target = address + 4 + ((int8_t) op << 1);
	} else if ((op & 0xF800) == 0xE000) {
		b->target = address + 4 + ((int32_t) (op << 21) >> 20);
	} else if ((op & 0xF800) == 0xF800 && i > 0 && !c->hot[i - 1] && (c->ops[i - 1] & 0xF800) == 0xF000) {
		// BL whose first half is the previous instruction, so LR is known
		b->thumbLink = true;
		b->lr = address + 2 + ((int32_t) (c->ops[i - 1] << 21) >> 9);
		b->target = b->lr + ((op & 0x7FF) << 1);
	} else {
		return false;
	}
	b->cond = _condition(op, c->thumb);
	return true;
}

// A branch to a known address in the same region, which needs no region change
static bool _decodeBranch(struct Compiler* c, unsigned i, struct BranchOp* b) {
	if (!_branchTarget(c, i, b) || b->cond == 0xF || (b->target >> 24) != (c->pc >> 24) || b->target + 2 * c->width > _regionEnd(c->cpu, c->pc)) {
		return false;
	}
	b->index = -1;
	if (b->target >= c->pc && b->target < c->pc + c->width * c->count) {
		b->index = (b->target - c->pc) / c->width;
	}
	return true;
}

// Sequential loads GBAMemoryStall fits in a wait when nothing was prefetched before
static unsigned _prefetchLoads(int32_t seq, int32_t wait) {
	int32_t stall = seq + 1;
	unsigned loads = 1;
	while (stall < wait && loads < 8) {
		stall += seq;
		++loads;
	}
	return loads;
}

// Byte tables by loads left from the last prefetch: lastPrefetchedPc in halfwords past the PC, and -min(wait, stall)
static void _stallTables(int32_t seq, int32_t wait, uint64_t* advances, uint64_t* stalls) {
	unsigned first = _prefetchLoads(seq, wait);
	*advances = 0;
	*stalls = 0;
	unsigned previous;
	for (previous = 0; previous < 8; ++previous) {
		unsigned loads = first < 8 - previous ? first : 8 - previous;
		int32_t stall = seq * loads + 1;
		*advances |= (uint64_t) (loads + previous - 1) << (8 * previous);
		*stalls |= (uint64_t) (uint8_t) -(stall < wait ? stall : wait) << (8 * previous);
	}
}

// PC-relative loads: cartridge words are constants but for the GPIO registers, RAM words load at runtime
enum {
	LITERAL_NONE,
	LITERAL_CONSTANT,
	LITERAL_RAM,
};

static void* _hostAddress(struct Compiler* c, uint32_t address);

static int _literal(struct Compiler* c, const struct MemOp* mem, uint32_t* value, void** host, int32_t* wait) {
	if (!mem->load || !mem->base.constant || !mem->immediateOffset || mem->runtimeOffset || mem->writeback || mem->size != 4) {
		return LITERAL_NONE;
	}
	uint32_t address = mem->up ? mem->base.value + mem->offset : mem->base.value - mem->offset;
	unsigned region = address >> 24;
	const struct GBAMemory* memory = &c->gba->memory;
	if (address & 3) {
		return LITERAL_NONE;
	}
	if (region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2_EX) {
		uint32_t offset = address & (GBA_SIZE_ROM0 - 4);
		if (offset + 4 > memory->romSize || (offset >= 0xC0 && offset < 0xD0)) {
			return LITERAL_NONE;
		}
		LOAD_32(*value, offset, memory->rom);
		*wait = memory->waitstatesNonseq32[region] + 2;
		return LITERAL_CONSTANT;
	}
	if (region == GBA_REGION_IWRAM || region == GBA_REGION_EWRAM) {
		*host = _hostAddress(c, address);
		*wait = (region == GBA_REGION_EWRAM ? memory->waitstatesNonseq32[GBA_REGION_EWRAM] : 0) + 2;
		return LITERAL_RAM;
	}
	return LITERAL_NONE;
}

// Timer counter reads, as GBALoad16 and GBAIORead make them
static uint32_t _readTimer(struct GBA* gba, uint32_t address) {
	gba->haltPending = false;
	GBATimerReadRegister(gba, (address >> 2) & 3);
	return gba->memory.io[GBA_REG(TM0CNT_LO) + ((address >> 1) & 6)];
}

static uint32_t _opAt(struct Compiler* c, uint32_t address) {
	uint32_t op;
	if (c->thumb) {
		LOAD_16(op, address & c->mask, c->region);
	} else {
		LOAD_32(op, address & c->mask, c->region);
	}
	return op;
}

static void* _hostAddress(struct Compiler* c, uint32_t address) {
	if ((address >> 24) == GBA_REGION_IWRAM) {
		return (uint8_t*) c->gba->memory.iwram + (address & (GBA_SIZE_IWRAM - 1));
	}
	return (uint8_t*) c->gba->memory.wram + (address & (GBA_SIZE_EWRAM - 1));
}

static int _valueRegion(uint32_t value) {
	return value >> 24 < 16 ? (int) (value >> 24) : -1;
}

static int _sourceRegion(struct Compiler* c, struct Source source) {
	return source.constant ? _valueRegion(source.value) : c->regRegion[source.reg];
}

static void _setRegion(struct Compiler* c, unsigned reg, int region) {
	if (reg < ARM_PC) {
		c->regRegion[reg] = region;
	}
}

static void _forgetRegions(struct Compiler* c) {
	unsigned reg;
	for (reg = 0; reg < 16; ++reg) {
		c->regRegion[reg] = -1;
	}
}

// Pointer arithmetic keeps a register's region; other results lose it
static void _trackAlu(struct Compiler* c, const struct AluOp* alu) {
	if (alu->opcode >= ALU_TST && alu->opcode <= ALU_CMN) {
		return;
	}
	int region = -1;
	if (alu->opcode == ALU_ADD || alu->opcode == ALU_SUB) {
		region = _sourceRegion(c, alu->n);
	} else if (alu->opcode == ALU_MOV && alu->immediate) {
		region = _valueRegion(alu->imm);
	} else if (alu->opcode == ALU_MOV && !alu->shiftType && !alu->shiftAmount) {
		region = _sourceRegion(c, alu->m);
	}
	_setRegion(c, alu->rd, region);
}

// What a literal pool word points into, if that has an inline path: known for cartridge literals, read now for RAM ones
static int _literalRegion(struct Compiler* c, const struct MemOp* mem) {
	uint32_t value;
	void* host;
	int32_t wait;
	switch (_literal(c, mem, &value, &host, &wait)) {
	case LITERAL_CONSTANT:
		break;
	case LITERAL_RAM:
		LOAD_32(value, 0, host);
		break;
	default:
		return -1;
	}
	unsigned region = value >> 24;
	if (region == GBA_REGION_IWRAM || region == GBA_REGION_EWRAM || region == GBA_REGION_VRAM || (region >= GBA_REGION_ROM0 && region <= GBA_REGION_ROM2)) {
		return region;
	}
	return -1;
}

static void _trackMulti(struct Compiler* c, const struct MultiOp* m) {
	unsigned reg;
	for (reg = 0; reg < 16 && m->load; ++reg) {
		if (m->list & (1 << reg)) {
			_setRegion(c, reg, -1);
		}
	}
}

static int32_t _ramWait(struct Compiler* c, const struct MemOp* mem, bool ewram) {
	if (!ewram) {
		return 0;
	}
	return (mem->size == 4 ? c->gba->memory.waitstatesNonseq32 : c->gba->memory.waitstatesNonseq16)[GBA_REGION_EWRAM];
}

enum {
	PATH_NONE,
	PATH_IWRAM,
	PATH_EWRAM,
	PATH_VRAM,
	PATH_CART,
};

// Where the code's own N and S fetch waits live, which a store to WAITCNT changes for the rest of its instruction
static void _fetchWaits(const struct Compiler* c, unsigned* nonseq, unsigned* seq) {
	*nonseq = OFF_MEMORY + (c->thumb ? offsetof(struct ARMMemory, activeNonseqCycles16) : offsetof(struct ARMMemory, activeNonseqCycles32));
	*seq = OFF_MEMORY + (c->thumb ? offsetof(struct ARMMemory, activeSeqCycles16) : offsetof(struct ARMMemory, activeSeqCycles32));
}

// An access checks inline only the region its base register pointed into when compiled
static int _memPath(struct Compiler* c, const struct MemOp* mem) {
	switch (_sourceRegion(c, mem->base)) {
	case -1:
	case GBA_REGION_IWRAM:
		return PATH_IWRAM;
	case GBA_REGION_EWRAM:
		return PATH_EWRAM;
	case GBA_REGION_VRAM:
		return !c->romCode && (mem->load || mem->size > 1) ? PATH_VRAM : PATH_NONE;
	case GBA_REGION_ROM0:
	case GBA_REGION_ROM0_EX:
	case GBA_REGION_ROM1:
	case GBA_REGION_ROM1_EX:
	case GBA_REGION_ROM2:
		return mem->load ? PATH_CART : PATH_NONE;
	default:
		return PATH_NONE;
	}
}

#if defined(__aarch64__)
#include "jit-a64.h"
#else
#include "jit-x64.h"
#endif

// A patched load or store whose patches only change the offset runs inline
#define PATCHED_SHAPE 0xFF7FF000

static bool _patchedMem(struct Compiler* c, unsigned i, uint32_t op, struct MemOp* mem) {
	uint32_t address = c->pc + c->width * i;
	if (c->thumb || (op & 0x0C000000) != 0x04000000 || !_decodeArmMem(op, address, mem) || !mem->immediateOffset) {
		return false;
	}
	mem->runtimeOffset = true;
	mem->fetchedIndex = i;
	return true;
}

// Patched ARM instructions run their last two forms inline; anything else goes through the handler
static void _emitPatched(struct Compiler* c, unsigned i) {
	uint32_t forms[2] = { c->ops[i], 0 };
	unsigned nForms = 1;
	int word = ARMJitRamWord(c->pc + c->width * i);
	if (!c->thumb && word >= 0 && c->jit->patchOther[word] && c->jit->patchOther[word] != forms[0]) {
		forms[nForms++] = c->jit->patchOther[word];
	}
	uint8_t* done[2];
	unsigned nDone = 0;
	unsigned k;
	for (k = 0; k < nForms; ++k) {
		struct MemOp mem;
		struct AluOp alu;
		uint8_t* other;
		if (_patchedMem(c, i, forms[k], &mem)) {
			if (k && !((forms[0] ^ forms[k]) & PATCHED_SHAPE)) {
				continue;
			}
			other = _fetchedMismatch(c, i, PATCHED_SHAPE, forms[k] & PATCHED_SHAPE);
			_emitMem(c, i, &mem);
			if (mem.load) {
				_setRegion(c, mem.rd, -1);
			}
		} else if (!c->thumb && _decodeArmAlu(forms[k], c->pc + c->width * i, c->aluCycles, &alu)) {
			other = _fetchedMismatch(c, i, 0xFFFFFFFF, forms[k]);
			alu.flagsLive = true;
			_emitAlu(c, &alu, alu.keepsShifterCarry);
			_setRegion(c, alu.rd, -1);
			_addCycles(c, alu.cycles);
			_eventCheck(c, i + 1);
		} else {
			continue;
		}
		done[nDone++] = _forwardJump(c);
		_patch(other, c->e.p);
	}
	_emitDynamicHandler(c, i);
	for (k = 0; k < nDone; ++k) {
		_patch(done[k], c->e.p);
	}
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

static void _unlink(struct ARMJitBlock* block) {
	struct ARMJitLink* link = block->incoming;
	while (link) {
		struct ARMJitLink* next = link->next;
		_patchLink(link->site, link->stub);
		link->target = NULL;
		link = next;
	}
	block->incoming = NULL;
	unsigned i;
	for (i = 0; i < block->nLinks; ++i) {
		link = &block->links[i];
		if (link->target) {
			*link->prev = link->next;
			if (link->next) {
				link->next->prev = link->prev;
			}
			link->target = NULL;
		}
	}
}

void ARMJitLink(struct ARMJit* jit, struct ARMJitLink* link, struct ARMJitEntry* entry) {
	UNUSED(jit);
	if (link->target) {
		return;
	}
	struct ARMJitBlock* block = entry->block;
	link->target = block;
	link->next = block->incoming;
	link->prev = &block->incoming;
	if (block->incoming) {
		block->incoming->prev = &link->next;
	}
	block->incoming = link;
	_patchLink(link->site, entry->code);
}

static void _freeBlock(struct ARMJitBlock* block) {
	free(block->entries);
	free(block->links);
	free(block);
}

static void _removeBlock(struct ARMJit* jit, struct ARMJitBlock* block) {
	unsigned i;
	for (i = 0; i < block->nEntries; ++i) {
		struct ARMJitEntry* entry = &block->entries[i];
		struct ARMJitPage* page = _page(jit, entry->pc, false);
		if (page && page->entries[(entry->pc & 0xFFF) >> 1] == entry) {
			page->entries[(entry->pc & 0xFFF) >> 1] = NULL;
		}
	}
	*block->prev = block->next;
	if (block->next) {
		block->next->prev = block->prev;
	}
	_unlink(block);
	for (i = 0; i < block->coverWords; ++i) {
		if (block->coverMask[i / 32] & (1u << (i & 31))) {
			--jit->cover[block->coverStart + i];
		}
	}
	jit->smcHit = 1;
	_freeBlock(block);
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

// EWRAM waitstate changes and a full code buffer drop every block but keep the patch history
void ARMJitDropBlocks(struct ARMJit* jit) {
	struct ARMJitBlock* block = jit->blockList;
	while (block) {
		struct ARMJitBlock* next = block->next;
		_freeBlock(block);
		block = next;
	}
	jit->smcHit = 1;
	jit->blockList = NULL;
	unsigned i;
	for (i = 0; i < ARM_JIT_PAGES; ++i) {
		if (jit->pages[i]) {
			memset(jit->pages[i]->entries, 0, sizeof(jit->pages[i]->entries));
		}
	}
	memset(jit->cover, 0, sizeof(jit->cover));
	jit->codeUsed = jit->codeStart;
}

// Blocks whose PC is in [start, end), for waitstate changes that alter their fetch timing
void ARMJitDropRegion(struct ARMJit* jit, uint32_t start, uint32_t end) {
	struct ARMJitBlock* block = jit->blockList;
	while (block) {
		struct ARMJitBlock* next = block->next;
		if (block->pc >= start && block->pc < end) {
			_removeBlock(jit, block);
		}
		block = next;
	}
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
	memset(jit->patchValue, 0, sizeof(jit->patchValue));
	memset(jit->patchOther, 0, sizeof(jit->patchOther));
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
	uint32_t value = word < ARM_JIT_IWRAM_WORDS ? jit->iwram[word] : jit->ewram[word - ARM_JIT_IWRAM_WORDS];
	if (value != jit->patchValue[word]) {
		jit->patchOther[word] = jit->patchValue[word];
		jit->patchValue[word] = value;
	}
	// A block can start up to MAX_SPAN ARM words before the word it covers
	uint32_t address = _ramWordAddress(word);
	unsigned back;
	for (back = 0; back < 4 * MAX_SPAN + 4 && jit->cover[word]; back += 2) {
		uint32_t start = address + 2 - back;
		struct ARMJitPage* page = _page(jit, start, false);
		struct ARMJitEntry* entry = page ? page->entries[(start & 0xFFF) >> 1] : NULL;
		if (entry && entry->pc == start && _covers(entry->block, word)) {
			_removeBlock(jit, entry->block);
		}
	}
}

// Words that keep getting patched are read when fetched instead of compiled in
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

// The words a block at pc runs through, up to the first that ends it, and the two after
static bool _readBlock(struct Compiler* c, struct ARMJit* jit, struct ARMCore* cpu, uint32_t pc, bool thumb) {
	c->region = cpu->memory.activeRegion;
	c->mask = cpu->memory.activeMask;
	c->thumb = thumb;
	c->width = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
	c->pc = pc;
	uint32_t end = _regionEnd(cpu, pc);
	unsigned count = 0;
	while (count < MAX_BLOCK && pc + c->width * (count + 3) <= end) {
		++count;
		if (!_isPatched(jit, pc + c->width * (count - 1)) && _endsBlock(_opAt(c, pc + c->width * (count - 1)), thumb)) {
			break;
		}
	}
	c->count = count;
	unsigned i;
	for (i = 0; i < count + 2; ++i) {
		c->hot[i] = _isPatched(jit, pc + c->width * i);
		c->ops[i] = _opAt(c, pc + c->width * i);
	}
	return count && !((c->ops[0] ^ cpu->prefetch[0]) & (c->hot[0] ? 0 : 0xFFFFFFFF)) &&
	       !((c->ops[1] ^ cpu->prefetch[1]) & (c->hot[1] ? 0 : 0xFFFFFFFF));
}

static struct ARMJitBlock* _newBlock(unsigned count) {
	struct ARMJitBlock* block = calloc(1, sizeof(*block));
	if (!block) {
		return NULL;
	}
	// Two linkable exits per branch at most (inline and through the handler), plus falling off the end
	block->links = calloc(2 * count + 1, sizeof(*block->links));
	block->entries = calloc(count + 1, sizeof(*block->entries));
	if (!block->links || !block->entries) {
		_freeBlock(block);
		return NULL;
	}
	return block;
}

static void _startCompiler(struct Compiler* c, struct ARMJit* jit, struct ARMCore* cpu, struct ARMJitBlock* block) {
	c->e.p = &jit->code[jit->codeUsed];
	c->jit = jit;
	c->cpu = cpu;
	c->gba = (struct GBA*) cpu->master;
	c->block = block;
	c->romCode = (c->pc >> 24) >= GBA_REGION_ROM0;
	c->nExits = 0;
	c->nLinks = 0;
	c->nRuns = 0;
	c->nLoops = 0;
	c->nCold = 0;
	c->seq16 = cpu->memory.activeSeqCycles16;
	c->nonseq16 = cpu->memory.activeNonseqCycles16;
	c->prefetch = c->gba->memory.prefetch;
	c->aluCycles = 1 + (c->thumb ? cpu->memory.activeSeqCycles16 : cpu->memory.activeSeqCycles32);
	c->memCycles = 1 + (c->thumb ? cpu->memory.activeNonseqCycles16 : cpu->memory.activeNonseqCycles32);
	memset(c->fast, 0, sizeof(c->fast));
	memset(c->careful, 0, sizeof(c->careful));
	memset(c->runLength, 0, sizeof(c->runLength));
	memset(c->isTarget, 0, sizeof(c->isTarget));
	unsigned i;
	for (i = 0; i < 16; ++i) {
		c->regRegion[i] = _valueRegion(cpu->gprs[i]);
	}
	// Branches back into the block jump straight to their target, so it starts a unit
	struct BranchOp branch;
	for (i = 0; i < c->count; ++i) {
		if (_decodeBranch(c, i, &branch) && branch.index >= 0) {
			c->isTarget[branch.index] = true;
		}
	}
}

// A run of ALU instructions goes at once only when no event can come due before its last one
static unsigned _emitAluRun(struct Compiler* c, unsigned i, struct AluOp* alus) {
	unsigned run = 1;
	while (i + run < c->count && !c->hot[i + run] && !c->isTarget[i + run] && _decodeAlu(c, i + run, &alus[i + run])) {
		++run;
	}
	uint32_t cycles = 0;
	unsigned lastAlways = 0;
	unsigned j;
	for (j = 0; j < run; ++j) {
		if (j < run - 1) {
			cycles += alus[i + j].cycles;
		}
		if (alus[i + j].cond == 0xE) {
			lastAlways = j;
		}
	}
	if (run > 1) {
		c->runs[c->nRuns].site = _runCheck(c, cycles);
		c->runs[c->nRuns].index = i;
		++c->nRuns;
		c->runLength[i] = run;
	}
	_markLiveFlags(&alus[i], run);
	for (j = 0; j < run; ++j) {
		_fetchAhead(c, i + j);
		_emitAlu(c, &alus[i + j], alus[i + j].keepsShifterCarry && j >= lastAlways);
		_trackAlu(c, &alus[i + j]);
	}
	_addCycles(c, cycles + alus[i + run - 1].cycles);
	_eventCheck(c, i + run);
	return run;
}

// One unit per ALU run or other instruction, each entered at fast[i]
static void _emitBody(struct Compiler* c) {
	struct AluOp alus[MAX_SPAN];
	struct MemOp mem;
	struct MultiOp multi;
	struct MulOp mul;
	struct BranchOp branch;
	unsigned i = 0;
	while (i < c->count) {
		c->fast[i] = c->e.p;
		uint32_t op = c->ops[i];
		if (c->hot[i]) {
			_fetchAhead(c, i);
			_emitPatched(c, i);
		} else if (_decodeAlu(c, i, &alus[i])) {
			i += _emitAluRun(c, i, alus);
			continue;
		} else if (_decodeMem(c, i, &mem)) {
			_fetchAhead(c, i);
			_emitMem(c, i, &mem);
			if (mem.load) {
				_setRegion(c, mem.rd, _literalRegion(c, &mem));
			}
		} else if (_decodeMul(op, c->thumb, &mul)) {
			_fetchAhead(c, i);
			_emitMul(c, i, &mul);
			_setRegion(c, mul.rd, -1);
		} else if (_decodeMulti(op, c->thumb, &multi)) {
			_fetchAhead(c, i);
			_emitMulti(c, i, &multi);
			_trackMulti(c, &multi);
		} else if (_decodeBranch(c, i, &branch)) {
			_fetchAhead(c, i);
			_emitBranch(c, i, &branch);
			if (branch.link || branch.thumbLink) {
				_setRegion(c, ARM_LR, -1);
			}
		} else {
			_fetchAhead(c, i);
			_emitFallback(c, i);
			_forgetRegions(c);
		}
		++i;
	}
	c->fast[c->count] = c->e.p;
	_storeState(c, c->count);
	_linkJump(c);
}

// Careful copies of ALU runs: each instruction through its handler, checking for events
static void _emitCarefulCopies(struct Compiler* c) {
	unsigned i;
	for (i = 0; i < c->count; ++i) {
		if (!c->runLength[i]) {
			continue;
		}
		unsigned j;
		for (j = i; j < i + c->runLength[i]; ++j) {
			c->careful[j] = c->e.p;
			_fetchAhead(c, j);
			_emitFallback(c, j);
		}
		_jumpTo(c, c->fast[j]);
	}
	for (i = 0; i < c->nRuns; ++i) {
		_patch(c->runs[i].site, c->careful[c->runs[i].index]);
	}
}

static uint8_t* _entryCode(struct Compiler* c, unsigned i) {
	return c->fast[i] ? c->fast[i] : c->careful[i];
}

// Exit stubs store the state for the next instruction, then dispatch or run the due events
static void _emitExits(struct Compiler* c) {
	uint8_t* stubs[2][MAX_SPAN];
	memset(stubs, 0, sizeof(stubs));
	unsigned x;
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		bool due = index & EXIT_DUE;
		unsigned at = index & ~EXIT_DUE;
		if (index >= 0 && !stubs[due][at]) {
			stubs[due][at] = c->e.p;
			_storeState(c, at);
			if (due) {
				_resumeAfterEvents(c, _entryCode(c, at));
			} else {
				_exitJump(c, EXIT_DIRECT);
			}
		}
	}
	uint8_t* direct = c->e.p;
	_jumpTo(c, c->jit->dispatch);
	uint8_t* events = c->e.p;
	_jumpTo(c, c->jit->events);
	for (x = 0; x < c->nExits; ++x) {
		int index = c->exits[x].index;
		uint8_t* target = index == EXIT_DIRECT ? direct : index == EXIT_EVENTS ? events : stubs[(index & EXIT_DUE) != 0][index & ~EXIT_DUE];
		_patch(c->exits[x].at, target);
	}
	struct ARMJitBlock* block = c->block;
	for (x = 0; x < c->nLinks; ++x) {
		struct ARMJitLink* link = &block->links[x];
		link->stub = c->e.p;
		_linkStub(c, link);
		_patch(link->site, link->stub);
	}
	block->nLinks = c->nLinks;
}

// Entry stubs take patched first words from the pipeline
static void _emitEntries(struct Compiler* c) {
	struct ARMJitBlock* block = c->block;
	block->nEntries = 0;
	unsigned i;
	for (i = 0; i < c->count; ++i) {
		uint8_t* target = _entryCode(c, i);
		if (!target) {
			continue;
		}
		struct ARMJitEntry* entry = &block->entries[block->nEntries++];
		entry->code = target;
		entry->pc = c->pc + c->width * i;
		entry->op0 = c->ops[i];
		entry->op1 = c->ops[i + 1];
		entry->opMask0 = c->hot[i] ? 0 : 0xFFFFFFFF;
		entry->opMask1 = c->hot[i + 1] ? 0 : 0xFFFFFFFF;
		entry->thumb = c->thumb;
		entry->block = block;
		if (c->hot[i] || c->hot[i + 1]) {
			entry->code = c->e.p;
			_entryStub(c, i, target);
		}
	}
	for (i = 0; i < c->nLoops; ++i) {
		_patch(c->loops[i], block->entries[0].code);
	}
}

// The words the block covers, so writing them removes it, and the boundaries it can be entered at
static void _registerBlock(struct Compiler* c) {
	struct ARMJit* jit = c->jit;
	struct ARMJitBlock* block = c->block;
	block->pc = c->pc;
	block->coverStart = ARMJitRamWord(c->pc);
	block->coverWords = 0;
	memset(block->coverMask, 0, sizeof(block->coverMask));
	unsigned i;
	if (block->coverStart >= 0) {
		block->coverWords = ARMJitRamWord(c->pc + c->width * (c->count + 2) - 1) - block->coverStart + 1;
		for (i = 0; i < block->coverWords; ++i) {
			if (jit->patched[block->coverStart + i] < PATCH_LIMIT) {
				block->coverMask[i / 32] |= 1u << (i & 31);
				++jit->cover[block->coverStart + i];
			}
		}
	}
	// The first block to register an address keeps its entry
	for (i = 0; i < block->nEntries; ++i) {
		struct ARMJitEntry* entry = &block->entries[i];
		struct ARMJitPage* page = _page(jit, entry->pc, true);
		if (!page->entries[(entry->pc & 0xFFF) >> 1]) {
			page->entries[(entry->pc & 0xFFF) >> 1] = entry;
		}
	}
	block->next = jit->blockList;
	block->prev = &jit->blockList;
	if (jit->blockList) {
		jit->blockList->prev = &block->next;
	}
	jit->blockList = block;
}

static struct ARMJitBlock* _compile(struct ARMJit* jit, struct ARMCore* cpu, uint32_t pc, bool thumb) {
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	if (!_readBlock(c, jit, cpu, pc, thumb)) {
		return NULL;
	}
	if (jit->codeUsed + c->count * MAX_INSN_BYTES + 1024 > jit->codeSize) {
		ARMJitDropBlocks(jit);
	}
	struct ARMJitBlock* block = _newBlock(c->count);
	if (!block) {
		return NULL;
	}
	_startCompiler(c, jit, cpu, block);
	uint8_t* code = c->e.p;
	_emitBody(c);
	_emitColdPaths(c);
	_emitCarefulCopies(c);
	_emitExits(c);
	_emitEntries(c);
	__builtin___clear_cache((char*) code, (char*) c->e.p);
	jit->codeUsed += c->e.p - code;
	_registerBlock(c);
	return block;
}

// Debug: compare inline ALU code with the interpreter on random inputs
static unsigned _selfTestAluMode(struct ARMJit* jit, unsigned iterations, bool thumb) {
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint32_t seed = thumb ? 54321 : 12345;
	unsigned failures = 0;
	unsigned n;
	for (n = 0; n < iterations && failures < 20; ++n) {
		uint32_t address = 0x03001000;
		uint32_t op;
		struct AluOp alu;
		do {
			seed = seed * 1103515245 + 12345;
			op = seed;
			seed = seed * 1103515245 + 12345;
			op ^= seed << 16;
			if (thumb) {
				op &= 0xFFFF;
			}
		} while (thumb ? !_decodeThumbAlu(op, address, 1, &alu) : !_decodeArmAlu(op, address, 1, &alu));
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
		a.gprs[ARM_PC] = address + (thumb ? 4 : 8);
		seed = seed * 1103515245 + 12345;
		a.cpsr.packed = (seed & 0xFF000000) | (thumb ? 0x3F : 0x1F);
		struct ARMCore b = a;

		if (thumb) {
			_thumbTable[op >> 6](&a, op);
		} else if (_conditionLut[op >> 28] & (1 << ((uint32_t) a.cpsr.packed >> 28))) {
			_armTable[((op >> 16) & 0xFF0) | ((op >> 4) & 0x00F)](&a, op);
		} else {
			a.cycles += 1;
		}

		uint8_t* code = &jit->code[jit->codeUsed];
		c->e.p = code;
		c->jit = jit;
		c->gba = NULL;
		c->pc = address;
		memset(c->ops, 0, sizeof(c->ops));
		memset(c->hot, 0, sizeof(c->hot));
		c->ops[0] = op;
		c->count = 1;
		c->thumb = thumb;
		c->width = thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM;
		c->romCode = false;
		c->nExits = 0;
		c->aluCycles = 1;
		c->memCycles = 1;
		alu.flagsLive = true;
		_prologue(c);
		_emitAlu(c, &alu, alu.keepsShifterCarry);
		_addCycles(c, alu.cycles);
		_epilogue(c);
		__builtin___clear_cache((char*) code, (char*) c->e.p);
		((void (*)(struct ARMCore*)) code)(&b);

		bool bad = a.cpsr.packed != b.cpsr.packed || a.cycles != b.cycles || (a.shifterCarryOut & 1) != (b.shifterCarryOut & 1);
		for (r = 0; r < 16; ++r) {
			bad = bad || a.gprs[r] != b.gprs[r];
		}
		if (bad) {
			++failures;
			fprintf(stderr, "%s ALU mismatch op %08X: cpsr %08X/%08X cycles %d/%d", thumb ? "Thumb" : "ARM", op, a.cpsr.packed, b.cpsr.packed, a.cycles, b.cycles);
			for (r = 0; r < 16; ++r) {
				if (a.gprs[r] != b.gprs[r]) {
					fprintf(stderr, " r%u %08X/%08X", r, a.gprs[r], b.gprs[r]);
				}
			}
			fprintf(stderr, "\n");
		}
	}
	return failures;
}

static void _selfTestAlu(struct ARMJit* jit, unsigned iterations) {
	unsigned arm = _selfTestAluMode(jit, iterations, false);
	unsigned thumb = _selfTestAluMode(jit, iterations, true);
	fprintf(stderr, "ALU self-test: %u ARM and %u Thumb instructions, %u and %u failures\n", iterations, iterations, arm, thumb);
}

static void _buildTrampoline(struct ARMJit* jit, struct ARMCore* cpu) {
	static struct Compiler compiler;
	struct Compiler* c = &compiler;
	uint8_t* code = jit->code;
	c->e.p = code;
	c->jit = jit;
	c->cpu = cpu;
	c->gba = (struct GBA*) cpu->master;
	jit->iwram = c->gba->memory.iwram;
	jit->ewram = c->gba->memory.wram;
	_emitTrampoline(c);
	__builtin___clear_cache((char*) code, (char*) c->e.p);
	jit->codeStart = c->e.p - code;
	jit->codeUsed = jit->codeStart;
}

// Runs due events for generated code, and says whether the frame loop would run on
bool ARMJitEvents(struct ARMCore* cpu) {
	struct ARMJit* jit = cpu->jit;
	struct GBA* gba = (struct GBA*) cpu->master;
	cpu->irqh.processEvents(cpu);
	jit->eventsRan = true;
	jit->vramNotified = 0;
	return jit->inFrame && gba->video.frameCounter == jit->frameCounter &&
	    mTimingCurrentTime(&gba->timing) - jit->frameStart < VIDEO_TOTAL_LENGTH + VIDEO_HORIZONTAL_LENGTH;
}

enum ARMJitResult ARMJitRun(struct ARMCore* cpu) {
	struct ARMJit* jit = cpu->jit;
	if (!jit->enter) {
		_buildTrampoline(jit, cpu);
	}
	bool thumb = cpu->executionMode == MODE_THUMB;
	uint32_t pc = cpu->gprs[ARM_PC] - (thumb ? WORD_SIZE_THUMB : WORD_SIZE_ARM);
	struct ARMJitPage* page = _page(jit, pc, false);
	unsigned index = (pc & 0xFFF) >> 1;
	struct ARMJitEntry* entry = page ? page->entries[index] : NULL;
	if (!entry || entry->pc != pc || entry->thumb != thumb) {
		if (!_regionEnd(cpu, pc)) {
			return ARM_JIT_STEP;
		}
		if (!page) {
			page = _page(jit, pc, true);
		}
		if (page->hits[index] < HOT_THRESHOLD) {
			++page->hits[index];
			return ARM_JIT_STEP;
		}
		if (entry) {
			_removeBlock(jit, entry->block);
		}
		struct ARMJitBlock* block = _compile(jit, cpu, pc, thumb);
		if (!block) {
			page->hits[index] = 0;
			return ARM_JIT_STEP;
		}
		entry = &block->entries[0];
	}
	if (((cpu->prefetch[0] ^ entry->op0) & entry->opMask0) || ((cpu->prefetch[1] ^ entry->op1) & entry->opMask1)) {
		return ARM_JIT_STEP;
	}
	jit->pendingLink = NULL;
	jit->smcHit = 0;
	jit->eventsRan = false;
	jit->vramNotified = 0;
	int32_t cycles = cpu->cycles;
	jit->enter(cpu, entry->code);
	if (jit->eventsRan) {
		return ARM_JIT_EVENTS;
	}
	// A block that ran nothing leaves the instruction to the interpreter
	return cpu->cycles != cycles ? ARM_JIT_RAN : ARM_JIT_STEP;
}

#endif
