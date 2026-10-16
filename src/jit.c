/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "jit-internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

// The recompiler's machine-independent side: blocks and how they are found and linked,
// self-modifying code, the run loop that switches between compiled code and the exact CPU, idle
// loops, and the helpers compiled code calls. The backends in a64/ and x64/ emit the code.

enum {
	PAGE_SHIFT = 8,
};

static const uint32_t codeKindSpan[CODE_KINDS] = {
	[CODE_EWRAM] = SIZE_EWRAM,
	[CODE_IWRAM] = SIZE_IWRAM,
	[CODE_PALETTE] = SIZE_PALETTE,
	[CODE_VRAM] = 0x20000,
	[CODE_OAM] = SIZE_OAM,
};

// Which memory an address's code lives in. Blocks are indexed by this kind and by their offset
// within it, so all mirrors of a memory map to one place.
int codeKindOf(struct GBA* gba, uint32_t address) {
	switch (address >> 24) {
	case REGION_BIOS:
		return address < SIZE_BIOS ? CODE_BIOS : CODE_NONE;
	case REGION_EWRAM:
		return CODE_EWRAM;
	case REGION_IWRAM:
		return CODE_IWRAM;
	case REGION_PALETTE:
		return CODE_PALETTE;
	case REGION_VRAM:
		return CODE_VRAM;
	case REGION_OAM:
		return CODE_OAM;
	case REGION_ROM0 ... REGION_ROM2_EX:
		return (address & (SIZE_ROM - 1)) < gba->romSize ? CODE_ROM : CODE_NONE;
	default:
		return CODE_NONE;
	}
}

// Returns an address's offset within the memory backing its region, folding mirrors together.
static uint32_t canonical(int kind, uint32_t address) {
	switch (kind) {
	case CODE_EWRAM:
		return address & (SIZE_EWRAM - 1);
	case CODE_IWRAM:
		return address & (SIZE_IWRAM - 1);
	case CODE_PALETTE:
		return address & (SIZE_PALETTE - 1);
	case CODE_VRAM:
		return (address & 0x1FFFF) >= SIZE_VRAM ? address & 0x17FFF : address & 0x1FFFF;
	case CODE_OAM:
		return address & (SIZE_OAM - 1);
	default:
		return address;
	}
}

// The part of the cartridge a mapper's window covers: the Matrix mapper's sixteen 512-byte pages.
enum {
	ROM_WINDOW = 0x2000,
};

// Returns what compiled cartridge code's costs depend on.
static uint32_t currentWaits(struct GBA* gba) {
	return gba->io[IO(REG_WAITCNT)] | (gba->prefetchDisabled ? 0x10000 : 0);
}

// Makes compiled code leave at its next check; every instruction that calls out is followed by one.
static void yield(struct GBA* gba) {
	gba->cpu.nextEvent = gba->cpu.cycles;
}

// The cartridge's six regions.
static bool inRom(uint32_t address) {
	return (address >> 24) >= REGION_ROM0 && (address >> 24) <= REGION_ROM2_EX;
}

// The cartridge starts a new burst every 128 KiB, so fetching an address there is nonsequential
// whatever came before. The instruction two back makes that fetch. When compiled for the prefetch
// buffer, its cost is the buffer's; without the buffer, only the exact CPU counts it. The
// instruction before that one also goes to the exact CPU: if compiled, a data access in it would
// count that fetch's nonsequential part early, and the exact CPU would count it again.
static bool burstEdge(struct GBA* gba, uint32_t pc, bool thumb) {
	int width = thumb ? 2 : 4;
	return inRom(pc) && !gba->prefetchEnabled && (!((pc + 2 * width) & 0x1FFFF) || !((pc + 3 * width) & 0x1FFFF));
}

// Blocks are found by key: the address, with the instruction set in its low bit.
static uint32_t hashBlock(uint32_t key) {
	return ((key ^ (key >> 16)) * 0x9E3779B1u >> 16) & (BLOCK_HASH_SIZE - 1);
}

// Returns the block starting at a key, or NULL.
static struct Block* findBlock(struct Jit* jit, uint32_t key) {
	struct Block* block = jit->hash[hashBlock(key)];
	for (; block; block = block->hashNext) {
		if (blockKey(block->pc, block->thumb) == key) {
			return block;
		}
	}
	return NULL;
}

// A map with one byte per word of work RAM, saying how compiled code depends on that word.
static uint8_t* codeMapFor(struct GBA* gba, int kind) {
	switch (kind) {
	case CODE_EWRAM:
		return gba->ewramCode;
	case CODE_IWRAM:
		return gba->iwramCode;
	default:
		return NULL;
	}
}

enum {
	PATCH_THRESHOLD = 2,
	SPLIT_THRESHOLD = 2,
	// Shape changes up to this many frames apart still count together. Payback's texture mapper
	// switches loads to moves and back every few seconds while a surface is on screen; counted per
	// 60 frames, each change threw away every block over the word, some twenty a frame.
	REWRITE_FRAMES = 1200,
	// How many forms a rewritten word can go through before it is compiled as it stands.
	FORM_DROP_LIMIT = 8,
};

// Returns a work RAM word's index in the tables that count its rewrites: internal RAM first, then
// external.
static int smcIndex(int kind, uint32_t offset) {
	return kind == CODE_IWRAM ? (offset >> 2) : (SIZE_IWRAM >> 2) + (offset >> 2);
}

// How a block depends on a word it covers: its bits are compiled in, it's a patchable load or
// store, it's checked when it runs, or it's one of the two words past the end that are only
// prefetched.
static void markWords(uint8_t* map, struct Block* block, uint32_t from, uint32_t to) {
	for (uint32_t offset = from; offset < to; offset += 4) {
		if (offset >= block->bodyStop) {
			if (!map[offset >> 2]) {
				map[offset >> 2] = CODE_MAP_TAIL;
			}
			continue;
		}
		uint32_t index = (offset - (block->start & ~3u)) >> 2;
		bool patchable = !block->thumb && index < 64 && (block->patchable >> index) & 1;
		bool polymorphic = !block->thumb && index < 64 && (block->polymorphic >> index) & 1;
		if (polymorphic) {
			if (!map[offset >> 2] || map[offset >> 2] == CODE_MAP_TAIL) {
				map[offset >> 2] = CODE_MAP_CHECKED;
			}
		} else if (patchable) {
			if (map[offset >> 2] != CODE_MAP_BAKED) {
				map[offset >> 2] = CODE_MAP_PATCHABLE;
			}
		} else {
			map[offset >> 2] = CODE_MAP_BAKED;
		}
	}
}

// Blocks are also listed by the guest memory pages they cover, so a write can find the blocks it
// lands in without looking at all of them.
static void addPages(struct Jit* jit, struct Block* block) {
	int kind = block->codeKind;
	if (!jit->pages[kind]) {
		return;
	}
	for (uint32_t page = block->start >> PAGE_SHIFT; page <= (block->stop - 1) >> PAGE_SHIFT; ++page) {
		struct PageNode* node = malloc(sizeof(*node));
		node->block = block;
		node->next = jit->pages[kind][page];
		jit->pages[kind][page] = node;
	}
	uint8_t* map = codeMapFor(jit->gba, kind);
	if (map) {
		markWords(map, block, block->start & ~3u, block->stop);
	}
	if (kind == CODE_PALETTE || kind == CODE_VRAM || kind == CODE_OAM) {
		jit->gba->videoCode = true;
	}
}

// Points jumps into a block that is going away back at their stubs, which look the target up.
static void unlinkIncoming(struct Block* block) {
	for (struct Link* link = block->incoming; link; link = link->nextIncoming) {
		backendPatchJump(link->site, link->stub);
		link->to = NULL;
	}
	block->incoming = NULL;
}

// Also takes a block that is going away off the lists of the blocks it jumps to.
static void removeOutgoing(struct Block* block) {
	for (int i = 0; i < block->outCount; ++i) {
		struct Link* link = &block->out[i];
		if (!link->to) {
			continue;
		}
		// Points the exit back at its stub, so a revived block links afresh.
		backendPatchJump(link->site, link->stub);
		struct Link** prev = &link->to->incoming;
		while (*prev && *prev != link) {
			prev = &(*prev)->nextIncoming;
		}
		if (*prev) {
			*prev = link->nextIncoming;
		}
		link->to = NULL;
	}
}

// Makes a block unreachable: no jump leads to it and no lookup finds it. Its code stays in place
// until the next flush, since the CPU may be inside it.
static void removeBlock(struct Jit* jit, struct Block* block) {
	if (!block->valid) {
		return;
	}
	block->valid = false;
	unlinkIncoming(block);
	removeOutgoing(block);
	if (block->twin) {
		removeBlock(jit, block->twin);
		// A block that is set aside keeps its careful copy, which comes back with it.
		if (!block->setAside) {
			block->twin = NULL;
		}
	}
	uint32_t key = blockKey(block->pc, block->thumb);
	if (block->cached) {
		struct Block** prev = &jit->hash[hashBlock(key)];
		while (*prev && *prev != block) {
			prev = &(*prev)->hashNext;
		}
		if (*prev) {
			*prev = block->hashNext;
		}
		struct LookupEntry* entry = &jit->lookup[lookupHash(key)];
		if (entry->key == key && entry->entry == block->entry) {
			entry->key = ~0u;
		}
		if (block->bytes || block->setAside) {
			uint32_t h = hashBlock(key);
			block->dormantNext = jit->dormant[h];
			jit->dormant[h] = block;
		}
	}
	// Code memory is only reclaimed by a flush, so a block can finish running after it has been
	// removed.
}

// Frees every block and list, for a flush or at shutdown.
static void freeAll(struct Jit* jit) {
	for (int kind = 0; kind < CODE_KINDS; ++kind) {
		if (!jit->pages[kind]) {
			continue;
		}
		for (uint32_t page = 0; page < jit->pageCount[kind]; ++page) {
			struct PageNode* node = jit->pages[kind][page];
			while (node) {
				struct PageNode* next = node->next;
				free(node);
				node = next;
			}
			jit->pages[kind][page] = NULL;
		}
	}
	struct Block* block = jit->allBlocks;
	while (block) {
		struct Block* next = block->allNext;
		free(block->sites);
		free(block->bytes);
		free(block->out);
		free(block->entries);
		free(block);
		block = next;
	}
	jit->allBlocks = NULL;
	memset(jit->hash, 0, sizeof(jit->hash));
	memset(jit->dormant, 0, sizeof(jit->dormant));
}

// A site is a place in a block's code that calls out to C, with the guest PC and the two prefetched
// opcodes that go with it. Compiled code keeps none of these in the CPU state, so a helper that
// needs them finds its site by its return address.
void jitRecordSite(struct Block* block, uint32_t offset, uint32_t pc, uint32_t op0, uint32_t op1) {
	if (block->siteCount == block->siteCap) {
		block->siteCap = block->siteCap ? block->siteCap * 2 : 8;
		block->sites = realloc(block->sites, block->siteCap * sizeof(*block->sites));
	}
	block->sites[block->siteCount++] = (struct Site) { offset, pc, { op0, op1 } };
}

