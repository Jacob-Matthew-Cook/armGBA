/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef ARMGBA_TESTS_GAME_H
#define ARMGBA_TESTS_GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Everything compared between the two emulators
struct Snapshot {
	uint32_t r[16];
	uint32_t cpsr;
	uint32_t spsr;
	uint32_t bank[6][7];
	uint32_t bankSpsr[6];
	uint32_t shifterCarry;
	uint16_t io[0x20C / 2];
	uint8_t iwram[0x8000];
	uint8_t ewram[0x40000];
	uint8_t palette[0x400];
	uint8_t vram[0x18000];
	uint8_t oam[0x400];
	// BGR555, as mGBA's 16-bit color builds draw it
	uint16_t frame[160][240];
};

struct GameEmu {
	const char* name;
	bool (*load)(const char* bios, const uint8_t* rom, size_t romSize, bool skipBios);
	void (*reset)(void);
	void (*runFrame)(void);
	// Runs to the first instruction boundary at least this many cycles from now
	void (*runCycles)(int32_t cycles);
	uint32_t (*now)(void);
	uint32_t (*frame)(void);
	void (*snapshot)(struct Snapshot*);
	void (*setKeys)(uint16_t);
	// Every reset starts from this save again
	void (*loadSave)(const uint8_t* data, size_t size);
	// The save chip, as mGBA's enum numbers it; mGBA only finds it on first access, armGBA from the ROM
	int (*saveType)(void);
	void (*forceSaveType)(int type);
	// The save chip's contents, for test ROMs that log there
	const uint8_t* (*saveData)(void);
	// A hash of all the sound and pictures put out so far, where the emulator keeps one
	uint32_t (*audioHash)(void);
};

extern const struct GameEmu mgbaGame;
extern const struct GameEmu armgbaGame;
// armGBA's machine run by tests/ref.c instead of the JIT
extern const struct GameEmu refGame;

#endif
