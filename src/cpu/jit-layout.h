/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_JIT_LAYOUT_H
#define ARMGBA_JIT_LAYOUT_H

// For backends that keep blocks' bodies apart from their cold code: how the code memory is laid out,
// and the retry of a block that doesn't fit. Included after jit-compile.h; the backend defines
// compileInStretch.

// Blocks' bodies are kept apart from their cold code (slow paths, exits, and whole careful copies),
// so the code a game spends its time in is dense in the caches; on a Cortex-A7 or A55 with 32 KiB of
// instruction cache, that decides much of a busy game's speed. The code memory is laid out in
// stretches, each with a part for bodies and a part for cold code, so a block's cold code stays near
// its body.
enum {
	HOT_PART = 352 << 10,
	COLD_PART = 640 << 10,
	STRETCH = HOT_PART + COLD_PART,
	// Room a block may need in each part.
	HOT_ROOM = 32 << 10,
	COLD_ROOM = 64 << 10,
};

static bool compileInStretch(struct Jit*, struct Block*, struct Inst* insts, int count, const uint32_t* nextOps);

// Lays out a stretch at base: block bodies fill its first part and cold code its second.
static void startStretch(struct Jit* jit, uint8_t* base) {
	jit->codeNext = base;
	jit->hotEnd = base + HOT_PART;
	jit->coldNext = jit->hotEnd;
	jit->coldEnd = base + STRETCH;
}

// After a flush, compiling starts in a new stretch.
void backendReset(struct Jit* jit) {
	startStretch(jit, jit->codeNext);
}

// Whether an address is in a stretch's cold part.
bool backendColdSide(const struct Jit* jit, const uint8_t* p) {
	return p >= jit->codeStart && (size_t) (p - jit->codeStart) % STRETCH >= HOT_PART;
}

// Forgets what a compile whose code didn't fit left behind.
static void abandon(struct Compiler* c) {
	c->block->siteCount = 0;
	free(c->block->out);
	c->block->out = NULL;
	c->block->outCount = 0;
}

// A block's code goes in the stretch being filled while there is room for a large block there,
// otherwise in a new one; a block too large for the room left is compiled again in a new stretch.
// Fails if the code memory is full; the caller then flushes.
bool backendCompile(struct Jit* jit, struct Block* block, struct Inst* insts, int count, const uint32_t* nextOps) {
	if (jit->hotEnd - jit->codeNext < HOT_ROOM || jit->coldEnd - jit->coldNext < COLD_ROOM) {
		if (jit->codeEnd - jit->coldEnd < STRETCH) {
			return false;
		}
		startStretch(jit, jit->coldEnd);
	}
	if (compileInStretch(jit, block, insts, count, nextOps)) {
		return true;
	}
	if (jit->codeEnd - jit->coldEnd < STRETCH) {
		return false;
	}
	startStretch(jit, jit->coldEnd);
	return compileInStretch(jit, block, insts, count, nextOps);
}

#endif