// Sets r15 and the prefetch to their values at the site the current helper was called from.
static void syncSite(struct GBA* gba) {
	struct Jit* jit = gba->jit;
	if (!gba->jitRunning || !gba->siteValid || !jit->rangeCount) {
		return;
	}
	gba->siteValid = false;
	uint8_t* ret = gba->siteReturn;
	struct SiteCacheEntry* cached = &jit->siteCache[((uintptr_t) ret >> 2) & (SITE_CACHE_SIZE - 1)];
	if (cached->ret == ret) {
		gba->cpu.r[15] = cached->r15;
		gba->cpu.prefetch[0] = cached->ops[0];
		gba->cpu.prefetch[1] = cached->ops[1];
		return;
	}
	bool coldSide = jit->coldRangeCount && backendColdSide(jit, ret);
	struct Block** ranges = coldSide ? jit->coldRanges : jit->ranges;
	size_t lo = 0;
	size_t hi = coldSide ? jit->coldRangeCount : jit->rangeCount;
	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;
		if ((coldSide ? ranges[mid]->cold : ranges[mid]->entry) <= ret) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	struct Block* block = ranges[lo];
	uint32_t offset = ret - block->entry;
	// Sites are recorded in code order.
	int first = 0;
	int last = block->siteCount - 1;
	while (first <= last) {
		int mid = (first + last) / 2;
		struct Site* site = &block->sites[mid];
		if (site->offset < offset) {
			first = mid + 1;
		} else if (site->offset > offset) {
			last = mid - 1;
		} else {
			gba->cpu.r[15] = site->pc + (block->thumb ? 4 : 8);
			gba->cpu.prefetch[0] = site->ops[0];
			gba->cpu.prefetch[1] = site->ops[1];
			cached->ret = ret;
			cached->r15 = gba->cpu.r[15];
			cached->ops[0] = site->ops[0];
			cached->ops[1] = site->ops[1];
			return;
		}
	}
}

// For C code reached from compiled code that needs the guest PC to be right, such as open-bus
// reads.
void jitSyncPc(struct GBA* gba) {
	if (gba->jit) {
		syncSite(gba);
	}
}

// Cartridge contents change only at the GPIO registers (0xC4-0xC9), and the EEPROM sits in the last
// region.
bool jitRomLiteral(struct GBA* gba, const struct Inst* in, uint32_t* value, int* wait) {
	if (in->kind != I_MEM || !in->load || in->cond != COND_AL || in->rn != 15 || !in->op2.isImm || in->patchable ||
	    in->writeback || !in->pre || in->size != 4 || in->rd == 15) {
		return false;
	}
	uint32_t base = in->thumb ? (in->address + 4) & ~3u : in->address + 8;
	uint32_t address = in->up ? base + in->op2.imm : base - in->op2.imm;
	int region = address >> 24;
	uint32_t offset = address & (SIZE_ROM - 1);
	if (region < REGION_ROM0 || region >= REGION_ROM2_EX || (address & 3) || offset + 4 > gba->romSize ||
	    (offset < 0xCA && offset + 4 > 0xC4)) {
		return false;
	}
	if (value) {
		memcpy(value, &gba->rom[offset], 4);
	}
	if (wait) {
		*wait = gba->waitN32[region] + 2;
	}
	return true;
}

// Handles a compiled load or store that left the inline paths, timed like the exact CPU: a load's
// internal cycle follows it.
static int32_t fetchGap(struct GBA* gba);

// Whether the instruction at the site r15 points to is a load into the PC, which a refill follows.
static bool loadsPc(struct GBA* gba) {
	struct Inst in;
	if (gba->cpu.cpsr & PSR_T) {
		uint32_t pc = gba->cpu.r[15] - 4;
		decodeThumb(&in, pc, codeFetch16(gba, pc));
	} else {
		uint32_t pc = gba->cpu.r[15] - 8;
		decodeArm(&in, pc, codeFetch32(gba, pc));
	}
	return in.kind == I_MEM && in.load && in.rd == 15;
}

uint32_t jitSlowAccess(struct GBA* gba, int kind, uint32_t address, uint32_t value) {
	// The PC is looked up only if something asks for it.
	gba->siteValid = true;
	uint32_t runs = gba->eventRuns;
	uint32_t result = 0;
	switch (kind) {
	case MEM_LOAD32:
		result = busRead(gba, address, 4, false);
		break;
	case MEM_LOAD16:
	case MEM_LOADS16:
		result = busRead(gba, address, 2, false);
		break;
	case MEM_LOAD8:
	case MEM_LOADS8:
		result = busRead(gba, address, 1, false);
		break;
	case MEM_STORE32:
		busWrite(gba, address, value, 4, false);
		break;
	case MEM_STORE16:
		busWrite(gba, address, value, 2, false);
		break;
	default:
		busWrite(gba, address, value, 1, false);
		break;
	}
	if (kind < MEM_STORE32) {
		busIdle(gba);
	}
	if (runs != gba->eventRuns) {
		yield(gba);
		// Without the prefetch buffer, cartridge code adds the next fetch's nonsequential part once
		// this returns, and then leaves. An interrupt the events raised is taken before that fetch,
		// so the fetch is left for whatever runs next to count: the exact CPU, or compiled code
		// entered again.
		syncSite(gba);
		if (inRom(gba->cpu.r[15]) && !gba->prefetchEnabled && !gba->prefetchDisabled && !loadsPc(gba)) {
			gba->cpu.cycles -= fetchGap(gba);
			gba->cpu.fetchSeq = false;
		}
	}
	gba->siteValid = false;
	return result;
}

// Throws all compiled code away and starts again from the bottom of the code memory.
void jitFlush(struct GBA* gba) {
	struct Jit* jit = gba->jit;
	freeAll(jit);
	jit->rangeCount = 0;
	jit->coldRangeCount = 0;
	for (int i = 0; i < LOOKUP_SIZE; ++i) {
		jit->lookup[i].key = ~0u;
	}
	memset(gba->iwramCode, 0, sizeof(gba->iwramCode));
	memset(gba->ewramCode, 0, sizeof(gba->ewramCode));
	gba->videoCode = false;
	jit->codeNext = jit->codeStart;
	backendReset(jit);
	memset(jit->tiny, 0, sizeof(jit->tiny));
	memset(jit->siteCache, 0, sizeof(jit->siteCache));
	// The cartridge may have changed.
	memset(jit->idleLoops, 0, sizeof(jit->idleLoops));
	memset(jit->idleSpans, 0, sizeof(jit->idleSpans));
	memset(jit->idleHeads, 0, sizeof(jit->idleHeads));
	memset(jit->idleRejected, 0, sizeof(jit->idleRejected));
	++jit->idleSpanGen;
	jit->staleValid = false;
	jit->romWindowCode = false;
	jit->romBlockCount = 0;
	++jit->flushes;
}

// Called after a save state is loaded: compiled code carries on unless what it was compiled from
// changed, and the run loop starts afresh.
void jitStateLoaded(struct GBA* gba, bool codeChanged) {
	struct Jit* jit = gba->jit;
	if (!jit) {
		return;
	}
	if (codeChanged) {
		jitFlush(gba);
	}
	jit->staleValid = false;
	jit->stepping = false;
	jit->steps = 0;
}

// Forgets what has been learned about which code rewrites itself, as after a reset.
void jitForgetRewrites(struct GBA* gba) {
	struct Jit* jit = gba->jit;
	memset(jit->smcCount, 0, sizeof(jit->smcCount));
	memset(jit->smcShapes, 0, sizeof(jit->smcShapes));
	memset(jit->formSets, 0, sizeof(jit->formSets));
}

// Whether any block covers the page an address is in.
bool jitCovers(struct GBA* gba, uint32_t address) {
	struct Jit* jit = gba->jit;
	int kind = codeKindOf(gba, address);
	if (!jit || !jit->pages[kind]) {
		return false;
	}
	return jit->pages[kind][canonical(kind, address) >> PAGE_SHIFT] != NULL;
}

// Removes the blocks whose instructions overlap [start, stop). Returns true if one of them holds
// currentOffset.
static bool removeBlocks(struct GBA* gba, int kind, uint32_t start, uint32_t stop, uint32_t currentOffset) {
	struct Jit* jit = gba->jit;
	bool current = false;
	uint8_t* map = codeMapFor(gba, kind);
	for (uint32_t page = start >> PAGE_SHIFT; page <= (stop - 1) >> PAGE_SHIFT && page < jit->pageCount[kind]; ++page) {
		uint32_t base = page << PAGE_SHIFT;
		uint32_t top = base + (1 << PAGE_SHIFT);
		// The part of the page the dropped blocks covered.
		uint32_t dirtyFrom = top;
		uint32_t dirtyTo = base;
		struct PageNode** prev = &jit->pages[kind][page];
		while (*prev) {
			struct PageNode* node = *prev;
			struct Block* block = node->block;
			if (block->valid && block->start < stop && block->bodyStop > start) {
				if (currentOffset >= block->start && currentOffset < block->bodyStop) {
					current = true;
				}
				removeBlock(jit, block);
			}
			if (!block->valid) {
				uint32_t from = block->start & ~3u;
				if (from < dirtyFrom) {
					dirtyFrom = from < base ? base : from;
				}
				if (block->stop > dirtyTo) {
					dirtyTo = block->stop > top ? top : block->stop;
				}
				*prev = node->next;
				free(node);
				continue;
			}
			prev = &node->next;
		}
		// Words there stay marked while a remaining block still covers them. Marks combine by
		// priority, so marking just this part again gives the same result as marking the whole
		// page.
		if (map && dirtyFrom < dirtyTo) {
			memset(&map[dirtyFrom >> 2], 0, (dirtyTo - dirtyFrom + 3) >> 2);
			for (struct PageNode* node = jit->pages[kind][page]; node; node = node->next) {
				uint32_t from = node->block->start & ~3u;
				uint32_t to = node->block->stop;
				from = from < dirtyFrom ? dirtyFrom : from;
				to = to > dirtyTo ? dirtyTo : to;
				if (from < to) {
					markWords(map, node->block, from, to);
				}
			}
		}
	}
	return current;
}

// The instruction running now, found from r15's lead over it.
static uint32_t currentInstruction(struct GBA* gba) {
	bool thumb = gba->cpu.cpsr & PSR_T;
	return gba->cpu.r[15] - (thumb ? 4 : 8);
}

// Called when the cartridge's contents change in [start, start + size), as a mapper's window does.
// Cartridge blocks aren't listed by page, so every block is looked at, but only when some were
// compiled from the window; a change anywhere else throws all the code away.
static void romInvalidate(struct GBA* gba, uint32_t start, uint32_t size) {
	struct Jit* jit = gba->jit;
	if (start >= ROM_WINDOW || size > ROM_WINDOW - start) {
		jitRomChanged(gba);
		return;
	}
	if (!jit->romWindowCode) {
		return;
	}
	uint32_t current = gba->jitRunning ? currentInstruction(gba) : 0;
	bool left = false;
	for (struct Block* block = jit->allBlocks; block; block = block->allNext) {
		if (!block->valid || block->codeKind != CODE_ROM) {
			continue;
		}
		uint32_t from = block->start & (SIZE_ROM - 1);
		uint32_t to = from + (block->bodyStop - block->start);
		if (from >= ROM_WINDOW) {
			continue;
		}
		if (from < start + size && to > start) {
			if (gba->jitRunning && inRom(current) && (current & (SIZE_ROM - 1)) - from < to - from) {
				// The running block changed under itself, so leave at the next instruction
				// boundary.
				yield(gba);
			}
			removeBlock(jit, block);
		} else {
			left = true;
		}
	}
	jit->romWindowCode = left;
}

