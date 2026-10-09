/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARM_JIT_H
#define ARM_JIT_H

#include <mgba-util/common.h>

CXX_GUARD_START

#if (defined(__aarch64__) || defined(__x86_64__)) && !defined(__APPLE__) && !defined(_WIN32)
#define M_ARM_JIT 1
#endif

#ifdef M_ARM_JIT

// RAM that code can run from, as one run of words: IWRAM, then EWRAM
#define ARM_JIT_IWRAM_WORDS 0x2000
#define ARM_JIT_EWRAM_WORDS 0x10000
#define ARM_JIT_RAM_WORDS (ARM_JIT_IWRAM_WORDS + ARM_JIT_EWRAM_WORDS)
#define ARM_JIT_PAGES 0x10000
#define ARM_JIT_MAX_SPAN 66

struct ARMCore;
struct ARMJitBlock;
struct ARMJitEntry;
struct ARMJitLink;
struct ARMJitPage;

struct ARMJit {
	// Set when blocks are removed, so code running in one stops after the call that did it
	uint8_t smcHit;

	uint8_t* code;
	size_t codeSize;
	size_t codeUsed;
	// Generated once: enter(cpu, block) runs blocks until an event is due or the next block
	// isn't compiled; blocks exit to dispatch, and toC returns to the caller
	size_t codeStart;
	void (*enter)(struct ARMCore*, void*);
	uint8_t* dispatch;
	uint8_t* linkDispatch;
	uint8_t* toC;
	// Set by a link stub so the dispatcher patches that exit to jump straight to the block
	struct ARMJitLink* pendingLink;
	// Runtime copies of patched words, taken when the GBA's pipeline would fetch them
	uint32_t fetched[ARM_JIT_MAX_SPAN];

	struct ARMJitBlock* blockList;
	// Entry points by address, in 4 KiB pages of the bus
	struct ARMJitPage* pages[ARM_JIT_PAGES];
	// Number of compiled blocks whose code (or baked-in prefetch) covers each RAM word
	uint8_t cover[ARM_JIT_RAM_WORDS];
	// Writes into compiled code, by the word written
	uint8_t patched[ARM_JIT_RAM_WORDS];
};

struct ARMJit* ARMJitCreate(void);
void ARMJitDestroy(struct ARMJit* jit);
void ARMJitFlush(struct ARMJit* jit);
void ARMJitDropBlocks(struct ARMJit* jit);
void ARMJitDropRegion(struct ARMJit* jit, uint32_t start, uint32_t end);
void ARMJitInvalidateWord(struct ARMJit* jit, unsigned word);
bool ARMJitRun(struct ARMCore* cpu);
void ARMJitLink(struct ARMJit* jit, struct ARMJitLink* link, struct ARMJitEntry* entry);

static inline int ARMJitRamWord(uint32_t address) {
	switch (address >> 24) {
	case 2:
		return ARM_JIT_IWRAM_WORDS + ((address & 0x3FFFF) >> 2);
	case 3:
		return (address & 0x7FFF) >> 2;
	default:
		return -1;
	}
}

static inline void ARMJitNotifyWrite(struct ARMJit* jit, uint32_t address) {
	int word = ARMJitRamWord(address);
	if (word >= 0 && jit->cover[word]) {
		ARMJitInvalidateWord(jit, word);
	}
}

#endif

CXX_GUARD_END

#endif
