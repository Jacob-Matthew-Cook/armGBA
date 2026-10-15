/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_TESTS_ORACLE_H
#define ARMGBA_TESTS_ORACLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct FuzzState {
	uint32_t r[16];
	uint32_t cpsr;
	uint32_t spsr;
	uint32_t bank[6][7];
	uint32_t bankSpsr[6];
	uint32_t shifterCarry;
};

// One emulator behind a common face: memory pointers, and a run from a state to a cycle deadline
struct Emu {
	bool (*init)(const char* bios, uint8_t* rom, size_t romSize);
	uint8_t* (*memory)(uint32_t address);
	void (*codeChanged)(void);
	uint32_t (*run)(const struct FuzzState* in, uint32_t pc, uint16_t waitcnt, int32_t deadline, struct FuzzState* out);
};

extern const struct Emu mgbaEmu;
extern const struct Emu armgbaEmu;

#endif