// Called when guest memory in [address, address + size) changes: the blocks compiled from it are
// removed. If the running block is one of them, compiled code is made to leave at its next check.
void jitInvalidate(struct GBA* gba, uint32_t address, uint32_t size) {
	struct Jit* jit = gba->jit;
	int kind = codeKindOf(gba, address);
	if (jit && inRom(address)) {
		romInvalidate(gba, address & (SIZE_ROM - 1), size);
		return;
	}
	if (!jit || !jit->pages[kind]) {
		return;
	}
	uint32_t start = canonical(kind, address);
	++jit->codeWrites;
	syncSite(gba);
	if (gba->jitRunning) {
		// The next two instructions are already prefetched and keep their old opcodes, which the
		// prefetch holds.
		bool thumb = gba->cpu.cpsr & PSR_T;
		int width = thumb ? 2 : 4;
		uint32_t next = currentInstruction(gba) + width;
		if ((next - address < size || next + width - address < size) && !(jit->staleValid && jit->stalePc == next)) {
			jit->staleValid = true;
			jit->stalePc = next;
			jit->staleOps[0] = gba->cpu.prefetch[0];
			jit->staleOps[1] = gba->cpu.prefetch[1];
			yield(gba);
		}
	}
	uint32_t current = gba->jitRunning ? canonical(kind, currentInstruction(gba)) : ~0u;
	if (removeBlocks(gba, kind, start, start + size, current) && gba->jitRunning) {
		// The running block changed under itself, so leave at the next instruction boundary.
		yield(gba);
	}
}

// Returns the bits of an ARM instruction that compiled code can read from its word when it runs: a
// load or store's offset and direction, a data-processing constant, or the amount of a
// data-processing LSL. Returns 0 for none.
static uint32_t patchField(uint32_t opcode) {
	if ((opcode >> 28) == 0xF) {
		return 0;
	}
	// LDR/STR, word or byte, with an immediate offset.
	if ((opcode & 0x0E000000) == 0x04000000) {
		return 0x800FFF;
	}
	// Not MSR or the other instructions in that encoding space, and not a write to the PC.
	if ((opcode & 0x01900000) == 0x01000000 || ((opcode >> 12) & 0xF) == 15) {
		return 0;
	}
	if ((opcode & 0x0E000000) == 0x02000000) {
		return 0xFFF;
	}
	if ((opcode & 0x0E000070) == 0) {
		return 0xF80;
	}
	return 0;
}

// Returns the opcode the CPU prefetched from address, given that the word there just changed from
// old.
static uint32_t prefetchedOpcode(struct GBA* gba, uint32_t address, uint32_t word, uint32_t old, bool thumb) {
	if ((address & ~3u) == word) {
		return thumb ? (old >> ((address & 2) * 8)) & 0xFFFF : old;
	}
	return thumb ? codeFetch16(gba, address) : codeFetch32(gba, address);
}

// Called when a store changes a work RAM word the map marks. Rewrites are counted, and later
// compiles decide by that count. A patchable word whose operand field alone changed needs nothing
// done, since its code reads that field from memory as it runs; neither does a checked word, or one
// that is only prefetched. Anything else removes the blocks compiled from the word.
static void wordWritten(struct GBA* gba, int kind, uint32_t address) {
	struct Jit* jit = gba->jit;
	uint8_t* map = codeMapFor(gba, kind);
	uint32_t word = address & ~3u;
	uint32_t offset = canonical(kind, word);
	uint8_t mark = map[offset >> 2];
	if (!mark) {
		return;
	}
	int index = smcIndex(kind, offset);
	uint32_t old = jit->smcWord[index];
	uint32_t now = codeFetch32(gba, word);
	if (now == old) {
		return;
	}
	++jit->codeWrites;
	syncSite(gba);
	if (gba->jitRunning) {
		// The next two instructions are already prefetched and keep their old opcodes.
		bool thumb = gba->cpu.cpsr & PSR_T;
		int width = thumb ? 2 : 4;
		uint32_t pc = currentInstruction(gba);
		uint32_t next = pc + width;
		if (((next & ~3u) == word || ((next + width) & ~3u) == word) && !(jit->staleValid && jit->stalePc == next)) {
			jit->staleValid = true;
			jit->stalePc = next;
			jit->staleOps[0] = prefetchedOpcode(gba, next, word, old, thumb);
			jit->staleOps[1] = prefetchedOpcode(gba, next + width, word, old, thumb);
			yield(gba);
		}
	}
	// Every rewrite counts, so a word the game keeps switching between forms is still compiled to
	// read its offset.
	uint32_t field = patchField(now);
	bool sameShape = field && !((now ^ old) & ~field);
	if (jit->smcCount[index] < 255) {
		++jit->smcCount[index];
	}
	if (!sameShape) {
		// A game reloading code here now and then isn't rewriting it as it runs.
		uint16_t frame = gba->video.frameCounter;
		if ((uint16_t) (frame - jit->smcShapeFrame[index]) > REWRITE_FRAMES) {
			jit->smcShapes[index] = 0;
		}
		jit->smcShapeFrame[index] = frame;
		if (jit->smcShapes[index] < 255) {
			++jit->smcShapes[index];
		}
	}
	jit->smcWord[index] = now;
	if (mark == CODE_MAP_TAIL || mark == CODE_MAP_CHECKED || (mark == CODE_MAP_PATCHABLE && sameShape)) {
		++jit->patchWrites;
		return;
	}
	++jit->invalidations;
	static int smcLog = -1;
	if (smcLog < 0) {
		smcLog = getenv("ARMGBA_SMCLOG") != NULL;
	}
	if (smcLog) {
		static int logged;
		if (logged++ < 30) {
			fprintf(stderr, "write %08X: %08X -> %08X mark %d\n", address, old, now, mark);
		}
	}
	uint32_t current = gba->jitRunning ? canonical(kind, currentInstruction(gba)) : ~0u;
	if (removeBlocks(gba, kind, offset, offset + 4, current) && gba->jitRunning) {
		yield(gba);
	}
}

// Called by the memory routines for every store that could have landed on code.
void jitCodeWritten(struct GBA* gba, uint32_t address, int size) {
	if (!gba->jit) {
		return;
	}
	int kind = codeKindOf(gba, address);
	if (kind != CODE_IWRAM && kind != CODE_EWRAM) {
		jitInvalidate(gba, address & ~3u, size < 4 ? 4 : size);
		return;
	}
	for (uint32_t word = address & ~3u; word < address + size; word += 4) {
		wordWritten(gba, kind, word);
	}
}

// Called from compiled stores, after the store, when the code maps flag the word.
void jitHelperCodeWrite(struct GBA* gba, uint32_t address) {
	gba->siteValid = true;
	syncSite(gba);
	wordWritten(gba, codeKindOf(gba, address), address);
	gba->siteValid = false;
}

// Returns how much more a nonsequential fetch costs than a sequential one at the PC.
static int32_t fetchGap(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	int region = cpu->r[15] >> 24;
	return (cpu->cpsr & PSR_T) ? gba->waitN16[region] - gba->waitS16[region] : gba->waitN32[region] - gba->waitS32[region];
}

// Called when the cartridge shows other contents: all compiled code goes before any more cartridge
// code runs.
void jitRomChanged(struct GBA* gba) {
	if (!gba->jit) {
		return;
	}
	if (gba->jitRunning) {
		gba->jit->flushRequested = true;
		yield(gba);
	} else {
		jitFlush(gba);
	}
}

// gap: jitFetchGap from before the change.
void jitWaitstatesChanged(struct GBA* gba, int32_t gap) {
	if (gba->jit) {
		// Cartridge code has its costs compiled in, so it is set aside once compiled code has been
		// left.
		gba->jit->waitsChanged = true;
		if (gba->jitRunning) {
			if (gap >= 0) {
				gba->cpu.cycles += fetchGap(gba) - gap;
			}
			yield(gba);
		}
	}
}

// Enters a block in the hash table, and in the small direct-mapped table the dispatcher tries
// first.
static void registerBlock(struct Jit* jit, struct Block* block) {
	if (block->codeKind == CODE_ROM && !block->careful && !block->listed) {
		if (jit->romBlockCount == jit->romBlockCap) {
			jit->romBlockCap = jit->romBlockCap ? 2 * jit->romBlockCap : 256;
			jit->romBlocks = realloc(jit->romBlocks, jit->romBlockCap * sizeof(*jit->romBlocks));
		}
		jit->romBlocks[jit->romBlockCount++] = block;
		block->listed = true;
	}
	uint32_t key = blockKey(block->pc, block->thumb);
	uint32_t h = hashBlock(key);
	block->hashNext = jit->hash[h];
	jit->hash[h] = block;
	if (!jit->gba->traceFile) {
		struct LookupEntry* entry = &jit->lookup[lookupHash(key)];
		entry->key = key;
		entry->entry = block->entry;
	}
	addPages(jit, block);
}

// Loads and stores compiled to read their offset at run time may come back with a different offset.
static bool sameCode(const struct Block* block, const uint8_t* code) {
	uint32_t size = block->bodyStop - block->start;
	if (!block->patchable) {
		return !memcmp(code, block->bytes, size);
	}
	for (uint32_t i = 0; i < size / 4; ++i) {
		uint32_t now;
		uint32_t then;
		memcpy(&now, code + i * 4, 4);
		memcpy(&then, block->bytes + i * 4, 4);
		uint32_t ignored = i < 64 && ((block->patchable >> i) & 1) ? patchField(then) : 0;
		if ((now ^ then) & ~ignored) {
			return false;
		}
	}
	return true;
}

// Whether the game keeps changing a word into other instructions as it runs.
static bool reshaped(struct Jit* jit, int kind, uint32_t address) {
	if (kind != CODE_IWRAM && kind != CODE_EWRAM) {
		return false;
	}
	return jit->smcShapes[smcIndex(kind, canonical(kind, address))] >= SPLIT_THRESHOLD;
}

// A rewritten word's known forms are kept in a small hash table.
static struct FormSet* formSet(struct Jit* jit, int kind, uint32_t address) {
	uint32_t index = smcIndex(kind, canonical(kind, address));
	return &jit->formSets[(index * 0x9E3779B1u) >> (32 - FORM_SET_BITS)];
}

// Whether a word has gone through more forms than the table keeps, so keeping them doesn't pay.
static bool formsKeepChanging(struct Jit* jit, int kind, uint32_t address) {
	struct FormSet* set = formSet(jit, kind, address);
	return set->word == (uint32_t) smcIndex(kind, canonical(kind, address)) + 1 && set->dropped >= FORM_DROP_LIMIT;
}

// A rewritten ARM word is compiled to check which form it holds when it runs. One whose forms keep
// changing too gets a block of its own, so a new form only costs that block. Rewritten Thumb code
// gets blocks of its own that are thrown away when it changes.
static bool alone(struct Jit* jit, int kind, uint32_t address, bool thumb) {
	return reshaped(jit, kind, address) && (thumb || formsKeepChanging(jit, kind, address));
}

// Whether an instruction is compiled to read its offset or constant when it runs, so one form
// covers them all.
static bool patchableInst(const struct Inst* in) {
	if (in->thumb || !patchField(in->opcode)) {
		return false;
	}
	if (in->kind == I_MEM) {
		return in->op2.isImm && in->rn != 15;
	}
	return in->kind == I_ALU && (in->op2.isImm || (!in->op2.byReg && in->op2.type == SHIFT_LSL));
}

// Whether a block dates from before a word in it became one the game keeps rewriting or patching.
static bool outdated(struct Jit* jit, const struct Block* block) {
	if (block->thumb) {
		return false;
	}
	int kind = block->codeKind;
	for (uint32_t address = block->pc; address < block->end; address += 4) {
		if (reshaped(jit, kind, address)) {
			return true;
		}
		int i = (address - block->pc) >> 2;
		if (i < 64 && !((block->patchable >> i) & 1) && jit->smcCount[smcIndex(kind, canonical(kind, address))] >= PATCH_THRESHOLD) {
			struct Inst in;
			decodeArm(&in, address, codeFetch32(jit->gba, address));
			if (patchableInst(&in)) {
				return true;
			}
		}
	}
	return false;
}

