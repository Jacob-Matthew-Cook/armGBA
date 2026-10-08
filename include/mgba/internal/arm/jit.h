/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARM_JIT_H
#define ARM_JIT_H

#include <mgba-util/common.h>

CXX_GUARD_START

#if defined(__aarch64__) && !defined(__APPLE__)
#define M_ARM_JIT 1
#endif

#ifdef M_ARM_JIT

#define ARM_JIT_IWRAM_WORDS 0x2000

struct ARMCore;
struct ARMJitBlock;

struct ARMJit {
	// Number of compiled blocks whose code (or baked-in prefetch) covers each IWRAM word
	uint8_t cover[ARM_JIT_IWRAM_WORDS];
	struct ARMJitBlock* blocks[ARM_JIT_IWRAM_WORDS];
	uint8_t hits[ARM_JIT_IWRAM_WORDS];
	uint8_t invalidations[ARM_JIT_IWRAM_WORDS];

	struct ARMJitBlock* current;
	uint8_t smcHit;

	uint8_t* code;
	size_t codeSize;
	size_t codeUsed;
};

struct ARMJit* ARMJitCreate(void);
void ARMJitDestroy(struct ARMJit* jit);
void ARMJitFlush(struct ARMJit* jit);
void ARMJitInvalidateWord(struct ARMJit* jit, unsigned word);
bool ARMJitRun(struct ARMCore* cpu);

static inline void ARMJitNotifyWrite(struct ARMJit* jit, uint32_t address) {
	unsigned word = (address & 0x7FFF) >> 2;
	if (jit->cover[word]) {
		ARMJitInvalidateWord(jit, word);
	}
}

#endif

CXX_GUARD_END

#endif
