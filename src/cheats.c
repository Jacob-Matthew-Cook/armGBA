/* Copyright (c) 2013-2016 Jeffrey Pfau
 * Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "cheats.h"

#include "gba.h"
#include "cpu/jit.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// mGBA's cheat engine (core/cheats.c) and its GameShark, Action Replay v3 and CodeBreaker decoders
// (gba/cheats/), with one difference: a code naming a hook in the game is still applied as the
// frame ends, as mGBA applies codes without one.

#define COMPLETE ((size_t) -1)

enum CheatType {
	CHEAT_ASSIGN,
	CHEAT_ASSIGN_INDIRECT,
	CHEAT_AND,
	CHEAT_ADD,
	CHEAT_SUB,
	CHEAT_OR,
	CHEAT_IF_EQ,
	CHEAT_IF_NE,
	CHEAT_IF_LT,
	CHEAT_IF_GT,
	CHEAT_IF_LE,
	CHEAT_IF_GE,
	CHEAT_IF_ULT,
	CHEAT_IF_UGT,
	CHEAT_IF_AND,
	CHEAT_IF_LAND,
	CHEAT_IF_NAND,
	CHEAT_IF_BUTTON,
	CHEAT_NEVER,
};

enum {
	BASE_ROM0 = 0x08000000,
	BASE_IO = 0x04000000,
	OFFSET_MASK = 0x00FFFFFF,
};

// Which encryption the GameShark and Action Replay codes use, worked out from the first one.
enum GameSharkVersion {
	GS_NOT_SET,
	GS_GSAV1,
	GS_GSAV1_RAW,
	GS_PARV3,
	GS_PARV3_RAW,
};

enum CodeBreakerType {
	CB_GAME_ID = 0x0,
	CB_HOOK = 0x1,
	CB_OR_2 = 0x2,
	CB_ASSIGN_1 = 0x3,
	CB_FILL = 0x4,
	CB_FILL_LIST = 0x5,
	CB_AND_2 = 0x6,
	CB_IF_EQ = 0x7,
	CB_ASSIGN_2 = 0x8,
	CB_ENCRYPT = 0x9,
	CB_IF_NE = 0xA,
	CB_IF_GT = 0xB,
	CB_IF_LT = 0xC,
	CB_IF_SPECIAL = 0xD,
	CB_ADD_2 = 0xE,
	CB_IF_AND = 0xF,
};

enum GameSharkType {
	GSA_ASSIGN_1 = 0x0,
	GSA_ASSIGN_2 = 0x1,
	GSA_ASSIGN_4 = 0x2,
	GSA_ASSIGN_LIST = 0x3,
	GSA_PATCH = 0x6,
	GSA_BUTTON = 0x8,
	GSA_IF = 0xD,
	GSA_IF_RANGE = 0xE,
	GSA_HOOK = 0xF,
};

enum GameSharkIfType {
	GSA_IF_EQ = 0,
	GSA_IF_NE = 1,
	GSA_IF_LE = 2,
	GSA_IF_GE = 3,
};

enum {
	PAR3_COND_OTHER = 0x00000000,
	PAR3_COND_EQ = 0x08000000,
	PAR3_COND_NE = 0x10000000,
	PAR3_COND_LT = 0x18000000,
	PAR3_COND_GT = 0x20000000,
	PAR3_COND_ULT = 0x28000000,
	PAR3_COND_UGT = 0x30000000,
	PAR3_COND_AND = 0x38000000,

	PAR3_ACTION_NEXT = 0x00000000,
	PAR3_ACTION_NEXT_TWO = 0x40000000,
	PAR3_ACTION_BLOCK = 0x80000000,
	PAR3_ACTION_DISABLE = 0xC0000000,

	PAR3_BASE_ASSIGN = 0x00000000,
	PAR3_BASE_INDIRECT = 0x40000000,
	PAR3_BASE_ADD = 0x80000000,
	PAR3_BASE_OTHER = 0xC0000000,

	PAR3_OTHER_END = 0x00000000,
	PAR3_OTHER_SLOWDOWN = 0x08000000,
	PAR3_OTHER_BUTTON_1 = 0x10000000,
	PAR3_OTHER_BUTTON_2 = 0x12000000,
	PAR3_OTHER_BUTTON_4 = 0x14000000,
	PAR3_OTHER_PATCH_1 = 0x18000000,
	PAR3_OTHER_PATCH_2 = 0x1A000000,
	PAR3_OTHER_PATCH_3 = 0x1C000000,
	PAR3_OTHER_PATCH_4 = 0x1E000000,
	PAR3_OTHER_ENDIF = 0x40000000,
	PAR3_OTHER_ELSE = 0x60000000,
	PAR3_OTHER_FILL_1 = 0x80000000,
	PAR3_OTHER_FILL_2 = 0x82000000,
	PAR3_OTHER_FILL_4 = 0x84000000,

	PAR3_COND = 0x38000000,
	PAR3_WIDTH = 0x06000000,
	PAR3_ACTION = 0xC0000000,
	PAR3_BASE = 0xC0000000,
	PAR3_WIDTH_BASE = 25,
};

static const uint32_t gameSharkSeeds[4] = { 0x09F4FBBD, 0x9681884A, 0x352027E9, 0xF3DEE5A7 };
static const uint32_t actionReplaySeeds[4] = { 0x7AA9648F, 0x7FAE6994, 0xC0EFAAD5, 0x42712C57 };

// The tables a GameShark and an Action Replay reseed their keys from.
static const uint8_t gsa1T1[256] = {
	0x31, 0x1C, 0x23, 0xE5, 0x89, 0x8E, 0xA1, 0x37, 0x74, 0x6D, 0x67, 0xFC, 0x1F, 0xC0, 0xB1, 0x94,
	0x3B, 0x05, 0x56, 0x86, 0x00, 0x24, 0xF0, 0x17, 0x72, 0xA2, 0x3D, 0x1B, 0xE3, 0x17, 0xC5, 0x0B,
	0xB9, 0xE2, 0xBD, 0x58, 0x71, 0x1B, 0x2C, 0xFF, 0xE4, 0xC9, 0x4C, 0x5E, 0xC9, 0x55, 0x33, 0x45,
	0x7C, 0x3F, 0xB2, 0x51, 0xFE, 0x10, 0x7E, 0x75, 0x3C, 0x90, 0x8D, 0xDA, 0x94, 0x38, 0xC3, 0xE9,
	0x95, 0xEA, 0xCE, 0xA6, 0x06, 0xE0, 0x4F, 0x3F, 0x2A, 0xE3, 0x3A, 0xE4, 0x43, 0xBD, 0x7F, 0xDA,
	0x55, 0xF0, 0xEA, 0xCB, 0x2C, 0xA8, 0x47, 0x61, 0xA0, 0xEF, 0xCB, 0x13, 0x18, 0x20, 0xAF, 0x3E,
	0x4D, 0x9E, 0x1E, 0x77, 0x51, 0xC5, 0x51, 0x20, 0xCF, 0x21, 0xF9, 0x39, 0x94, 0xDE, 0xDD, 0x79,
	0x4E, 0x80, 0xC4, 0x9D, 0x94, 0xD5, 0x95, 0x01, 0x27, 0x27, 0xBD, 0x6D, 0x78, 0xB5, 0xD1, 0x31,
	0x6A, 0x65, 0x74, 0x74, 0x58, 0xB3, 0x7C, 0xC9, 0x5A, 0xED, 0x50, 0x03, 0xC4, 0xA2, 0x94, 0x4B,
	0xF0, 0x58, 0x09, 0x6F, 0x3E, 0x7D, 0xAE, 0x7D, 0x58, 0xA0, 0x2C, 0x91, 0xBB, 0xE1, 0x70, 0xEB,
	0x73, 0xA6, 0x9A, 0x44, 0x25, 0x90, 0x16, 0x62, 0x53, 0xAE, 0x08, 0xEB, 0xDC, 0xF0, 0xEE, 0x77,
	0xC2, 0xDE, 0x81, 0xE8, 0x30, 0x89, 0xDB, 0xFE, 0xBC, 0xC2, 0xDF, 0x26, 0xE9, 0x8B, 0xD6, 0x93,
	0xF0, 0xCB, 0x56, 0x90, 0xC0, 0x46, 0x68, 0x15, 0x43, 0xCB, 0xE9, 0x98, 0xE3, 0xAF, 0x31, 0x25,
	0x4D, 0x7B, 0xF3, 0xB1, 0x74, 0xE2, 0x64, 0xAC, 0xD9, 0xF6, 0xA0, 0xD5, 0x0B, 0x9B, 0x49, 0x52,
	0x69, 0x3B, 0x71, 0x00, 0x2F, 0xBB, 0xBA, 0x08, 0xB1, 0xAE, 0xBB, 0xB3, 0xE1, 0xC9, 0xA6, 0x7F,
	0x17, 0x97, 0x28, 0x72, 0x12, 0x6E, 0x91, 0xAE, 0x3A, 0xA2, 0x35, 0x46, 0x27, 0xF8, 0x12, 0x50,
};

static const uint8_t gsa1T2[256] = {
	0xD8, 0x65, 0x04, 0xC2, 0x65, 0xD5, 0xB0, 0x0C, 0xDF, 0x9D, 0xF0, 0xC3, 0x9A, 0x17, 0xC9, 0xA6,
	0xE1, 0xAC, 0x0D, 0x14, 0x2F, 0x3C, 0x2C, 0x87, 0xA2, 0xBF, 0x4D, 0x5F, 0xAC, 0x2D, 0x9D, 0xE1,
	0x0C, 0x9C, 0xE7, 0x7F, 0xFC, 0xA8, 0x66, 0x59, 0xAC, 0x18, 0xD7, 0x05, 0xF0, 0xBF, 0xD1, 0x8B,
	0x35, 0x9F, 0x59, 0xB4, 0xBA, 0x55, 0xB2, 0x85, 0xFD, 0xB1, 0x72, 0x06, 0x73, 0xA4, 0xDB, 0x48,
	0x7B, 0x5F, 0x67, 0xA5, 0x95, 0xB9, 0xA5, 0x4A, 0xCF, 0xD1, 0x44, 0xF3, 0x81, 0xF5, 0x6D, 0xF6,
	0x3A, 0xC3, 0x57, 0x83, 0xFA, 0x8E, 0x15, 0x2A, 0xA2, 0x04, 0xB2, 0x9D, 0xA8, 0x0D, 0x7F, 0xB8,
	0x0F, 0xF6, 0xAC, 0xBE, 0x97, 0xCE, 0x16, 0xE6, 0x31, 0x10, 0x60, 0x16, 0xB5, 0x83, 0x45, 0xEE,
	0xD7, 0x5F, 0x2C, 0x08, 0x58, 0xB1, 0xFD, 0x7E, 0x79, 0x00, 0x34, 0xAD, 0xB5, 0x31, 0x34, 0x39,
	0xAF, 0xA8, 0xDD, 0x52, 0x6A, 0xB0, 0x60, 0x35, 0xB8, 0x1D, 0x52, 0xF5, 0xF5, 0x30, 0x00, 0x7B,
	0xF4, 0xBA, 0x03, 0xCB, 0x3A, 0x84, 0x14, 0x8A, 0x6A, 0xEF, 0x21, 0xBD, 0x01, 0xD8, 0xA0, 0xD4,
	0x43, 0xBE, 0x23, 0xE7, 0x76, 0x27, 0x2C, 0x3F, 0x4D, 0x3F, 0x43, 0x18, 0xA7, 0xC3, 0x47, 0xA5,
	0x7A, 0x1D, 0x02, 0x55, 0x09, 0xD1, 0xFF, 0x55, 0x5E, 0x17, 0xA0, 0x56, 0xF4, 0xC9, 0x6B, 0x90,
	0xB4, 0x80, 0xA5, 0x07, 0x22, 0xFB, 0x22, 0x0D, 0xD9, 0xC0, 0x5B, 0x08, 0x35, 0x05, 0xC1, 0x75,
	0x4F, 0xD0, 0x51, 0x2D, 0x2E, 0x5E, 0x69, 0xE7, 0x3B, 0xC2, 0xDA, 0xFF, 0xF6, 0xCE, 0x3E, 0x76,
	0xE8, 0x36, 0x8C, 0x39, 0xD8, 0xF3, 0xE9, 0xA6, 0x42, 0xE6, 0xC1, 0x4C, 0x05, 0xBE, 0x17, 0xF2,
	0x5C, 0x1B, 0x19, 0xDB, 0x0F, 0xF3, 0xF8, 0x49, 0xEB, 0x36, 0xF6, 0x40, 0x6F, 0xAD, 0xC1, 0x8C,
};

static const uint8_t par3T1[256] = {
	0xD0, 0xFF, 0xBA, 0xE5, 0xC1, 0xC7, 0xDB, 0x5B, 0x16, 0xE3, 0x6E, 0x26, 0x62, 0x31, 0x2E, 0x2A,
	0xD1, 0xBB, 0x4A, 0xE6, 0xAE, 0x2F, 0x0A, 0x90, 0x29, 0x90, 0xB6, 0x67, 0x58, 0x2A, 0xB4, 0x45,
	0x7B, 0xCB, 0xF0, 0x73, 0x84, 0x30, 0x81, 0xC2, 0xD7, 0xBE, 0x89, 0xD7, 0x4E, 0x73, 0x5C, 0xC7,
	0x80, 0x1B, 0xE5, 0xE4, 0x43, 0xC7, 0x46, 0xD6, 0x6F, 0x7B, 0xBF, 0xED, 0xE5, 0x27, 0xD1, 0xB5,
	0xD0, 0xD8, 0xA3, 0xCB, 0x2B, 0x30, 0xA4, 0xF0, 0x84, 0x14, 0x72, 0x5C, 0xFF, 0xA4, 0xFB, 0x54,
	0x9D, 0x70, 0xE2, 0xFF, 0xBE, 0xE8, 0x24, 0x76, 0xE5, 0x15, 0xFB, 0x1A, 0xBC, 0x87, 0x02, 0x2A,
	0x58, 0x8F, 0x9A, 0x95, 0xBD, 0xAE, 0x8D, 0x0C, 0xA5, 0x4C, 0xF2, 0x5C, 0x7D, 0xAD, 0x51, 0xFB,
	0xB1, 0x22, 0x07, 0xE0, 0x29, 0x7C, 0xEB, 0x98, 0x14, 0xC6, 0x31, 0x97, 0xE4, 0x34, 0x8F, 0xCC,
	0x99, 0x56, 0x9F, 0x78, 0x43, 0x91, 0x85, 0x3F, 0xC2, 0xD0, 0xD1, 0x80, 0xD1, 0x77, 0xA7, 0xE2,
	0x43, 0x99, 0x1D, 0x2F, 0x8B, 0x6A, 0xE4, 0x66, 0x82, 0xF7, 0x2B, 0x0B, 0x65, 0x14, 0xC0, 0xC2,
	0x1D, 0x96, 0x78, 0x1C, 0xC4, 0xC3, 0xD2, 0xB1, 0x64, 0x07, 0xD7, 0x6F, 0x02, 0xE9, 0x44, 0x31,
	0xDB, 0x3C, 0xEB, 0x93, 0xED, 0x9A, 0x57, 0x05, 0xB9, 0x0E, 0xAF, 0x1F, 0x48, 0x11, 0xDC, 0x35,
	0x6C, 0xB8, 0xEE, 0x2A, 0x48, 0x2B, 0xBC, 0x89, 0x12, 0x59, 0xCB, 0xD1, 0x18, 0xEA, 0x72, 0x11,
	0x01, 0x75, 0x3B, 0xB5, 0x56, 0xF4, 0x8B, 0xA0, 0x41, 0x75, 0x86, 0x7B, 0x94, 0x12, 0x2D, 0x4C,
	0x0C, 0x22, 0xC9, 0x4A, 0xD8, 0xB1, 0x8D, 0xF0, 0x55, 0x2E, 0x77, 0x50, 0x1C, 0x64, 0x77, 0xAA,
	0x3E, 0xAC, 0xD3, 0x3D, 0xCE, 0x60, 0xCA, 0x5D, 0xA0, 0x92, 0x78, 0xC6, 0x51, 0xFE, 0xF9, 0x30,
};

static const uint8_t par3T2[256] = {
	0xAA, 0xAF, 0xF0, 0x72, 0x90, 0xF7, 0x71, 0x27, 0x06, 0x11, 0xEB, 0x9C, 0x37, 0x12, 0x72, 0xAA,
	0x65, 0xBC, 0x0D, 0x4A, 0x76, 0xF6, 0x5C, 0xAA, 0xB0, 0x7A, 0x7D, 0x81, 0xC1, 0xCE, 0x2F, 0x9F,
	0x02, 0x75, 0x38, 0xC8, 0xFC, 0x66, 0x05, 0xC2, 0x2C, 0xBD, 0x91, 0xAD, 0x03, 0xB1, 0x88, 0x93,
	0x31, 0xC6, 0xAB, 0x40, 0x23, 0x43, 0x76, 0x54, 0xCA, 0xE7, 0x00, 0x96, 0x9F, 0xD8, 0x24, 0x8B,
	0xE4, 0xDC, 0xDE, 0x48, 0x2C, 0xCB, 0xF7, 0x84, 0x1D, 0x45, 0xE5, 0xF1, 0x75, 0xA0, 0xED, 0xCD,
	0x4B, 0x24, 0x8A, 0xB3, 0x98, 0x7B, 0x12, 0xB8, 0xF5, 0x63, 0x97, 0xB3, 0xA6, 0xA6, 0x0B, 0xDC,
	0xD8, 0x4C, 0xA8, 0x99, 0x27, 0x0F, 0x8F, 0x94, 0x63, 0x0F, 0xB0, 0x11, 0x94, 0xC7, 0xE9, 0x7F,
	0x3B, 0x40, 0x72, 0x4C, 0xDB, 0x84, 0x78, 0xFE, 0xB8, 0x56, 0x08, 0x80, 0xDF, 0x20, 0x2F, 0xB9,
	0x66, 0x2D, 0x60, 0x63, 0xF5, 0x18, 0x15, 0x1B, 0x86, 0x85, 0xB9, 0xB4, 0x68, 0x0E, 0xC6, 0xD1,
	0x8A, 0x81, 0x2B, 0xB3, 0xF6, 0x48, 0xF0, 0x4F, 0x9C, 0x28, 0x1C, 0xA4, 0x51, 0x2F, 0xD7, 0x4B,
	0x17, 0xE7, 0xCC, 0x50, 0x9F, 0xD0, 0xD1, 0x40, 0x0C, 0x0D, 0xCA, 0x83, 0xFA, 0x5E, 0xCA, 0xEC,
	0xBF, 0x4E, 0x7C, 0x8F, 0xF0, 0xAE, 0xC2, 0xD3, 0x28, 0x41, 0x9B, 0xC8, 0x04, 0xB9, 0x4A, 0xBA,
	0x72, 0xE2, 0xB5, 0x06, 0x2C, 0x1E, 0x0B, 0x2C, 0x7F, 0x11, 0xA9, 0x26, 0x51, 0x9D, 0x3F, 0xF8,
	0x62, 0x11, 0x2E, 0x89, 0xD2, 0x9D, 0x35, 0xB1, 0xE4, 0x0A, 0x4D, 0x93, 0x01, 0xA7, 0xD1, 0x2D,
	0x00, 0x87, 0xE2, 0x2D, 0xA4, 0xE9, 0x0A, 0x06, 0x66, 0xF8, 0x1F, 0x44, 0x75, 0xB5, 0x6B, 0x1C,
	0xFC, 0x31, 0x09, 0x48, 0xA3, 0xFF, 0x92, 0x12, 0x58, 0xE9, 0xFA, 0xAE, 0x4F, 0xE2, 0xB4, 0xCC,
};

void cheatsInit(struct CheatSet* set) {
	memset(set, 0, sizeof(*set));
	set->incompleteCheat = COMPLETE;
	set->incompletePatch = COMPLETE;
	set->currentBlock = COMPLETE;
}

void cheatsClear(struct CheatSet* set, struct GBA* gba) {
	for (size_t i = set->patchCount; i-- > 0;) {
		struct CheatPatch* patch = &set->patches[i];
		if (patch->applied && gba) {
			uint32_t offset = patch->address & (SIZE_ROM - 1);
			memcpy(&gba->rom[offset], &patch->original, patch->width);
			jitInvalidate(gba, patch->address, patch->width);
		}
	}
	free(set->list);
	free(set->patches);
	cheatsInit(set);
}

// Where an operation or patch goes when the list can't grow: outside the list, so it is dropped.
static struct Cheat spareCheat;
static struct CheatPatch sparePatch;

// Adds a new operation, all zero, and returns where it is in the list.
static struct Cheat* append(struct CheatSet* set) {
	if (set->count == set->capacity) {
		size_t const capacity = set->capacity ? set->capacity * 2 : 16;
		struct Cheat* const grown = realloc(set->list, capacity * sizeof(*grown));
		if (!grown) {
			memset(&spareCheat, 0, sizeof(spareCheat));
			return &spareCheat;
		}
		set->list = grown;
		set->capacity = capacity;
	}
	struct Cheat* cheat = &set->list[set->count++];
	memset(cheat, 0, sizeof(*cheat));
	return cheat;
}

static size_t indexOf(struct CheatSet* set, struct Cheat* cheat) {
	return cheat == &spareCheat ? COMPLETE : (size_t) (cheat - set->list);
}

static struct CheatPatch* appendPatch(struct CheatSet* set) {
	if (set->patchCount == set->patchCapacity) {
		size_t const capacity = set->patchCapacity ? set->patchCapacity * 2 : 4;
		struct CheatPatch* const grown = realloc(set->patches, capacity * sizeof(*grown));
		if (!grown) {
			memset(&sparePatch, 0, sizeof(sparePatch));
			return &sparePatch;
		}
		set->patches = grown;
		set->patchCapacity = capacity;
	}
	struct CheatPatch* patch = &set->patches[set->patchCount++];
	memset(patch, 0, sizeof(*patch));
	return patch;
}

// Reads exactly digits hex digits; returns NULL if they aren't there.
static const char* hex(const char* line, int digits, uint32_t* out) {
	uint32_t value = 0;
	for (int i = 0; i < digits; ++i) {
		char c = line[i];
		int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
		if (digit < 0) {
			return NULL;
		}
		value = value << 4 | digit;
	}
	*out = value;
	return line + digits;
}

// Scores how likely an address is to be one a code would touch.
static int addressIsReal(uint32_t address) {
	switch (address >> 24) {
	case REGION_BIOS:
		return -0x80;
	case REGION_EWRAM:
		return (address & OFFSET_MASK) > SIZE_EWRAM ? -0x40 : 0x20;
	case REGION_IWRAM:
		return (address & OFFSET_MASK) > SIZE_IWRAM ? -0x40 : 0x20;
	case REGION_IO:
		return (address & OFFSET_MASK) > SIZE_IO ? -0x80 : 0x10;
	case REGION_OAM:
		return (address & OFFSET_MASK) > SIZE_OAM ? -0x80 : -0x8;
	case REGION_VRAM:
		return (address & OFFSET_MASK) > SIZE_VRAM ? -0x80 : -0x8;
	case REGION_PALETTE:
		return (address & OFFSET_MASK) > SIZE_PALETTE ? -0x80 : -0x8;
	case 0x8:
	case 0x9:
	case 0xA:
	case 0xB:
	case 0xC:
	case 0xD:
		return -0x8;
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		return (address & OFFSET_MASK) > SIZE_FLASH512 ? -0x80 : -0x8;
	default:
		return -0xC0;
	}
}

// GameShark and Action Replay: TEA (https://en.wikipedia.org/wiki/Tiny_Encryption_Algorithm).
static void decryptGameShark(uint32_t* op1, uint32_t* op2, const uint32_t* seeds) {
	uint32_t sum = 0xC6EF3720;
	for (int i = 0; i < 32; ++i) {
		*op2 -= ((*op1 << 4) + seeds[2]) ^ (*op1 + sum) ^ ((*op1 >> 5) + seeds[3]);
		*op1 -= ((*op2 << 4) + seeds[0]) ^ (*op2 + sum) ^ ((*op2 >> 5) + seeds[1]);
		sum -= 0x9E3779B9;
	}
}

static void reseedGameShark(uint32_t* seeds, uint16_t params, const uint8_t* t1, const uint8_t* t2) {
	int s0 = params >> 8;
	int s1 = params & 0xFF;
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			uint8_t z = t1[(s0 + x) & 0xFF] + t2[(s1 + y) & 0xFF];
			seeds[y] <<= 8;
			seeds[y] |= z;
		}
	}
}

static void setGameSharkVersion(struct CheatSet* set, enum GameSharkVersion version) {
	set->gsaVersion = version;
	switch (version) {
	case GS_GSAV1:
	case GS_GSAV1_RAW:
		memcpy(set->gsaSeeds, gameSharkSeeds, sizeof(set->gsaSeeds));
		break;
	case GS_PARV3:
	case GS_PARV3_RAW:
		memcpy(set->gsaSeeds, actionReplaySeeds, sizeof(set->gsaSeeds));
		break;
	default:
		break;
	}
}

static bool addGameSharkRaw(struct CheatSet* set, uint32_t op1, uint32_t op2) {
	enum GameSharkType type = op1 >> 28;
	struct Cheat* cheat = NULL;

	if (set->incompleteCheat != COMPLETE) {
		uint32_t operand = set->list[set->incompleteCheat].operand;
		if (set->remainingAddresses > 0) {
			cheat = append(set);
			cheat->type = CHEAT_ASSIGN;
			cheat->width = 4;
			cheat->address = op1;
			cheat->operand = operand;
			cheat->repeat = 1;
			--set->remainingAddresses;
		}
		if (set->remainingAddresses > 0) {
			cheat = append(set);
			cheat->type = CHEAT_ASSIGN;
			cheat->width = 4;
			cheat->address = op2;
			cheat->operand = operand;
			cheat->repeat = 1;
			--set->remainingAddresses;
		}
		if (set->remainingAddresses == 0) {
			set->incompleteCheat = COMPLETE;
		}
		return true;
	}

	switch (type) {
	case GSA_ASSIGN_1:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 1;
		cheat->address = op1 & 0x0FFFFFFF;
		break;
	case GSA_ASSIGN_2:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 2;
		cheat->address = op1 & 0x0FFFFFFF;
		break;
	case GSA_ASSIGN_4:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 4;
		cheat->address = op1 & 0x0FFFFFFF;
		break;
	case GSA_ASSIGN_LIST:
		set->remainingAddresses = (op1 & 0xFFFF) - 1;
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 4;
		cheat->address = op2;
		set->incompleteCheat = indexOf(set, cheat);
		break;
	case GSA_PATCH: {
		struct CheatPatch* patch = appendPatch(set);
		patch->address = BASE_ROM0 | ((op1 & 0xFFFFFF) << 1);
		patch->value = op2;
		patch->width = 2;
		return true;
	}
	case GSA_BUTTON:
		switch (op1 & 0x00F00000) {
		case 0x00100000:
			cheat = append(set);
			cheat->type = CHEAT_IF_BUTTON;
			cheat->repeat = 1;
			cheat = append(set);
			cheat->type = CHEAT_ASSIGN;
			cheat->width = 1;
			cheat->address = op1 & 0x0F0FFFFF;
			break;
		case 0x00200000:
			cheat = append(set);
			cheat->type = CHEAT_IF_BUTTON;
			cheat->repeat = 1;
			cheat = append(set);
			cheat->type = CHEAT_ASSIGN;
			cheat->width = 2;
			cheat->address = op1 & 0x0F0FFFFF;
			break;
		default:
			return false;
		}
		break;
	case GSA_IF:
		if (op1 == 0xDEADFACE) {
			reseedGameShark(set->gsaSeeds, op2, gsa1T1, gsa1T2);
			return true;
		}
		cheat = append(set);
		switch (op2 >> 20) {
		case GSA_IF_EQ:
			cheat->type = CHEAT_IF_EQ;
			break;
		case GSA_IF_NE:
			cheat->type = CHEAT_IF_NE;
			break;
		case GSA_IF_LE:
			cheat->type = CHEAT_IF_LE;
			break;
		case GSA_IF_GE:
			cheat->type = CHEAT_IF_GE;
			break;
		}
		cheat->width = 2;
		cheat->repeat = 1;
		cheat->address = op1 & 0x0FFFFFFF;
		cheat->operand = op2 & 0xFFFF;
		return true;
	case GSA_IF_RANGE:
		cheat = append(set);
		cheat->type = CHEAT_IF_EQ;
		cheat->width = 2;
		cheat->address = op2 & 0x0FFFFFFF;
		cheat->operand = op1 & 0xFFFF;
		cheat->repeat = (op1 >> 16) & 0xFF;
		return true;
	case GSA_HOOK:
		return true;
	default:
		return false;
	}
	cheat->operand = op2;
	cheat->repeat = 1;
	cheat->negativeRepeat = 0;
	return true;
}

static bool addGameShark(struct CheatSet* set, uint32_t op1, uint32_t op2) {
	switch (set->gsaVersion) {
	default:
		setGameSharkVersion(set, GS_GSAV1);
		// Fall through.
	case GS_GSAV1:
		decryptGameShark(&op1, &op2, set->gsaSeeds);
		// Fall through.
	case GS_GSAV1_RAW:
		return addGameSharkRaw(set, op1, op2);
	}
}

static int gameSharkProbability(uint32_t op1, uint32_t op2) {
	int probability = 0;
	if (op2 == 0x001DC0DE) {
		return 0x100;
	}
	uint32_t address = op1 & 0x0FFFFFFF;
	switch (op1 >> 28) {
	case GSA_ASSIGN_1:
		probability += 0x20;
		if (op2 & 0xFFFFFF00) {
			probability -= 0x10;
		}
		probability += addressIsReal(address);
		break;
	case GSA_ASSIGN_2:
		probability += 0x20;
		if (op2 & 0xFFFF0000) {
			probability -= 0x10;
		}
		probability += addressIsReal(address);
		break;
	case GSA_ASSIGN_4:
		probability += 0x20;
		probability += addressIsReal(address);
		break;
	case GSA_PATCH:
		probability += 0x20;
		if (op2 & 0xCFFF0000) {
			probability -= 0x10;
		}
		break;
	case GSA_BUTTON:
		probability += 0x10;
		break;
	case GSA_IF:
		probability += 0x20;
		if (op2 & 0xFFCF0000) {
			probability -= 0x10;
		}
		probability += addressIsReal(address);
		break;
	case GSA_IF_RANGE:
		probability += 0x20;
		probability += addressIsReal(op2);
		if (op1 & 0x0F000000) {
			probability -= 0x10;
		}
		break;
	case GSA_HOOK:
		probability += 0x20;
		if (op2 & 0xFFFF0000) {
			probability -= 0x10;
		}
		break;
	default:
		probability -= 0x40;
	}
	return probability;
}

// Action Replay v3.
static uint32_t parAddress(uint32_t x) {
	return (x & 0xFFFFF) | ((x << 4) & 0x0F000000);
}

static void parEndBlock(struct CheatSet* set) {
	size_t size = set->count - set->currentBlock - 1;
	struct Cheat* block = &set->list[set->currentBlock];
	if (block->repeat) {
		block->negativeRepeat = size - block->repeat;
	} else {
		block->repeat = size;
	}
	set->currentBlock = COMPLETE;
}

static void parElseBlock(struct CheatSet* set) {
	set->list[set->currentBlock].repeat = set->count - set->currentBlock - 1;
}

static bool addParCondition(struct CheatSet* set, uint32_t op1, uint32_t op2) {
	uint32_t condition = op1 & PAR3_COND;
	int width = 1 << ((op1 & PAR3_WIDTH) >> PAR3_WIDTH_BASE);
	if ((op1 & PAR3_ACTION) == PAR3_ACTION_DISABLE) {
		// Codes that disable others aren't supported, as in mGBA.
		return false;
	}

	struct Cheat* cheat = append(set);
	cheat->address = parAddress(op1);
	cheat->width = width;
	cheat->operand = op2 & (0xFFFFFFFFU >> ((4 - width) * 8));

	switch (op1 & PAR3_ACTION) {
	case PAR3_ACTION_NEXT:
		cheat->repeat = 1;
		break;
	case PAR3_ACTION_NEXT_TWO:
		cheat->repeat = 2;
		break;
	case PAR3_ACTION_BLOCK:
		cheat->repeat = 0;
		if (set->currentBlock != COMPLETE) {
			parEndBlock(set);
		}
		set->currentBlock = indexOf(set, cheat);
		break;
	}

	switch (condition) {
	case PAR3_COND_OTHER:
		cheat->type = CHEAT_IF_LAND;
		cheat->operand = 0;
		break;
	case PAR3_COND_EQ:
		cheat->type = CHEAT_IF_EQ;
		break;
	case PAR3_COND_NE:
		cheat->type = CHEAT_IF_NE;
		break;
	case PAR3_COND_LT:
		cheat->type = CHEAT_IF_LT;
		break;
	case PAR3_COND_GT:
		cheat->type = CHEAT_IF_GT;
		break;
	case PAR3_COND_ULT:
		cheat->type = CHEAT_IF_ULT;
		break;
	case PAR3_COND_UGT:
		cheat->type = CHEAT_IF_UGT;
		break;
	case PAR3_COND_AND:
		cheat->type = CHEAT_IF_AND;
		break;
	}

	if (width > 4) {
		cheat->width = 0;
		cheat->type = CHEAT_NEVER;
	}
	return true;
}

static bool addParSpecial(struct CheatSet* set, uint32_t op2) {
	bool romPatch = false;
	struct Cheat* cheat;
	switch (op2 & 0xFF000000) {
	case PAR3_OTHER_SLOWDOWN:
		// Not supported, as in mGBA.
		return false;
	case PAR3_OTHER_BUTTON_1:
	case PAR3_OTHER_BUTTON_2:
	case PAR3_OTHER_BUTTON_4:
		cheat = append(set);
		cheat->type = CHEAT_IF_BUTTON;
		cheat->repeat = 1;
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 1 << ((op2 >> 25) & 3);
		cheat->repeat = 1;
		cheat->address = parAddress(op2);
		set->incompleteCheat = indexOf(set, cheat);
		break;
	case PAR3_OTHER_PATCH_1:
	case PAR3_OTHER_PATCH_2:
	case PAR3_OTHER_PATCH_3:
	case PAR3_OTHER_PATCH_4:
		romPatch = true;
		break;
	case PAR3_OTHER_ENDIF:
		if (set->currentBlock != COMPLETE) {
			parEndBlock(set);
			return true;
		}
		return false;
	case PAR3_OTHER_ELSE:
		if (set->currentBlock != COMPLETE) {
			parElseBlock(set);
			return true;
		}
		return false;
	case PAR3_OTHER_FILL_1:
	case PAR3_OTHER_FILL_2:
	case PAR3_OTHER_FILL_4:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->address = parAddress(op2);
		cheat->width = 1 << ((op2 >> 25) & 3);
		set->incompleteCheat = indexOf(set, cheat);
		break;
	}
	if (romPatch) {
		struct CheatPatch* patch = appendPatch(set);
		patch->address = BASE_ROM0 | ((op2 & 0xFFFFFF) << 1);
		patch->width = 2;
		set->incompletePatch = patch == &sparePatch ? COMPLETE : set->patchCount - 1;
	}
	return true;
}

static bool addActionReplayRaw(struct CheatSet* set, uint32_t op1, uint32_t op2) {
	if (set->incompletePatch != COMPLETE) {
		set->patches[set->incompletePatch].value = op1;
		set->incompletePatch = COMPLETE;
		return true;
	}
	if (set->incompleteCheat != COMPLETE) {
		struct Cheat* incomplete = &set->list[set->incompleteCheat];
		incomplete->operand = op1 & (0xFFFFFFFFU >> ((4 - incomplete->width) * 8));
		if (set->incompleteCheat > 0 && set->list[set->incompleteCheat - 1].type == CHEAT_IF_BUTTON) {
			set->incompleteCheat = COMPLETE;
			return true;
		}
		incomplete->operandOffset = op2 >> 24;
		incomplete->repeat = (op2 >> 16) & 0xFF;
		incomplete->addressOffset = (op2 & 0xFFFF) * incomplete->width;
		set->incompleteCheat = COMPLETE;
		return true;
	}

	if (op2 == 0x001DC0DE) {
		return true;
	}

	switch (op1) {
	case 0x00000000:
		return addParSpecial(set, op2);
	case 0xDEADFACE:
		reseedGameShark(set->gsaSeeds, op2, par3T1, par3T2);
		return true;
	}

	if (op1 >> 24 == 0xC4) {
		// The hook a master code names.
		return true;
	}

	if (op1 & PAR3_COND) {
		return addParCondition(set, op1, op2);
	}

	int width = 1 << ((op1 & PAR3_WIDTH) >> PAR3_WIDTH_BASE);
	struct Cheat* cheat = append(set);
	cheat->address = parAddress(op1);
	cheat->repeat = 1;

	switch (op1 & PAR3_BASE) {
	case PAR3_BASE_ASSIGN:
		cheat->type = CHEAT_ASSIGN;
		cheat->addressOffset = width;
		if (width < 4) {
			cheat->repeat = (op2 >> (width * 8)) + 1;
		}
		break;
	case PAR3_BASE_INDIRECT:
		cheat->type = CHEAT_ASSIGN_INDIRECT;
		if (width < 4) {
			cheat->addressOffset = (op2 >> (width * 8)) * width;
		}
		break;
	case PAR3_BASE_ADD:
		cheat->type = CHEAT_ADD;
		break;
	case PAR3_BASE_OTHER:
		width = ((op1 >> 24) & 1) + 1;
		cheat->type = CHEAT_ASSIGN;
		cheat->address = BASE_IO | (op1 & OFFSET_MASK);
		break;
	}
	if (op1 & 0x01000000 && (op1 & 0xFE000000) != 0xC6000000) {
		return false;
	}

	cheat->width = width;
	cheat->operand = op2 & (0xFFFFFFFFU >> ((4 - width) * 8));
	return true;
}

static bool addActionReplay(struct CheatSet* set, uint32_t op1, uint32_t op2) {
	switch (set->gsaVersion) {
	default:
		setGameSharkVersion(set, GS_PARV3);
		// Fall through.
	case GS_PARV3:
		decryptGameShark(&op1, &op2, set->gsaSeeds);
		// Fall through.
	case GS_PARV3_RAW:
		return addActionReplayRaw(set, op1, op2);
	}
}

static int actionReplayProbability(uint32_t op1, uint32_t op2) {
	int probability = 0;
	if (op2 == 0x001DC0DE) {
		return 0x100;
	}
	if (op1 == 0xDEADFACE && !(op2 & 0xFFFF0000)) {
		return 0x100;
	}
	if (!op1) {
		probability += 0x40;
		uint32_t address = parAddress(op2);
		switch (op2 & 0xFE000000) {
		case PAR3_OTHER_FILL_1:
		case PAR3_OTHER_FILL_2:
		case PAR3_OTHER_FILL_4:
			probability += addressIsReal(address);
			break;
		case PAR3_OTHER_PATCH_1:
		case PAR3_OTHER_PATCH_2:
		case PAR3_OTHER_PATCH_3:
		case PAR3_OTHER_PATCH_4:
			break;
		case PAR3_OTHER_END:
		case PAR3_OTHER_SLOWDOWN:
		case PAR3_OTHER_BUTTON_1:
		case PAR3_OTHER_BUTTON_2:
		case PAR3_OTHER_BUTTON_4:
		case PAR3_OTHER_ENDIF:
		case PAR3_OTHER_ELSE:
			if (op2 & 0x01000000) {
				probability -= 0x40;
			}
			break;
		default:
			probability -= 0x40;
			break;
		}
		return probability;
	}
	int width = ((op1 & PAR3_WIDTH) >> (PAR3_WIDTH_BASE - 3));
	if (op1 & PAR3_COND) {
		probability += 0x20;
		if (width >= 24) {
			return 0;
		}
		if (op2 & ~((1 << width) - 1)) {
			probability -= 0x10;
		}
	} else {
		uint32_t address = parAddress(op1);
		probability += 0x20;
		switch (op1 & PAR3_BASE) {
		case PAR3_BASE_ADD:
			if (op2 & ~((1 << width) - 1)) {
				probability -= 0x10;
			}
			// Fall through.
		case PAR3_BASE_ASSIGN:
		case PAR3_BASE_INDIRECT:
			probability += addressIsReal(address);
			if (op1 & 0x01000000) {
				return 0;
			}
			break;
		case PAR3_BASE_OTHER:
			break;
		}
	}
	return probability;
}

// CodeBreaker, whose codes may be encrypted with keys a type 9 code gives.
static void cbLoadByteswap(uint8_t* buffer, uint32_t op1, uint16_t op2) {
	buffer[0] = op1 >> 24;
	buffer[1] = op1 >> 16;
	buffer[2] = op1 >> 8;
	buffer[3] = op1;
	buffer[4] = op2 >> 8;
	buffer[5] = op2;
}

static void cbStoreByteswap(uint8_t* buffer, uint32_t* op1, uint16_t* op2) {
	*op1 = (uint32_t) buffer[0] << 24 | buffer[1] << 16 | buffer[2] << 8 | buffer[3];
	*op2 = buffer[4] << 8 | buffer[5];
}

static void cbDecrypt(struct CheatSet* set, uint32_t* op1, uint16_t* op2) {
	uint8_t buffer[6];

	cbLoadByteswap(buffer, *op1, *op2);
	for (int i = sizeof(set->cbTable) - 1; i >= 0; --i) {
		size_t offsetX = i >> 3;
		size_t offsetY = set->cbTable[i] >> 3;
		int bitX = i & 7;
		int bitY = set->cbTable[i] & 7;

		uint8_t x = (buffer[offsetX] >> bitX) & 1;
		uint8_t y = (buffer[offsetY] >> bitY) & 1;
		uint8_t x2 = buffer[offsetX] & ~(1 << bitX);
		if (y) {
			x2 |= 1 << bitX;
		}
		buffer[offsetX] = x2;

		// After the store above, the two bits may be in the same byte.
		uint8_t y2 = buffer[offsetY] & ~(1 << bitY);
		if (x) {
			y2 |= 1 << bitY;
		}
		buffer[offsetY] = y2;
	}
	cbStoreByteswap(buffer, op1, op2);

	*op1 ^= set->cbSeeds[0];
	*op2 ^= set->cbSeeds[1];

	cbLoadByteswap(buffer, *op1, *op2);
	uint32_t master = set->cbMaster;
	for (int i = 0; i < 5; ++i) {
		buffer[i] ^= (master >> 8) ^ buffer[i + 1];
	}
	buffer[5] ^= master >> 8;

	for (int i = 5; i > 0; --i) {
		buffer[i] ^= master ^ buffer[i - 1];
	}
	buffer[0] ^= master;
	cbStoreByteswap(buffer, op1, op2);

	*op1 ^= set->cbSeeds[2];
	*op2 ^= set->cbSeeds[3];
}

static uint32_t cbRand(struct CheatSet* set) {
	// The LCG is rolled three times for enough bits.
	uint32_t roll = set->cbRngState * 0x41C64E6D + 0x3039;
	uint32_t roll2 = roll * 0x41C64E6D + 0x3039;
	uint32_t roll3 = roll2 * 0x41C64E6D + 0x3039;
	uint32_t mix = (roll << 14) & 0xC0000000;
	mix |= (roll2 >> 1) & 0x3FFF8000;
	mix |= (roll3 >> 16) & 0x7FFF;
	set->cbRngState = roll3;
	return mix;
}

static uint32_t ror(uint32_t value, int bits) {
	return value >> bits | value << (32 - bits);
}

static size_t cbSwapIndex(struct CheatSet* set) {
	uint32_t roll = cbRand(set);
	uint32_t count = sizeof(set->cbTable);

	if (roll == count) {
		roll = 0;
	}

	if (roll < count) {
		return roll;
	}

	uint32_t bit = 1;

	while (count < 0x10000000 && count < roll) {
		count <<= 4;
		bit <<= 4;
	}

	while (count < 0x80000000 && count < roll) {
		count <<= 1;
		bit <<= 1;
	}

	uint32_t mask;
	while (true) {
		mask = 0;
		if (roll >= count) {
			roll -= count;
		}
		if (roll >= count >> 1) {
			roll -= count >> 1;
			mask |= ror(bit, 1);
		}
		if (roll >= count >> 2) {
			roll -= count >> 2;
			mask |= ror(bit, 2);
		}
		if (roll >= count >> 3) {
			roll -= count >> 3;
			mask |= ror(bit, 3);
		}
		if (!roll || !(bit >> 4)) {
			break;
		}
		bit >>= 4;
		count >>= 4;
	}

	mask &= 0xE0000000;
	if (!mask || !(bit & 7)) {
		return roll;
	}

	if (mask & ror(bit, 3)) {
		roll += count >> 3;
	}
	if (mask & ror(bit, 2)) {
		roll += count >> 2;
	}
	if (mask & ror(bit, 1)) {
		roll += count >> 1;
	}

	return roll;
}

static void cbReseed(struct CheatSet* set, uint32_t op1, uint16_t op2) {
	set->cbRngState = (op2 & 0xFF) ^ 0x1111;
	for (size_t i = 0; i < sizeof(set->cbTable); ++i) {
		set->cbTable[i] = i;
	}
	// Entries swapped as the code says.
	for (size_t i = 0; i < 0x50; ++i) {
		size_t x = cbSwapIndex(set);
		size_t y = cbSwapIndex(set);
		uint8_t swap = set->cbTable[x];
		set->cbTable[x] = set->cbTable[y];
		set->cbTable[y] = swap;
	}

	set->cbRngState = 0x4EFAD1C3;
	for (size_t i = 0; i < ((op1 >> 24) & 0xF); ++i) {
		set->cbRngState = cbRand(set);
	}

	set->cbSeeds[2] = cbRand(set);
	set->cbSeeds[3] = cbRand(set);

	set->cbRngState = (op2 >> 8) ^ 0xF254;
	for (size_t i = 0; i < (size_t) (op2 >> 8); ++i) {
		set->cbRngState = cbRand(set);
	}

	set->cbSeeds[0] = cbRand(set);
	set->cbSeeds[1] = cbRand(set);

	set->cbMaster = op1;
}

static bool addCodeBreaker(struct CheatSet* set, uint32_t op1, uint16_t op2) {
	if (set->cbMaster) {
		cbDecrypt(set, &op1, &op2);
	}

	enum CodeBreakerType type = op1 >> 28;
	struct Cheat* cheat = NULL;

	if (set->incompleteCheat != COMPLETE) {
		struct Cheat* incomplete = &set->list[set->incompleteCheat];
		if (set->remainingAddresses) {
			unsigned halfword = op1 >> 16;
			incomplete->operand = ((halfword >> 8) | (halfword << 8)) & 0xFFFF;
			--set->remainingAddresses;

			uint32_t address = incomplete->address;
			unsigned rest[2] = { op1 & 0xFFFF, op2 };
			for (int i = 0; i < 2 && set->remainingAddresses; ++i) {
				address += 2;
				cheat = append(set);
				cheat->type = CHEAT_ASSIGN;
				cheat->width = 2;
				cheat->address = address;
				cheat->operand = ((rest[i] >> 8) | (rest[i] << 8)) & 0xFFFF;
				cheat->repeat = 1;
				--set->remainingAddresses;
			}

			if (!set->remainingAddresses) {
				set->incompleteCheat = COMPLETE;
			} else {
				cheat = append(set);
				cheat->type = CHEAT_ASSIGN;
				cheat->width = 2;
				cheat->address = address + 2;
				cheat->repeat = 1;
				set->incompleteCheat = indexOf(set, cheat);
			}
		} else {
			incomplete->repeat = op1 & 0xFFFF;
			incomplete->addressOffset = op2;
			incomplete->operandOffset = op1 >> 16;
			set->incompleteCheat = COMPLETE;
		}
		return true;
	}

	switch (type) {
	case CB_GAME_ID:
	case CB_HOOK:
		return true;
	case CB_OR_2:
		cheat = append(set);
		cheat->type = CHEAT_OR;
		cheat->width = 2;
		break;
	case CB_ASSIGN_1:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 1;
		break;
	case CB_FILL:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 2;
		set->incompleteCheat = indexOf(set, cheat);
		break;
	case CB_FILL_LIST:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 2;
		set->incompleteCheat = indexOf(set, cheat);
		set->remainingAddresses = op2;
		break;
	case CB_AND_2:
		cheat = append(set);
		cheat->type = CHEAT_AND;
		cheat->width = 2;
		break;
	case CB_IF_EQ:
		cheat = append(set);
		cheat->type = CHEAT_IF_EQ;
		cheat->width = 2;
		break;
	case CB_ASSIGN_2:
		cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = 2;
		break;
	case CB_ENCRYPT:
		cbReseed(set, op1, op2);
		return true;
	case CB_IF_NE:
		cheat = append(set);
		cheat->type = CHEAT_IF_NE;
		cheat->width = 2;
		break;
	case CB_IF_GT:
		cheat = append(set);
		cheat->type = CHEAT_IF_GT;
		cheat->width = 2;
		break;
	case CB_IF_LT:
		cheat = append(set);
		cheat->type = CHEAT_IF_LT;
		cheat->width = 2;
		break;
	case CB_IF_SPECIAL:
		// Only "if these buttons are held", read from KEYINPUT, as in mGBA.
		if ((op1 & 0x0FFFFFFF) != 0x20) {
			return false;
		}
		cheat = append(set);
		cheat->type = CHEAT_IF_NAND;
		cheat->width = 2;
		cheat->address = BASE_IO | REG_KEYINPUT;
		cheat->operand = op2;
		cheat->repeat = 1;
		return true;
	case CB_ADD_2:
		cheat = append(set);
		cheat->type = CHEAT_ADD;
		cheat->width = 2;
		break;
	case CB_IF_AND:
		cheat = append(set);
		cheat->type = CHEAT_IF_AND;
		cheat->width = 2;
		break;
	default:
		return false;
	}

	cheat->address = op1 & 0x0FFFFFFF;
	cheat->operand = op2;
	cheat->repeat = 1;
	cheat->negativeRepeat = 0;
	return true;
}

// An eight-by-eight line: GameShark or Action Replay, encrypted or not, whichever the first such
// line seems most likely to be.
static bool addAutodetect(struct CheatSet* set, uint32_t op1, uint32_t op2) {
	if (set->gsaVersion != GS_NOT_SET) {
		return set->gsaVersion < GS_PARV3 ? addGameShark(set, op1, op2) : addActionReplay(set, op1, op2);
	}
	int best = INT_MIN;
	int probability;
	uint32_t o1 = op1;
	uint32_t o2 = op2;
	decryptGameShark(&o1, &o2, gameSharkSeeds);
	probability = gameSharkProbability(o1, o2);
	if (probability > best) {
		best = probability;
		setGameSharkVersion(set, GS_GSAV1);
	}

	o1 = op1;
	o2 = op2;
	decryptGameShark(&o1, &o2, actionReplaySeeds);
	probability = actionReplayProbability(o1, o2);
	if (probability > best) {
		best = probability;
		setGameSharkVersion(set, GS_PARV3);
	}

	probability = gameSharkProbability(op1, op2);
	if (probability > best) {
		best = probability;
		setGameSharkVersion(set, GS_GSAV1_RAW);
	}

	probability = actionReplayProbability(op1, op2);
	if (probability > best) {
		best = probability;
		setGameSharkVersion(set, GS_PARV3_RAW);
	}

	return set->gsaVersion < GS_PARV3 ? addGameShark(set, op1, op2) : addActionReplay(set, op1, op2);
}

// VBA's "address:value", the value one, two or four bytes: a store, or a ROM patch.
static bool addVba(struct CheatSet* set, uint32_t address, const char* line) {
	uint32_t value = 0;
	int width = 0;
	uint32_t byte;
	while (width < 4 && (line = hex(line, 2, &byte))) {
		value = value << 8 | byte;
		++width;
	}
	if (width == 0 || width == 3) {
		return false;
	}
	if (address < BASE_ROM0 || address >= 0x0E000000) {
		struct Cheat* cheat = append(set);
		cheat->type = CHEAT_ASSIGN;
		cheat->width = width;
		cheat->address = address;
		cheat->operand = value;
		cheat->repeat = 1;
	} else {
		struct CheatPatch* patch = appendPatch(set);
		patch->address = address;
		patch->value = value;
		patch->width = width;
	}
	return true;
}

// "XXXXXXXX XXXX" is CodeBreaker; "XXXXXXXX XXXXXXXX" is GameShark or Action Replay.
static bool addLine(struct CheatSet* set, const char* line) {
	uint32_t op1;
	uint32_t op2;
	uint32_t op3;
	line = hex(line, 8, &op1);
	if (!line) {
		return false;
	}
	if (*line == ':') {
		// VBA's form when the address is somewhere a GBA has memory; otherwise a CodeBreaker code
		// written with a colon.
		if (op1 >> 24 >= REGION_EWRAM && op1 >> 24 <= 0xD) {
			return addVba(set, op1, line + 1);
		}
		++line;
	}
	while (isspace((unsigned char) *line)) {
		++line;
	}
	line = hex(line, 4, &op2);
	if (!line) {
		return false;
	}
	if (!*line || isspace((unsigned char) *line)) {
		return addCodeBreaker(set, op1, op2);
	}
	line = hex(line, 4, &op3);
	if (!line) {
		return false;
	}
	return addAutodetect(set, op1, op2 << 16 | op3);
}

// A frontend's code is split into lines as mGBA's libretro core splits it: thirteen characters for
// a CodeBreaker line, seventeen for the others.
bool cheatsAdd(struct CheatSet* set, const char* code) {
	char line[18];
	size_t length = strlen(code) + 1;
	bool added = true;
	for (size_t i = 0, pos = 0; i < length; ++i) {
		line[pos] = isspace((unsigned char) code[i]) || code[i] == '+' ? ' ' : code[i];
		if ((pos == 13 && (line[pos] == ' ' || !line[pos])) || pos == 17) {
			line[pos] = '\0';
			added = addLine(set, line) && added;
			pos = 0;
			continue;
		}
		++pos;
	}
	return added;
}

static uint32_t readMemory(struct GBA* gba, uint32_t address, int width) {
	switch (width) {
	case 1:
		return memoryView32(gba, address & ~3u) >> ((address & 3) * 8) & 0xFF;
	case 2:
		return memoryView16(gba, address & ~1u);
	case 4:
		return memoryView32(gba, address & ~3u);
	}
	return 0;
}

// Makes a store as the CPU would, compiled code and all, but taking no time.
static void writeMemory(struct GBA* gba, uint32_t address, int width, uint32_t value) {
	int cycles = 0;
	switch (width) {
	case 1:
		memoryStore8(gba, address, value, &cycles);
		break;
	case 2:
		memoryStore16(gba, address, value, &cycles);
		break;
	case 4:
		memoryStore32(gba, address, value, &cycles);
		break;
	}
}

static void patchRom(struct CheatSet* set, struct GBA* gba) {
	for (size_t i = 0; i < set->patchCount; ++i) {
		struct CheatPatch* patch = &set->patches[i];
		uint32_t offset = patch->address & (SIZE_ROM - 1);
		if (patch->applied || offset + patch->width > gba->romSize) {
			continue;
		}
		memcpy(&patch->original, &gba->rom[offset], patch->width);
		memcpy(&gba->rom[offset], &patch->value, patch->width);
		patch->applied = true;
		jitInvalidate(gba, patch->address, patch->width);
	}
}

void cheatsApply(struct CheatSet* set, struct GBA* gba) {
	patchRom(set, gba);

	size_t elseLoc = 0;
	size_t endLoc = 0;
	for (size_t i = 0; i < set->count; ++i) {
		struct Cheat* cheat = &set->list[i];
		int32_t value = 0;
		int32_t operand = cheat->operand;
		uint32_t operationsRemaining = cheat->repeat;
		uint32_t address = cheat->address;
		bool performAssignment = false;
		bool condition = true;
		int conditionRemaining = 0;
		int negativeConditionRemaining = 0;

		for (; operationsRemaining; --operationsRemaining) {
			bool test = false;
			int32_t read = 0;
			switch (cheat->type) {
			case CHEAT_ASSIGN:
				value = operand;
				performAssignment = true;
				break;
			case CHEAT_ASSIGN_INDIRECT:
				value = operand;
				address = readMemory(gba, address, 4) + cheat->addressOffset;
				performAssignment = true;
				break;
			case CHEAT_AND:
				value = readMemory(gba, address, cheat->width) & operand;
				performAssignment = true;
				break;
			case CHEAT_ADD:
				value = readMemory(gba, address, cheat->width) + operand;
				performAssignment = true;
				break;
			case CHEAT_SUB:
				value = readMemory(gba, address, cheat->width) - operand;
				performAssignment = true;
				break;
			case CHEAT_OR:
				value = readMemory(gba, address, cheat->width) | operand;
				performAssignment = true;
				break;
			default:
				test = true;
				read = cheat->type == CHEAT_IF_BUTTON || cheat->type == CHEAT_NEVER ? 0 : (int32_t) readMemory(gba, address, cheat->width);
				break;
			}
			if (test) {
				switch (cheat->type) {
				case CHEAT_IF_EQ:
					condition = read == operand;
					break;
				case CHEAT_IF_NE:
					condition = read != operand;
					break;
				case CHEAT_IF_LT:
					condition = read < operand;
					break;
				case CHEAT_IF_GT:
					condition = read > operand;
					break;
				case CHEAT_IF_LE:
					condition = read <= operand;
					break;
				case CHEAT_IF_GE:
					condition = read >= operand;
					break;
				case CHEAT_IF_ULT:
					condition = (uint32_t) read < (uint32_t) operand;
					break;
				case CHEAT_IF_UGT:
					condition = (uint32_t) read > (uint32_t) operand;
					break;
				case CHEAT_IF_AND:
					condition = read & operand;
					break;
				case CHEAT_IF_LAND:
					condition = read && operand;
					break;
				case CHEAT_IF_NAND:
					condition = !(read & operand);
					break;
				// There's no button for codes to wait on, as in mGBA's libretro core.
				case CHEAT_IF_BUTTON:
				case CHEAT_NEVER:
					condition = false;
					break;
				}
				conditionRemaining = cheat->repeat;
				negativeConditionRemaining = cheat->negativeRepeat;
				operationsRemaining = 1;
			}

			if (performAssignment) {
				writeMemory(gba, address, cheat->width, value);
			}

			address += cheat->addressOffset;
			operand += cheat->operandOffset;
		}

		if (elseLoc && i == elseLoc) {
			i = endLoc;
			endLoc = 0;
		}
		if (conditionRemaining > 0 && !condition) {
			i += conditionRemaining;
		} else if (negativeConditionRemaining > 0) {
			elseLoc = i + conditionRemaining;
			endLoc = elseLoc + negativeConditionRemaining;
		}
	}
}