// Moves the form the word holds now to the front of those seen there, then decodes them all.
static void learnForms(struct Jit* jit, int kind, uint32_t address, uint32_t opcode, struct Poly* poly) {
	struct FormSet* set = formSet(jit, kind, address);
	uint32_t word = smcIndex(kind, canonical(kind, address)) + 1;
	if (set->word != word) {
		set->word = word;
		set->count = 0;
		set->dropped = 0;
	}
	struct Inst now;
	decodeArm(&now, address, opcode);
	uint32_t mask = patchableInst(&now) ? ~patchField(opcode) : ~0u;
	int at = 0;
	while (at < set->count && ((set->ops[at] ^ opcode) & mask)) {
		++at;
	}
	if (at == set->count) {
		if (set->count < MAX_FORMS) {
			++set->count;
		} else {
			--at;
			if (set->dropped < 255) {
				++set->dropped;
			}
		}
	}
	memmove(&set->ops[1], &set->ops[0], at * sizeof(*set->ops));
	set->ops[0] = opcode;
	poly->count = set->count;
	for (int i = 0; i < set->count; ++i) {
		struct Inst* form = &poly->forms[i];
		decodeArm(form, address, set->ops[i]);
		form->patchable = patchableInst(form);
		poly->masks[i] = form->patchable ? ~patchField(form->opcode) : ~0u;
	}
}

// A block whose code was copied away and back again doesn't need recompiling.
static struct Block* revive(struct GBA* gba, uint32_t key) {
	struct Jit* jit = gba->jit;
	struct Block** prev = &jit->dormant[hashBlock(key)];
	for (; *prev; prev = &(*prev)->dormantNext) {
		struct Block* block = *prev;
		if (blockKey(block->pc, block->thumb) != key) {
			continue;
		}
		// A head's check left in a block that is no longer one would leave at its start forever.
		if (block->idleHead != jitIsIdleHead(jit, key)) {
			continue;
		}
		if (block->setAside) {
			// Cartridge code that hasn't changed comes back once the wait states are what it was
			// compiled for.
			if (block->waits != currentWaits(gba)) {
				continue;
			}
			block->setAside = false;
			if (block->twin) {
				block->twin->valid = true;
			}
		} else {
			const uint8_t* code = codePointer(gba, block->pc);
			if (!code || !sameCode(block, code) || outdated(jit, block)) {
				continue;
			}
		}
		*prev = block->dormantNext;
		block->valid = true;
		int kind = block->codeKind;
		if (kind == CODE_IWRAM || kind == CODE_EWRAM) {
			for (uint32_t offset = block->start & ~3u; offset < block->stop; offset += 4) {
				jit->smcWord[smcIndex(kind, offset)] = codeFetch32(gba, (block->pc & ~(codeKindSpan[kind] - 1)) + offset);
			}
		}
		registerBlock(jit, block);
		++jit->revived;
		return block;
	}
	return NULL;
}

// The block ends where a mirror or the region would wrap, so its range stays contiguous.
static uint32_t mirrorMask(int kind) {
	switch (kind) {
	case CODE_BIOS:
		return SIZE_BIOS - 1;
	case CODE_ROM:
		return SIZE_ROM - 1;
	case CODE_VRAM:
		return 0x1FFFF;
	default:
		return codeKindSpan[kind] - 1;
	}
}

// parent: for a block's careful copy, the block itself, whose instructions it compiles the same
// way.
static void idleSpan(struct Jit* jit, const struct Block* block, const struct Inst* last);

static struct Block* compileBlock(struct GBA* gba, uint32_t pc, bool thumb, const struct Block* parent) {
	bool careful = parent != NULL;
	uint32_t limit = careful ? parent->end : 0;
	struct Jit* jit = gba->jit;
	struct Inst insts[MAX_BLOCK_INSTS];
	struct Poly polys[MAX_BLOCK_INSTS];
	int polyCount = 0;
	uint64_t polymorphic = 0;
	uint32_t nextOps[2];
	int width = thumb ? 2 : 4;
	int kind = codeKindOf(gba, pc);
	uint32_t mask = mirrorMask(kind);
	int count = 0;
	// Each form of a rewritten word is compiled, so all of them count against the block's size.
	int units = 0;
	uint32_t address = pc;
	while (count < MAX_BLOCK_INSTS) {
		uint32_t opcode = thumb ? codeFetch16(gba, address) : codeFetch32(gba, address);
		struct Inst* in = &insts[count];
		if (thumb) {
			decodeThumb(in, address, opcode);
		} else {
			decodeArm(in, address, opcode);
		}
		bool ends = instEndsBlock(in) || jitStepOnly(in);
		if (!thumb && (careful ? (parent->polymorphic >> count) & 1 : reshaped(jit, kind, address))) {
			// The block continues past a form that ends it, unless all the forms do.
			struct Poly* poly = &polys[polyCount];
			learnForms(jit, kind, address, opcode, poly);
			if (count && units + poly->count + 1 > MAX_BLOCK_INSTS) {
				break;
			}
			++polyCount;
			units += poly->count;
			in->poly = poly;
			polymorphic |= 1ull << count;
			for (int i = 0; i < poly->count; ++i) {
				ends = ends && (instEndsBlock(&poly->forms[i]) || jitStepOnly(&poly->forms[i]));
			}
		}
		++count;
		++units;
		if (ends || units >= MAX_BLOCK_INSTS) {
			break;
		}
		address += width;
		if (codeKindOf(gba, address) != kind || ((address ^ pc) & ~mask) || address == limit || burstEdge(gba, address, thumb)) {
			break;
		}
		if (!careful && (address & 3) == 0 && (alone(jit, kind, address, thumb) || alone(jit, kind, address - 4, thumb))) {
			break;
		}
	}
	uint32_t end = pc + count * width;
	uint64_t patchable = 0;
	if ((kind == CODE_IWRAM || kind == CODE_EWRAM) && !thumb) {
		for (int i = 0; i < count; ++i) {
			uint32_t offset = canonical(kind, insts[i].address);
			int index = smcIndex(kind, offset);
			bool patch = careful ? (parent->patchable >> i) & 1 : patchableInst(&insts[i]) && jit->smcCount[index] >= PATCH_THRESHOLD;
			if (!insts[i].poly && patch) {
				insts[i].patchable = true;
				patchable |= 1ull << i;
				++jit->patchableCompiles;
			}
		}
	}
	for (int i = 0; i < 2; ++i) {
		uint32_t at = end + i * width;
		nextOps[i] = thumb ? codeFetch16(gba, at) : codeFetch32(gba, at);
	}

	struct Block* block = calloc(1, sizeof(*block));
	block->pc = pc;
	block->end = end;
	block->thumb = thumb;
	block->careful = careful;
	block->patchable = patchable;
	block->polymorphic = polymorphic;
	block->count = count;
	block->codeKind = kind;
	// The two instructions prefetched past the end are watched too.
	block->start = canonical(kind, pc);
	block->bodyStop = block->start + count * width;
	block->stop = block->start + (count + 2) * width;
	if (codeKindSpan[kind] && block->stop > codeKindSpan[kind]) {
		block->stop = codeKindSpan[kind];
	}
	if (kind == CODE_IWRAM || kind == CODE_EWRAM) {
		for (uint32_t offset = block->start & ~3u; offset < block->stop; offset += 4) {
			jit->smcWord[smcIndex(kind, offset)] = codeFetch32(gba, (pc & ~(codeKindSpan[kind] - 1)) + offset);
		}
	}
	block->valid = true;
	block->idleHead = jitIsIdleHead(jit, blockKey(pc, thumb));
	if (careful) {
		block->entries = calloc(count, sizeof(*block->entries));
	}

	if (!backendCompile(jit, block, insts, count, nextOps)) {
		free(block->sites);
		free(block->out);
		free(block->entries);
		free(block);
		return NULL;
	}
	idleSpan(jit, block, &insts[count - 1]);
	if (jit->rangeCount == jit->rangeCap) {
		jit->rangeCap = jit->rangeCap ? jit->rangeCap * 2 : 4096;
		jit->ranges = realloc(jit->ranges, jit->rangeCap * sizeof(*jit->ranges));
	}
	// A block whose body is in the cold code (a careful copy, where the backend keeps them apart)
	// is found by that alone.
	if (!block->cold || !backendColdSide(jit, block->entry)) {
		jit->ranges[jit->rangeCount++] = block;
	}
	if (block->cold) {
		if (jit->coldRangeCount == jit->coldRangeCap) {
			jit->coldRangeCap = jit->coldRangeCap ? jit->coldRangeCap * 2 : 4096;
			jit->coldRanges = realloc(jit->coldRanges, jit->coldRangeCap * sizeof(*jit->coldRanges));
		}
		jit->coldRanges[jit->coldRangeCount++] = block;
	}
	block->allNext = jit->allBlocks;
	jit->allBlocks = block;
	block->waits = currentWaits(gba);
	if (kind == CODE_ROM && (block->start & (SIZE_ROM - 1)) < ROM_WINDOW) {
		jit->romWindowCode = true;
	}
	if (careful) {
		// A careful copy is reached only from its block's checks, but is thrown away with the code
		// like any block.
		addPages(jit, block);
	} else {
		block->cached = true;
		// Once copied back, rewritten words could hold forms the block wasn't compiled for.
		if (kind != CODE_BIOS && kind != CODE_ROM && kind != CODE_NONE && !polymorphic) {
			block->bytes = malloc(block->bodyStop - block->start);
			memcpy(block->bytes, codePointer(gba, pc), block->bodyStop - block->start);
		}
		registerBlock(jit, block);
	}
	++jit->compiled;
	// ARMGBA_DUMP=<directory> writes each block's host code there for disassembly.
	static const char* dumpDir;
	static bool dumpChecked;
	if (!dumpChecked) {
		dumpDir = getenv("ARMGBA_DUMP");
		dumpChecked = true;
	}
	if (dumpDir) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%08X%s%s.bin", dumpDir, pc, thumb ? "t" : "a", careful ? "c" : "");
		FILE* f = fopen(path, "wb");
		if (f) {
			const uint8_t* end = block->cold && backendColdSide(jit, block->entry) ? jit->coldNext : jit->codeNext;
			fwrite(block->entry, 1, end - block->entry, f);
			fclose(f);
		}
	}
	return block;
}

// With ARMGBA_PERFMAP set, writes each piece of compiled code's address and name to a perf map
// file, so a profile shows guest blocks by address.
void jitNameCode(const void* start, const void* end, const char* name) {
	static FILE* map;
	static bool checked;
	if (!checked) {
		checked = true;
		if (getenv("ARMGBA_PERFMAP")) {
			char path[64];
			snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int) getpid());
			map = fopen(path, "w");
		}
	}
	if (map && end > start) {
		fprintf(map, "%lx %lx %s\n", (unsigned long) (uintptr_t) start, (unsigned long) ((const uint8_t*) end - (const uint8_t*) start), name);
		fflush(map);
	}
}

// A block's two pieces: its body and its cold part.
void jitNameBlock(const struct Block* block, const void* bodyEnd, const void* cold, const void* end) {
	char name[32];
	snprintf(name, sizeof(name), "b%08X%s%s%s", block->pc, block->thumb ? "t" : "a", block->careful ? "c" : "", block->polymorphic ? "p" : "");
	jitNameCode(block->entry, bodyEnd, name);
	strcat(name, ".cold");
	jitNameCode(cold, end, name);
}

