/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_CHEATS_H
#define ARMGBA_CHEATS_H

// Cheat codes: GameShark, Action Replay and CodeBreaker, decoded and applied as mGBA's cheat engine
// does.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct GBA;

// One operation a code decodes to.
struct Cheat {
	int type;
	int width;
	uint32_t address;
	uint32_t operand;
	uint32_t repeat;
	uint32_t negativeRepeat;
	int32_t addressOffset;
	int32_t operandOffset;
};

// A ROM halfword a code replaces, and what was there before.
struct CheatPatch {
	uint32_t address;
	uint32_t value;
	int width;
	uint32_t original;
	bool applied;
};

// The codes in force. They are decoded in the order given, since a master code changes how the
// codes after it are decrypted.
struct CheatSet {
	struct Cheat* list;
	size_t count;
	size_t capacity;
	struct CheatPatch* patches;
	size_t patchCount;
	size_t patchCapacity;
	// Where decoding stands: a code still waiting for its next line, the open Action Replay block,
	// and each format's keys.
	size_t incompleteCheat;
	size_t incompletePatch;
	size_t currentBlock;
	int remainingAddresses;
	int gsaVersion;
	uint32_t gsaSeeds[4];
	uint32_t cbRngState;
	uint32_t cbMaster;
	uint8_t cbTable[0x30];
	uint32_t cbSeeds[4];
};

void cheatsInit(struct CheatSet*);
// Undoes the codes' ROM changes and forgets them.
void cheatsClear(struct CheatSet*, struct GBA*);
// Adds a code as a libretro frontend gives it: lines of eight and four hex digits (CodeBreaker) or
// eight and eight (GameShark, Action Replay), split by spaces or '+'. Returns false if a line isn't
// a code.
bool cheatsAdd(struct CheatSet*, const char* code);
// Runs the codes once a frame, as it ends.
void cheatsApply(struct CheatSet*, struct GBA*);

#endif