// Returns the block for a PC: an existing one, one brought back unchanged from before a rewrite, or
// a new one.
static struct Block* getBlock(struct GBA* gba, uint32_t pc, bool thumb) {
	struct Jit* jit = gba->jit;
	uint32_t key = blockKey(pc, thumb);
	struct Block* block = findBlock(jit, key);
	if (block) {
		return block;
	}
	block = revive(gba, key);
	if (block) {
		return block;
	}
	block = compileBlock(gba, pc, thumb, NULL);
	if (!block) {
		++jit->flushReasons[1];
		jitFlush(gba);
		block = compileBlock(gba, pc, thumb, NULL);
	}
	return block;
}

// Returns a block's careful copy, compiled the first time one of its checks fires, or NULL if
// there's no room before the next flush.
static struct Block* twinOf(struct GBA* gba, struct Block* block) {
	if (block->twin && !block->twin->valid) {
		block->twin = NULL;
	}
	if (!block->twin) {
		block->twin = compileBlock(gba, block->pc, block->thumb, block);
	}
	return block->twin;
}

// Patches a block's exit to jump straight to its target, now that the target exists.
static void linkExit(struct Jit* jit, struct Link* link, struct Block* target) {
	if (!link->from->valid || link->to || !target->valid || link->formMiss) {
		return;
	}
	if (link->careful && link->index >= target->count) {
		return;
	}
	link->to = target;
	link->nextIncoming = target->incoming;
	target->incoming = link;
	backendPatchJump(link->site, link->careful ? target->entries[link->index] : target->entry);
}

// Whether there is memory at a PC to compile from. A BX to an address with bit 1 set leaves ARM
// code running with the PC two bytes off a word, as Thumb's BX PC does from an address that isn't
// word-aligned. The prefetch buffer's bookkeeping in compiled code assumes whole words, so that
// code goes to the exact CPU.
static bool compilable(struct GBA* gba, uint32_t pc, bool thumb) {
	if (!thumb && (pc & 2) && inRom(pc) && gba->prefetchEnabled) {
		return false;
	}
	// The third cartridge region's power-on wait states make a sequential access the slower one.
	// Compiled code counts a nonsequential fetch as a sequential one plus extra, which would then
	// be negative, so code run there that way goes to the exact CPU.
	if (inRom(pc) && !gba->prefetchEnabled) {
		int region = pc >> 24;
		if (thumb ? gba->waitN16[region] < gba->waitS16[region] : gba->waitN32[region] < gba->waitS32[region]) {
			return false;
		}
	}
	return memoryCanFetch(gba, pc) && !burstEdge(gba, pc, thumb);
}

// Compiled cartridge code with the prefetch buffer assumes it reads ahead of the next fetch at the
// rate the region's wait states give; without the buffer, that it is idle.
static bool romEntryOk(struct GBA* gba, uint32_t pc, bool thumb) {
	struct Prefetcher* p = &gba->prefetcher;
	if (gba->prefetchDisabled) {
		return false;
	}
	if (!gba->prefetchEnabled) {
		return !gba->cpu.prefetching;
	}
	int width = thumb ? 2 : 4;
	int32_t duty = busCycles(gba, pc, width, true);
	return gba->cpu.prefetching && p->head == pc + 2 * width && p->width == width && p->duty == duty && p->full == (thumb ? 8 : 4) * duty;
}

// Compiled code assumes the bus is as it leaves it: no DMA waiting or just finished, no IRQ to
// take, the prefetch holding what memory holds, and the prefetch buffer as compiled for.
static bool compiledMayRun(struct GBA* gba, uint32_t pc, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	if (gba->dmaRunnable || gba->lastAccessDma || (gba->irq.line && (!cpu->irqMasked || !(cpu->cpsr & PSR_I)))) {
		return false;
	}
	if (!compilable(gba, pc, thumb) || (inRom(pc) && !romEntryOk(gba, pc, thumb))) {
		return false;
	}
	// Compiled code updates the active region only when it changes, since PC writes go between
	// regions.
	if (gba->activeRegion != (int32_t) (pc >> 24)) {
		return false;
	}
	if (thumb) {
		return cpu->prefetch[0] == codeFetch16(gba, pc) && cpu->prefetch[1] == codeFetch16(gba, pc + 2);
	}
	return cpu->prefetch[0] == codeFetch32(gba, pc) && cpu->prefetch[1] == codeFetch32(gba, pc + 4);
}

// Called when the wait states change: blocks compiled from the cartridge for other wait states wait
// on the dormant lists.
static void setAsideCartridge(struct GBA* gba) {
	struct Jit* jit = gba->jit;
	uint32_t waits = currentWaits(gba);
	int kept = 0;
	for (int i = 0; i < jit->romBlockCount; ++i) {
		struct Block* block = jit->romBlocks[i];
		if (block->valid && block->waits == waits) {
			jit->romBlocks[kept++] = block;
			continue;
		}
		if (block->valid) {
			block->setAside = true;
			removeBlock(jit, block);
		}
		block->listed = false;
	}
	jit->romBlockCount = kept;
}

// Idle loops that span blocks and calls (struct IdleLoop): the exact CPU goes round one twice to
// learn it, and later arrivals at its start in the same state skip the times round before the next
// event.

// Data a loop may read: memory only the CPU and DMA change, the cartridge, and the keys.
static bool idleReadable(struct GBA* gba, uint32_t address, int size) {
	switch (address >> 24) {
	case REGION_EWRAM:
	case REGION_IWRAM:
	case REGION_PALETTE:
	case REGION_VRAM:
	case REGION_OAM:
		return true;
	case REGION_IO:
		return size <= 2 && (address & 0xFFFFFE) == REG_KEYINPUT;
	case REGION_ROM0 ... REGION_ROM2_EX - 1:
		// Vast Fame's protection answers reads itself, and the GPIO port can read back through the
		// ROM.
		return !gba->cart.vfame && !(gba->gpio.readWrite && (address & 0x1FFFFFF) >= 0xC4 && (address & 0x1FFFFFF) < 0xCA);
	default:
		return false;
	}
}

static uint32_t idlePeek(struct GBA* gba, uint32_t address, int size) {
	switch (size) {
	case 4:
		return memoryLoad32(gba, address, NULL);
	case 2:
		return memoryLoad16(gba, address, NULL);
	default:
		return memoryLoad8(gba, address, NULL);
	}
}

static bool idleOverlaps(const struct IdleRead* a, uint32_t address, int size) {
	return address < a->address + a->size && a->address < address + size;
}

// Each time round's reads and writes, in order; the two times round must match.
static void idleLog(struct GBA* gba, uint32_t address, int size, uint32_t value, bool write) {
	struct Jit* jit = gba->jit;
	int lap = jit->idleLap - 1;
	if (jit->idleAccesses[lap] == IDLE_ACCESSES) {
		jit->idleBad = true;
		return;
	}
	jit->idleAccess[lap][jit->idleAccesses[lap]++] = (struct IdleAccess) { address & -size, value, size, write };
}

void jitIdleRead(struct GBA* gba, uint32_t address, int size, uint32_t value) {
	if (!idleReadable(gba, address, size)) {
		gba->jit->idleBad = true;
		return;
	}
	// The cartridge doesn't change.
	if ((address >> 24) < REGION_ROM0) {
		idleLog(gba, address, size, value, false);
	}
}

// Writes go to RAM.
void jitIdleWrite(struct GBA* gba, uint32_t address, uint32_t value, int size) {
	unsigned region = address >> 24;
	if (region != REGION_EWRAM && region != REGION_IWRAM) {
		gba->jit->idleBad = true;
		return;
	}
	if (size < 4) {
		value &= (1u << (size * 8)) - 1;
	}
	idleLog(gba, address, size, value, true);
}

// From the second time round: what it reads without having written it first, to check on arrival,
// and what each address it writes is left holding, to write when skipping.
static bool idleSummarize(struct Jit* jit, struct IdleLoop* loop) {
	const struct IdleAccess* log = jit->idleAccess[1];
	int count = jit->idleAccesses[1];
	loop->reads = 0;
	loop->writes = 0;
	for (int i = 0; i < count; ++i) {
		struct IdleRead access = { log[i].address, log[i].value, log[i].size };
		if (log[i].write) {
			int w = 0;
			while (w < loop->writes && !(loop->write[w].address == access.address && loop->write[w].size == access.size)) {
				++w;
			}
			for (int o = 0; o < loop->writes; ++o) {
				// A narrower or wider write over another would need the two merged, which isn't
				// handled.
				if (o != w && idleOverlaps(&loop->write[o], access.address, access.size)) {
					return false;
				}
			}
			if (w == loop->writes) {
				if (loop->writes == IDLE_READS) {
					return false;
				}
				++loop->writes;
			}
			loop->write[w] = access;
			continue;
		}
		bool own = false;
		for (int w = 0; w < loop->writes && !own; ++w) {
			own = idleOverlaps(&loop->write[w], access.address, access.size);
		}
		if (own) {
			continue;
		}
		if (loop->reads == IDLE_READS) {
			return false;
		}
		loop->read[loop->reads++] = access;
	}
	return true;
}

// What a time round depends on, with the prefetch buffer's progress as its distance from full.
static void idleCapture(struct GBA* gba, struct IdleState* s) {
	struct CPU* cpu = &gba->cpu;
	struct Prefetcher* p = &gba->prefetcher;
	memset(s, 0, sizeof(*s));
	memcpy(s->r, cpu->r, sizeof(s->r));
	s->cpsr = cpu->cpsr;
	s->prefetch[0] = cpu->prefetch[0];
	s->prefetch[1] = cpu->prefetch[1];
	if (cpu->prefetching) {
		int32_t lead = timingCurrentTime(gba) - p->base;
		s->prefetcherLead = lead < p->full ? lead : p->full;
		s->prefetcherWidth = p->width;
		s->prefetcherDuty = p->duty;
		s->prefetcherFull = p->full;
		s->prefetcherHead = p->head;
	}
	s->activeRegion = gba->activeRegion;
	s->fetchSeq = cpu->fetchSeq;
	s->prefetching = cpu->prefetching;
	s->irqMasked = cpu->irqMasked;
	s->lastAccessDma = gba->lastAccessDma;
}

static uint32_t idleHash(const struct IdleState* s) {
	const uint32_t* words = (const uint32_t*) s;
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < sizeof(*s) / 4; ++i) {
		h = (h ^ words[i]) * 16777619u;
	}
	return h;
}

// Skips the times round that end before the next event: their writes are made and their cycles
// counted.
static void idleSkip(struct GBA* gba, const struct IdleLoop* loop) {
	struct CPU* cpu = &gba->cpu;
	int32_t room = cpu->nextEvent - cpu->cycles;
	if (room <= loop->period) {
		return;
	}
	for (int i = 0; i < loop->writes; ++i) {
		const struct IdleRead* w = &loop->write[i];
		switch (w->size) {
		case 4:
			memoryStore32(gba, w->address, w->value, NULL);
			break;
		case 2:
			memoryStore16(gba, w->address, w->value, NULL);
			break;
		default:
			memoryStore8(gba, w->address, w->value, NULL);
			break;
		}
	}
	int32_t add = (room - 1) / loop->period * loop->period;
	cpu->cycles += add;
	if (cpu->prefetching) {
		gba->prefetcher.base += add;
	}
}

// Goes round from here twice with the exact CPU; it's a loop if the two times round match.
static void idleProbe(struct GBA* gba, const struct IdleState* start) {
	struct Jit* jit = gba->jit;
	struct CPU* cpu = &gba->cpu;
	uint32_t runs = gba->eventRuns;
	int32_t from = cpu->cycles;
	int32_t period = 0;
	struct IdleState now;
	jit->idleLap = 1;
	jit->idleAccesses[0] = 0;
	jit->idleAccesses[1] = 0;
	jit->idleBad = false;
	gba->idleProbing = true;
	bool found = false;
	int indirect = 0;
	for (int i = 0; i < IDLE_PROBE_STEPS && !jit->idleBad; ++i) {
		if (jit->idleLap == 2) {
			struct Inst in;
			if (cpu->cpsr & PSR_T) {
				decodeThumb(&in, cpu->r[15] - 4, (uint16_t) cpu->prefetch[0]);
			} else {
				decodeArm(&in, cpu->r[15] - 8, cpu->prefetch[0]);
			}
			indirect += in.kind == I_BX || (in.kind == I_MEM && in.load && in.rd == 15) ||
			            (in.kind == I_BLOCK && in.load && (in.list & 0x8000));
		}
		stepInstruction(gba);
		if (gba->eventRuns != runs || cpu->halted || gba->dmaRunnable || (cpu->cpsr & 0x1F) != (start->cpsr & 0x1F)) {
			break;
		}
		unsigned region = (cpu->r[15] >> 24);
		if (region < REGION_ROM0 || region >= REGION_ROM2_EX) {
			break;
		}
		if (cpu->r[15] != start->r[15]) {
			continue;
		}
		idleCapture(gba, &now);
		if (memcmp(&now, start, sizeof(now))) {
			continue;
		}
		if (jit->idleLap == 1) {
			period = cpu->cycles - from;
			from = cpu->cycles;
			jit->idleLap = 2;
			continue;
		}
		// A loop with no indirect branch costs compiled code little to go round, less than leaving
		// for the run loop after each event would.
		found = !jit->idleBad && indirect && cpu->cycles - from == period && jit->idleAccesses[0] == jit->idleAccesses[1];
		for (int a = 0; a < jit->idleAccesses[0] && found; ++a) {
			const struct IdleAccess* x = &jit->idleAccess[0][a];
			const struct IdleAccess* y = &jit->idleAccess[1][a];
			found = x->address == y->address && x->value == y->value && x->size == y->size && x->write == y->write;
		}
		break;
	}
	gba->idleProbing = false;
	jit->idleLap = 0;
	uint32_t hash = idleHash(start);
	unsigned slot = hash & 255;
	if (!found) {
		if (jit->idleSeen[slot].fails < 6) {
			++jit->idleSeen[slot].fails;
		}
		jit->idleSeen[slot].wait = (1 << jit->idleSeen[slot].fails) - 1;
		return;
	}
	struct IdleLoop* loop = &jit->idleLoops[jit->idleNext];
	if (!idleSummarize(jit, loop)) {
		loop->valid = false;
		return;
	}
	jit->idleSeen[slot].fails = 0;
	jit->idleNext = (jit->idleNext + 1) % IDLE_LOOPS;
	loop->valid = true;
	loop->start = *start;
	loop->period = period;
}

bool jitIsIdleHead(const struct Jit* jit, uint32_t key) {
	for (int i = 0; i < IDLE_HEADS; ++i) {
		if (jit->idleHeads[i].key == key) {
			return true;
		}
	}
	return false;
}

// When recompiled, a block takes on or drops the start of an idle loop's head.
static void idleRecompile(struct Jit* jit, uint32_t key) {
	struct Block* block = findBlock(jit, key);
	if (block) {
		removeBlock(jit, block);
	}
}

// Counts a probe or a skip at a head; one that keeps probing without ever skipping stops being a
// head.
static void idleHeadCount(struct Jit* jit, uint32_t r15, bool probe) {
	bool thumb = jit->gba->cpu.cpsr & PSR_T;
	uint32_t key = blockKey(r15 - (thumb ? 2 : 4), thumb);
	for (int i = 0; i < IDLE_HEADS; ++i) {
		if (jit->idleHeads[i].key != key) {
			continue;
		}
		if (!probe) {
			++jit->idleHeads[i].skips;
		} else if (++jit->idleHeads[i].probes >= IDLE_GIVE_UP && !jit->idleHeads[i].skips) {
			jit->idleHeads[i].key = 0;
			jit->idleRejected[jit->idleRejectedNext] = key;
			jit->idleRejectedNext = (jit->idleRejectedNext + 1) % IDLE_HEADS;
			idleRecompile(jit, key);
		}
		return;
	}
}

// A block ending in a branch back to an earlier cartridge block: a loop that may be idle.
static void idleSpan(struct Jit* jit, const struct Block* block, const struct Inst* last) {
	if (last->kind != I_B || last->link || last->cond != COND_AL || last->target >= block->pc || !inRom(block->pc) ||
	    last->address - last->target > IDLE_SPAN_BYTES) {
		return;
	}
	uint32_t head = blockKey(last->target, block->thumb);
	for (int i = 0; i < IDLE_SPANS; ++i) {
		if (jit->idleSpans[i].head == head) {
			return;
		}
	}
	jit->idleSpans[jit->idleSpanNext] = (typeof(jit->idleSpans[0])) { head, blockKey(last->address, block->thumb), 0, 0 };
	jit->idleSpanNext = (jit->idleSpanNext + 1) % IDLE_SPANS;
	++jit->idleSpanGen;
}

// Called when events ran in compiled code at block. If that happens often enough in a frame within
// a loop's span, its head becomes an idle head, replacing the one that has skipped least.
static void idleNote(struct Jit* jit, const struct Block* constBlock) {
	struct Block* block = (struct Block*) constBlock;
	if (block->idleSpanGen != jit->idleSpanGen) {
		block->idleSpanGen = jit->idleSpanGen;
		block->idleSpan = 0;
		uint32_t key = blockKey(block->pc, block->thumb);
		for (int i = 0; i < IDLE_SPANS; ++i) {
			uint32_t head = jit->idleSpans[i].head;
			if (head && key >= head && key <= jit->idleSpans[i].end && (key & 1) == (head & 1)) {
				block->idleSpan = i + 1;
				break;
			}
		}
	}
	if (!block->idleSpan) {
		return;
	}
	typeof(jit->idleSpans[0])* span = &jit->idleSpans[block->idleSpan - 1];
	if (span->frame != jit->frame) {
		span->frame = jit->frame;
		span->hits = 0;
	}
	if (++span->hits != IDLE_NOMINATE || jitIsIdleHead(jit, span->head)) {
		return;
	}
	for (int i = 0; i < IDLE_HEADS; ++i) {
		if (jit->idleRejected[i] == span->head) {
			return;
		}
	}
	int slot = 0;
	for (int i = 1; i < IDLE_HEADS; ++i) {
		if (jit->idleHeads[i].skips < jit->idleHeads[slot].skips) {
			slot = i;
		}
	}
	uint32_t old = jit->idleHeads[slot].key;
	jit->idleHeads[slot] = (typeof(jit->idleHeads[0])) { span->head, 0, 0 };
	if (old) {
		idleRecompile(jit, old);
	}
	idleRecompile(jit, span->head);
}

// Called at a cartridge PC compiled code is about to run from. At a known idle loop's start, skips
// ahead; at an unknown place, probes, unless probes there keep failing. Returns whether the CPU
// moved on.
static bool idleCheck(struct GBA* gba) {
	struct Jit* jit = gba->jit;
	struct CPU* cpu = &gba->cpu;
	unsigned region = cpu->r[15] >> 24;
	if (region < REGION_ROM0 || region >= REGION_ROM2_EX || cpu->nextEvent - cpu->cycles < 256 || gba->traceFile) {
		return false;
	}
	struct IdleState now;
	bool captured = false;
	for (int i = 0; i < IDLE_LOOPS; ++i) {
		const struct IdleLoop* loop = &jit->idleLoops[i];
		if (!loop->valid || loop->start.r[15] != cpu->r[15]) {
			continue;
		}
		if (!captured) {
			idleCapture(gba, &now);
			captured = true;
		}
		if (memcmp(&now, &loop->start, sizeof(now))) {
			continue;
		}
		bool same = true;
		for (int r = 0; r < loop->reads && same; ++r) {
			same = idlePeek(gba, loop->read[r].address, loop->read[r].size) == loop->read[r].value;
		}
		if (same) {
			idleSkip(gba, loop);
			idleHeadCount(jit, cpu->r[15], false);
			return false;
		}
	}
	if (!captured) {
		idleCapture(gba, &now);
	}
	uint32_t hash = idleHash(&now);
	unsigned slot = hash & 255;
	if (jit->idleSeen[slot].hash != hash) {
		jit->idleSeen[slot].hash = hash;
		jit->idleSeen[slot].wait = 0;
		jit->idleSeen[slot].fails = 0;
		return false;
	}
	if (jit->idleSeen[slot].wait) {
		--jit->idleSeen[slot].wait;
		return false;
	}
	idleHeadCount(jit, cpu->r[15], true);
	idleProbe(gba, &now);
	return true;
}

// Runs compiled code where nothing can happen inside its instructions, and the exact CPU around
// events, DMA, IRQs and anything else compiled code leaves to it.
void jitRunLoop(struct GBA* gba) {
	struct Jit* jit = gba->jit;
	struct CPU* cpu = &gba->cpu;
	uint32_t frame = gba->video.frameCounter;
	jit->frame = frame;
	// A careful copy to continue in straight away, entered where its block's check fired.
	void* resume = NULL;
	uint32_t resumeKey = ~0u;
	while (gba->video.frameCounter == frame && !gba->returnRequested) {
		void* resumeHere = resume;
		resume = NULL;
		// Closed once passed, so it can't come round again 2^31 cycles later.
		if (jit->stepping && (int32_t) (jit->stepUntil - timingCurrentTime(gba)) <= 0) {
			jit->stepping = false;
		}
		if (jit->flushRequested) {
			resumeHere = NULL;
			jit->flushRequested = false;
			jit->waitsChanged = false;
			++jit->flushReasons[0];
			jitFlush(gba);
		}
		if (jit->waitsChanged) {
			resumeHere = NULL;
			jit->waitsChanged = false;
			setAsideCartridge(gba);
		}
		if (cpu->halted) {
			if (cpu->cycles < cpu->nextEvent) {
				cpu->cycles = cpu->nextEvent;
			}
			processEvents(gba);
			continue;
		}
		bool thumb = cpu->cpsr & PSR_T;
		int width = thumb ? 2 : 4;
		uint32_t pc = cpu->r[15] - width;
		uint32_t key = blockKey(pc, thumb);
		// An idle head left for this keeps going until events run again, whether looked at or not.
		bool head = jitIsIdleHead(jit, key);
		if (head) {
			gba->idleHeadRuns = gba->eventRuns;
		}
		if (jit->steps || jit->stepping || !compiledMayRun(gba, pc, thumb)) {
			if (jit->steps) {
				--jit->steps;
			}
			++jit->stepped;
			stepInstruction(gba);
			continue;
		}
		if (head) {
			bool stepped = idleCheck(gba);
			gba->idleHeadRuns = gba->eventRuns;
			if (stepped) {
				continue;
			}
		}
		struct LookupEntry* cached = &jit->lookup[lookupHash(key)];
		void* entry = resumeHere && resumeKey == key ? resumeHere : cached->key == key ? cached->entry : NULL;
		if (!entry) {
			struct Block* block = getBlock(gba, pc, thumb);
			if (!block) {
				stepInstruction(gba);
				continue;
			}
			entry = block->entry;
		}
		if (gba->traceFile) {
			stepTrace(gba);
		}
		if (!cpu->fetchSeq && !(inRom(pc) && gba->prefetchEnabled)) {
			// The block counts its first fetch as sequential.
			int region = pc >> 24;
			cpu->cycles += thumb ? gba->waitN16[region] - gba->waitS16[region] : gba->waitN32[region] - gba->waitS32[region];
		}
		gba->dmaIdleCredit = 0;
		// Compiled code counts the nonsequential fetches itself.
		cpu->fetchSeq = true;
		// A waiting loop is skipped only after a time round entirely in compiled code.
		gba->idleMark = 0;
		gba->jitRunning = true;
		int reason = jit->enter(gba, entry);
		gba->jitRunning = false;
		++jit->exits[reason];
		// A check stops before the event it saw; the exact CPU runs until that event has happened.
		int32_t seen = gba->timing.masterCycles + cpu->nextEvent;
		// While halted, events run now, as HALTCNT left them: an IRQ already waiting wakes the CPU
		// at once.
		if (!cpu->halted) {
			timingResetNextEvent(gba);
		}
		thumb = cpu->cpsr & PSR_T;
		width = thumb ? 2 : 4;
		uint32_t next = gba->exitPc & ~1u;
		if (reason != EXIT_EXACT) {
			cpu->r[15] = next + width;
			if (jit->staleValid && jit->stalePc == next) {
				cpu->prefetch[0] = jit->staleOps[0];
				cpu->prefetch[1] = jit->staleOps[1];
			} else if (thumb) {
				cpu->prefetch[0] = codeFetch16(gba, next);
				cpu->prefetch[1] = codeFetch16(gba, next + 2);
			} else {
				cpu->prefetch[0] = codeFetch32(gba, next);
				cpu->prefetch[1] = codeFetch32(gba, next + 4);
			}
			jit->staleValid = false;
			cpu->irqMasked = cpu->cpsr & PSR_I;
		}
		gba->jitLeave = false;
		if (gba->jitDeferred) {
			// Events put off by a branch or block end run before the next instruction.
			gba->jitDeferred = false;
			if (!cpu->halted && cpu->cycles >= cpu->nextEvent) {
				processEvents(gba);
			}
		}
		switch (reason) {
		case EXIT_CHECK:
			jit->stepUntil = seen;
			jit->stepping = true;
			jit->steps = 1;
			break;
		case EXIT_STEP:
			jit->steps = 1;
			break;
		case EXIT_LOOKUP: {
			// Another block took its slot, so it takes the slot back.
			struct Block* target = findBlock(jit, blockKey(next, thumb));
			if (target && !gba->traceFile) {
				struct LookupEntry* entry = &jit->lookup[lookupHash(blockKey(next, thumb))];
				entry->key = blockKey(next, thumb);
				entry->entry = target->entry;
			}
			break;
		}
		case EXIT_LINK: {
			struct Link* link = gba->exitLink;
			if (link->careful) {
				// An event falls inside the instructions after the check, so continue in the
				// careful copy.
				struct Block* twin = NULL;
				if (!jit->flushRequested && !jit->waitsChanged && link->from->valid) {
					uint64_t flushes = jit->flushes;
					twin = twinOf(gba, link->from);
					if (twin && flushes == jit->flushes && link->index < twin->count) {
						linkExit(jit, link, twin);
						if (compiledMayRun(gba, next, thumb)) {
							resume = twin->entries[link->index];
							resumeKey = blockKey(next, thumb);
						}
						break;
					}
				}
				jit->stepUntil = seen;
				jit->stepping = true;
				jit->steps = 1;
				break;
			}
			if (link->formMiss) {
				// When compiled again, the block and any others starting at the word learn the form
				// it holds now.
				++jit->formMisses;
				removeBlock(jit, link->from);
				struct Block* at = findBlock(jit, blockKey(next, thumb));
				if (at) {
					removeBlock(jit, at);
				}
				break;
			}
			// When tracing, every block is entered from here.
			if (!jit->flushRequested && !jit->waitsChanged && compilable(gba, next, thumb) && !gba->traceFile) {
				uint64_t flushes = jit->flushes;
				struct Block* target = getBlock(gba, next, thumb);
				if (target && flushes == jit->flushes) {
					linkExit(jit, link, target);
				}
			}
			break;
		}
		}
	}
	gba->returnRequested = false;
}

// Instructions compiled code leaves to the exact CPU: CPSR writes and exceptions, which are rare
// enough not to be worth their bookkeeping.
bool jitStepOnly(const struct Inst* in) {
	if (in->cond == 0xF) {
		return false;
	}
	// ARMGBA_STEP=<hex mask of InstKind bits> leaves those kinds to the exact CPU too, to find
	// which one compiled code gets wrong.
	static int mask = -1;
	if (mask < 0) {
		mask = getenv("ARMGBA_STEP") ? (int) strtol(getenv("ARMGBA_STEP"), NULL, 16) : 0;
	}
	if (mask & (1 << in->kind)) {
		return true;
	}
	return in->kind == I_UNDEFINED || in->kind == I_STUB;
}

// Whether an instruction changes the CPSR in a way compiled code leaves to the exact CPU.
bool jitSystem(const struct Inst* in) {
	if (in->cond == 0xF || jitStepOnly(in)) {
		return false;
	}
	switch (in->kind) {
	case I_MSR:
	case I_SWI:
		return true;
	case I_ALU:
		// A write to r15 that restores the CPSR, or an ARM compare into r15.
		return !in->thumb && in->rd == 15 && in->setFlags;
	case I_BLOCK:
		return in->load && in->userBank && (in->list & 0x8000);
	default:
		return false;
	}
}

// Returns the key of the block at the PC in the CPU state.
static uint32_t currentKey(struct GBA* gba) {
	bool thumb = gba->cpu.cpsr & PSR_T;
	return blockKey(gba->cpu.r[15] - (thumb ? 2 : 4), thumb);
}

// Runs a block transfer or swap that left the inline paths on the exact CPU, with its fetch already
// counted. Returns the next block key after a PC write, or ~0 to go on. The helpers that run one
// instruction on the exact CPU decode it each time; the few a game runs over and over, such as the
// BIOS's around its calls, are kept decoded.
static const struct Inst* decodeCached(uint32_t opcode, uint32_t address) {
	static struct {
		uint32_t opcode;
		uint32_t address;
		bool valid;
		struct Inst in;
	} cache[64];
	unsigned slot = ((address >> 1) ^ (address >> 7)) & 63;
	if (!cache[slot].valid || cache[slot].address != address || cache[slot].opcode != opcode) {
		if (address & 1) {
			decodeThumb(&cache[slot].in, address & ~1u, opcode);
		} else {
			decodeArm(&cache[slot].in, address, opcode);
		}
		cache[slot].opcode = opcode;
		cache[slot].address = address;
		cache[slot].valid = true;
	}
	return &cache[slot].in;
}

uint32_t jitHelperExecute(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	uint32_t runs = gba->eventRuns;
	const struct Inst* in = decodeCached(opcode, address);
	stepExecute(gba, in);
	// Compiled code counts the nonsequential fetch after it.
	cpu->fetchSeq = true;
	// Events that ran inside it may need the exact CPU next.
	if (runs != gba->eventRuns) {
		yield(gba);
		// As in jitSlowAccess, the fetch after it is left to be counted where it is made.
		if (inRom(address) && !gba->prefetchEnabled && !gba->prefetchDisabled && !(in->kind == I_BLOCK && in->load && (in->list & 0x8000))) {
			int region = address >> 24;
			cpu->cycles -= (address & 1) ? gba->waitN16[region] - gba->waitS16[region] : gba->waitN32[region] - gba->waitS32[region];
			cpu->fetchSeq = false;
		}
	}
	if (in->kind != I_BLOCK || !in->load || (in->list && !(in->list & 0x8000))) {
		return ~0u;
	}
	bool thumb = cpu->cpsr & PSR_T;
	uint32_t pc = cpu->r[15] - (thumb ? 2 : 4);
	if (inRom(pc) && !romEntryOk(gba, pc, thumb)) {
		gba->jitLeave = true;
		yield(gba);
	}
	return currentKey(gba);
}

// Runs an instruction that writes the CPSR on the exact CPU, with its fetch already counted.
// Returns ~0 to go on after it, the block key to go on at after a PC write, or JIT_EXACT for the
// exact CPU to take over at the next instruction: for an IRQ waiting to be taken, or a switch of
// instruction set by MSR.
uint32_t jitHelperSystem(struct GBA* gba, uint32_t opcode, uint32_t address) {
	struct CPU* cpu = &gba->cpu;
	uint32_t runs = gba->eventRuns;
	uint32_t before = cpu->cpsr;
	const struct Inst* in = decodeCached(opcode, address);
	stepExecute(gba, in);
	cpu->fetchSeq = true;
	if (runs != gba->eventRuns) {
		yield(gba);
	}
	bool writesPc = in->kind != I_MSR && !(in->kind == I_ALU && in->op >= ALU_TST && in->op <= ALU_CMN);
	if (!writesPc) {
		// Once unmasked, an IRQ comes one instruction later: the mask from before still holds for
		// the next instruction.
		if (((before & PSR_I) && !(cpu->cpsr & PSR_I) && gba->irq.line) || ((before ^ cpu->cpsr) & PSR_T)) {
			cpu->irqMasked = before & PSR_I;
			return JIT_EXACT;
		}
		return ~0u;
	}
	// After a refill, the IRQ check uses the mask as it is now.
	if (gba->irq.line && !cpu->irqMasked) {
		return JIT_EXACT;
	}
	bool thumb = cpu->cpsr & PSR_T;
	uint32_t pc = cpu->r[15] - (thumb ? 2 : 4);
	if (inRom(pc) && !romEntryOk(gba, pc, thumb)) {
		gba->jitLeave = true;
		yield(gba);
	}
	return currentKey(gba);
}

// Whether compiled code can go on at pc after the exact CPU ran. Like compiledMayRun, except that
// cartridge code may be entered just after a data access stopped the prefetch buffer, which
// compiled code then restarts. Also sets up what compiled code relies on.
static bool continuable(struct GBA* gba, uint32_t pc, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	int width = thumb ? 2 : 4;
	if (gba->dmaRunnable || gba->lastAccessDma || (gba->irq.line && (!cpu->irqMasked || !(cpu->cpsr & PSR_I)))) {
		return false;
	}
	if (!compilable(gba, pc, thumb) || gba->activeRegion != (int32_t) (pc >> 24)) {
		return false;
	}
	if (thumb ? cpu->prefetch[0] != codeFetch16(gba, pc) || cpu->prefetch[1] != codeFetch16(gba, pc + 2)
	          : cpu->prefetch[0] != codeFetch32(gba, pc) || cpu->prefetch[1] != codeFetch32(gba, pc + 4)) {
		return false;
	}
	if (!inRom(pc)) {
		cpu->fetchSeq = true;
		return true;
	}
	struct Prefetcher* p = &gba->prefetcher;
	if (gba->prefetchDisabled) {
		return false;
	}
	if (!gba->prefetchEnabled) {
		if (gba->cpu.prefetching) {
			return false;
		}
		if (!cpu->fetchSeq) {
			int region = pc >> 24;
			cpu->cycles += thumb ? gba->waitN16[region] - gba->waitS16[region] : gba->waitN32[region] - gba->waitS32[region];
			cpu->fetchSeq = true;
		}
		return true;
	}
	if (gba->cpu.prefetching) {
		return romEntryOk(gba, pc, thumb);
	}
	int32_t duty = busCycles(gba, pc, width, true);
	if (p->width != width || p->duty != duty || p->full != (thumb ? 8 : 4) * duty) {
		return false;
	}
	// The fetch after a stop restarts the buffer: a fetch from it whose read finishes then.
	p->base = timingCurrentTime(gba) + busCycles(gba, pc, width, false) - duty;
	p->head = pc + 2 * width;
	cpu->fetchSeq = false;
	return true;
}

// Whether compiled code can go on at an instruction boundary in block.
static bool boundaryOk(struct GBA* gba, const struct Block* block) {
	struct CPU* cpu = &gba->cpu;
	struct Jit* jit = gba->jit;
	return block->valid && !gba->jitLeave && !cpu->halted && !jit->flushRequested && !jit->waitsChanged && !jit->staleValid && gba->video.frameCounter == jit->frame &&
	       !gba->returnRequested && !gba->dmaRunnable && !gba->lastAccessDma && !(gba->irq.line && (!cpu->irqMasked || !(cpu->cpsr & PSR_I)));
}

// Runs the events due by now at an instruction boundary. Returns false when compiled code has to
// leave.
static bool runDue(struct GBA* gba, const struct Block* block) {
	// At the end of a frame, or when asked to, the run loop returns before anything else runs.
	if (gba->video.frameCounter != gba->jit->frame || gba->returnRequested) {
		return false;
	}
	if (gba->cpu.cycles >= gba->cpu.nextEvent) {
		processEvents(gba);
		idleNote(gba->jit, block);
	}
	return boundaryOk(gba, block);
}

// A careful copy's check at an instruction of block that takes at most cost cycles. Events due by
// its end, including ones already due, fall in its fetch or later. They run once it is done if it
// falls through to the next instruction and none of them starts DMA or ends the run; otherwise the
// exact CPU runs the instruction. Not used for cartridge code without the prefetch buffer: that
// counts a nonsequential fetch before its instruction starts, so only the exact CPU can tell which
// instruction an event falls in.
int jitHelperCareful(struct GBA* gba, int32_t cost, int deferrable, const struct Block* block) {
	struct CPU* cpu = &gba->cpu;
	// Events put off by the branch or block end that led here run first, between the instructions.
	if (gba->jitDeferred) {
		if (!runDue(gba, block)) {
			return CAREFUL_LEAVE;
		}
		gba->jitDeferred = false;
	}
	if (!boundaryOk(gba, block)) {
		return CAREFUL_LEAVE;
	}
	if (cpu->cycles + cost < cpu->nextEvent) {
		return CAREFUL_RUN;
	}
	if (deferrable && timingDeferrable(gba, cpu->cycles + cost)) {
		gba->jitDeferred = true;
		return CAREFUL_RUN;
	}
	return CAREFUL_STEP;
}

// Called after an instruction whose events were put off. Returns 0 to go on.
int jitHelperDeferred(struct GBA* gba, const struct Block* block) {
	gba->jitDeferred = false;
	return !runDue(gba, block);
}

// Called when a careful copy's check fires because an event falls inside the instruction at pc. The
// exact CPU runs that instruction, and any after it compiled code can't, then returns the block key
// to go on at, or ~0 to leave the state to the run loop.
uint32_t jitHelperStepOne(struct GBA* gba, uint32_t pc) {
	struct Jit* jit = gba->jit;
	struct CPU* cpu = &gba->cpu;
	bool thumb = cpu->cpsr & PSR_T;
	int width = thumb ? 2 : 4;
	cpu->r[15] = pc + width;
	if (jit->staleValid && jit->stalePc == pc) {
		cpu->prefetch[0] = jit->staleOps[0];
		cpu->prefetch[1] = jit->staleOps[1];
	} else if (thumb) {
		cpu->prefetch[0] = codeFetch16(gba, pc);
		cpu->prefetch[1] = codeFetch16(gba, pc + 2);
	} else {
		cpu->prefetch[0] = codeFetch32(gba, pc);
		cpu->prefetch[1] = codeFetch32(gba, pc + 4);
	}
	jit->staleValid = false;
	cpu->irqMasked = cpu->cpsr & PSR_I;
	gba->jitRunning = false;
	gba->idleMark = 0;
	// Events put off by the branch or block end that led here run before this instruction.
	if (gba->jitDeferred) {
		gba->jitDeferred = false;
		if (cpu->cycles >= cpu->nextEvent) {
			processEvents(gba);
		}
	}
	for (int i = 0; i < 8; ++i) {
		// Called when halted, or when something else made compiled code yield. The run loop returns
		// at the end of a frame, or when asked to.
		if (cpu->halted || jit->flushRequested || jit->waitsChanged || gba->video.frameCounter != jit->frame || gba->returnRequested) {
			return ~0u;
		}
		timingResetNextEvent(gba);
		++jit->carefulSteps;
		stepInstruction(gba);
		thumb = cpu->cpsr & PSR_T;
		width = thumb ? 2 : 4;
		uint32_t next = cpu->r[15] - width;
		if (!cpu->halted && !jit->flushRequested && !jit->waitsChanged && gba->video.frameCounter == jit->frame && !gba->returnRequested && continuable(gba, next, thumb)) {
			gba->dmaIdleCredit = 0;
			gba->jitLeave = false;
			gba->jitRunning = true;
			return blockKey(next, thumb);
		}
	}
	return ~0u;
}

// Handles a PC write that compiled code doesn't refill itself: to the cartridge, whose prefetch
// buffer sets the cost, or close to an event.
void jitHelperReload(struct GBA* gba, uint32_t target) {
	struct CPU* cpu = &gba->cpu;
	uint32_t runs = gba->eventRuns;
	if (gba->activeRegion == REGION_BIOS) {
		// What memorySetActiveRegion keeps for BIOS reads once the PC leaves the BIOS.
		cpu->prefetch[1] = gba->biosPrefetchNext;
	}
	uint32_t pc = target & ~1u;
	cpu->r[15] = pc;
	stepReload(gba);
	bool thumb = cpu->cpsr & PSR_T;
	if (inRom(pc) && !romEntryOk(gba, pc, thumb)) {
		gba->jitLeave = true;
		yield(gba);
	} else if (runs != gba->eventRuns) {
		yield(gba);
	}
}

// Cartridge code compiled without the prefetch buffer counts a data access's next fetch as
// sequential and adds the difference to the access. Returns that difference, or -1 for other code.
int32_t jitFetchGap(struct GBA* gba) {
	if (!gba->jitRunning) {
		return -1;
	}
	jitSyncPc(gba);
	if (!inRom(gba->cpu.r[15]) || gba->prefetchEnabled) {
		return -1;
	}
	return fetchGap(gba);
}

// The multiplier stops early once the rest of the operand is all zeros (or all ones, when signed).
static int multiplyStages(uint32_t value, bool sign) {
	if (sign) {
		value ^= (uint32_t) ((int32_t) value >> 31);
	}
	return (31 - __builtin_clz(value | 1)) / 8 + 1;
}

// Returns the carry flag a flag-setting multiply leaves, which depends on the multiplier's
// internals. It's rarely wanted, so compiled code calls out for it only when the flag is live.
uint32_t jitMultiplyCarry(struct GBA* gba, uint32_t kind) {
	uint32_t multiplicand = gba->jitScratch[0];
	uint32_t multiplier = gba->jitScratch[1];
	uint32_t accumLo = gba->jitScratch[2];
	uint32_t accumHi = gba->jitScratch[3];
	if (!kind) {
		if (multiplyStages(multiplier, true) == 4) {
			return cpuMultiplyCarryFull(multiplier);
		}
		return cpuMultiplyCarryLow(multiplicand, multiplier, accumLo);
	}
	bool sign = kind == 2;
	if (multiplyStages(multiplier, sign) == 4) {
		return cpuMultiplyCarryHigh(multiplicand, multiplier, accumHi, sign);
	}
	return cpuMultiplyCarryLow(multiplicand, multiplier, accumLo);
}

// With ARMGBA_STATS set, prints the counts when the recompiler is freed.
static void printStats(const struct Jit* jit) {
	fprintf(stderr, "code writes: %llu other shape, %llu patchable, %llu same shape but compiled in; flushes: %llu requested %llu full\n",
	        (unsigned long long) jit->writeReasons[0], (unsigned long long) jit->writeReasons[1], (unsigned long long) jit->writeReasons[2],
	        (unsigned long long) jit->flushReasons[0], (unsigned long long) jit->flushReasons[1]);
	fprintf(stderr, "jit: %llu revived, ", (unsigned long long) jit->revived);
	fprintf(stderr, "%llu compiled, %llu flushes, %llu invalidating code writes, %llu patch writes, %llu patchable compiles, %llu form misses, exits %llu check %llu lookup %llu link\n",
	        (unsigned long long) jit->compiled, (unsigned long long) jit->flushes, (unsigned long long) jit->invalidations,
	        (unsigned long long) jit->patchWrites, (unsigned long long) jit->patchableCompiles, (unsigned long long) jit->formMisses, (unsigned long long) jit->exits[0],
	        (unsigned long long) jit->exits[1], (unsigned long long) jit->exits[2]);
	fprintf(stderr, "%llu exits to step, %llu instructions stepped, %llu in careful copies (%llu exits)\n", (unsigned long long) jit->exits[3],
	        (unsigned long long) jit->stepped, (unsigned long long) jit->carefulSteps, (unsigned long long) jit->exits[EXIT_EXACT]);
}

// Maps the code memory, writable and executable, and has the backend emit its shared routines.
bool jitInit(struct GBA* gba) {
	struct Jit* jit = calloc(1, sizeof(*jit));
	if (!jit) {
		return false;
	}
	jit->gba = gba;
	gba->jit = jit;
	jit->code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (jit->code == MAP_FAILED) {
		free(jit);
		gba->jit = NULL;
		return false;
	}
	jit->codeEnd = jit->code + CODE_SIZE;
	jit->lookup = calloc(LOOKUP_SIZE, sizeof(*jit->lookup));
	for (int i = 0; i < LOOKUP_SIZE; ++i) {
		jit->lookup[i].key = ~0u;
	}
	gba->lookup = jit->lookup;
	for (int kind = 0; kind < CODE_KINDS; ++kind) {
		if (codeKindSpan[kind]) {
			jit->pageCount[kind] = codeKindSpan[kind] >> PAGE_SHIFT;
			jit->pages[kind] = calloc(jit->pageCount[kind], sizeof(struct PageNode*));
		}
	}
	// prefetcherStop's rule: a stop costs one more cycle when a read is about to finish, which in
	// ARM state is either halfword of the word.
	for (int wide = 0; wide < 2; ++wide) {
		for (int duty = 1; duty < 32; ++duty) {
			int full = (wide ? 4 : 8) * duty;
			for (int lead = 0; lead < 128 && lead < full; ++lead) {
				int into = lead % duty;
				gba->jitStopPenalty[wide][duty][lead] = into == duty - 1 || (wide && into == duty - duty / 2 - 1);
			}
		}
	}
	jit->codeNext = jit->code;
	if (!backendInit(jit)) {
		jitDeinit(gba);
		return false;
	}
	jit->codeStart = jit->codeNext;
	backendReset(jit);
	return true;
}

// Frees everything jitInit and compiling allocated.
void jitDeinit(struct GBA* gba) {
	struct Jit* jit = gba->jit;
	if (!jit) {
		return;
	}
	if (getenv("ARMGBA_STATS")) {
		printStats(jit);
	}
	freeAll(jit);
	for (int kind = 0; kind < CODE_KINDS; ++kind) {
		free(jit->pages[kind]);
	}
	free(jit->lookup);
	free(jit->romBlocks);
	free(jit->ranges);
	free(jit->coldRanges);
	munmap(jit->code, CODE_SIZE);
	free(jit);
	gba->jit = NULL;
}
