/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "game.h"

#include "gba.h"
#include "jit.h"
#include "mgba-state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// armGBA behind the test tools' common interface (game.h): load, run frames, take snapshots.

// game-ref.c builds this file again around the reference interpreter.
#ifndef GAME
#define GAME armgbaGame
// The name results are printed under; scripts look for it.
#define GAME_NAME "armGBA"
#define RUN_LOOP jitRunLoop
#define AFTER_RESET(gba)
#define TRACE_ENV "ARMGBA_TRACE"
#endif

static struct GBA* gba;
static bool stopped;
static bool skip;
static uint16_t keys;
static uint8_t* save;
static size_t saveSize;
static uint32_t audioHash = 2166136261u;
static bool hashAudio;

static void hashBytes(const void* data, size_t size) {
	const uint8_t* bytes = data;
	for (size_t i = 0; i < size; ++i) {
		audioHash = (audioHash ^ bytes[i]) * 16777619u;
	}
}

// The samples the mixer put out since the last call, hashed in when BENCH_HASH asks for it.
// Intervals finish when something asks, so every interval done by now is taken: builds that ask at
// other times split the same stream the same way.
static void takeAudio(void) {
	audioCatchUp(gba, timingCurrentTime(gba), false);
	if (hashAudio) {
		hashBytes(gba->audio.out, gba->audio.outCount * sizeof(*gba->audio.out));
	}
	gba->audio.outCount = 0;
}

static uint32_t audioOut(void) {
	takeAudio();
	return audioHash;
}

static int64_t fixedTime(void* context) {
	return 1700000000;
}

static void reset(void) {
	gba->gpio.clock = fixedTime;
	if (save) {
		memcpy(gba->savedata.storage, save, saveSize);
	}
	gbaReset(gba, skip);
	AFTER_RESET(gba);
	// MGBA_STATE=<file> continues from an mGBA save state of the game.
	size_t stateSize;
	const uint8_t* state = mgbaStateFile(&stateSize);
	if (state && !mgbaStateLoad(gba, state, stateSize)) {
		fprintf(stderr, "MGBA_STATE isn't an mGBA GBA state\n");
	}
	audioHash = 2166136261u;
	hashAudio = getenv("BENCH_HASH");
	// Only the first run is traced, not one gamecmp makes again to find a difference.
	static bool traced;
	if (getenv(TRACE_ENV) && !traced) {
		traced = true;
		stepTraceOpen(gba, getenv(TRACE_ENV));
	}
}

static bool load(const char* bios, const uint8_t* rom, size_t romSize, bool skipBios) {
	skip = skipBios;
	gba = gbaCreate();
	if (!gba) {
		return false;
	}
	FILE* f = fopen(bios, "rb");
	uint8_t data[SIZE_BIOS];
	if (!f || fread(data, 1, sizeof(data), f) != sizeof(data)) {
		return false;
	}
	fclose(f);
	gbaLoadBios(gba, data, sizeof(data));
	gbaLoadRom(gba, rom, romSize);
	reset();
	// ARMGBA_THREADED: the renderer on a second thread; 2 shows frames one frame late.
	if (getenv("ARMGBA_THREADED")) {
		renderThreadStart(gba, atoi(getenv("ARMGBA_THREADED")) == 2);
	}
	return true;
}

static void runFrame(void) {
	gbaSetKeys(gba, keys);
	uint32_t frame = gba->video.frameCounter;
	while (gba->video.frameCounter == frame) {
		RUN_LOOP(gba);
	}
	uint32_t before = audioHash;
	takeAudio();
	if (hashAudio) {
		// Every frame's picture too.
		uint64_t h = 0;
		const uint64_t* words = (const uint64_t*) gba->renderer.frame;
		for (size_t i = 0; i < sizeof(gba->renderer.frame) / 8; ++i) {
			h = (h ^ words[i]) * 0x100000001B3ull;
		}
		hashBytes(&h, sizeof(h));
		static FILE* frames;
		if (!frames && getenv("BENCH_FRAMES")) {
			frames = fopen(getenv("BENCH_FRAMES"), "w");
		}
		if (frames) {
			// Each frame's sound and picture separately, to find where two builds part.
			fprintf(frames, "%u %08X %016llX\n", gba->video.frameCounter, before == audioHash ? 0 : audioHash, (unsigned long long) h);
		}
	}
}

static void stop(struct GBA* g, void* context, uint32_t late) {
	stopped = true;
	g->returnRequested = true;
}

static void runCycles(int32_t cycles) {
	struct Event event = { .callback = stop, .priority = 0x100, .endsRun = true };
	gbaSetKeys(gba, keys);
	timingSchedule(gba, &event, cycles);
	stopped = false;
	while (!stopped) {
		RUN_LOOP(gba);
	}
	timingDeschedule(gba, &event);
	takeAudio();
}

static uint32_t now(void) {
	return gba->timing.masterCycles + gba->cpu.cycles;
}

static uint32_t frame(void) {
	return gba->video.frameCounter;
}

static void snapshot(struct Snapshot* s) {
	struct CPU* cpu = &gba->cpu;
	videoSync(gba);
	gbaIrqSync(gba);
	memcpy(s->r, cpu->r, sizeof(s->r));
	s->cpsr = cpu->cpsr;
	s->spsr = cpu->spsr;
	memcpy(s->bank, cpu->bank, sizeof(s->bank));
	memcpy(s->bankSpsr, cpu->bankSpsr, sizeof(s->bankSpsr));
	s->shifterCarry = (cpu->shifterCarry >> 8) & 1;
	memcpy(s->io, gba->io, sizeof(s->io));
	memcpy(s->iwram, gba->iwram, sizeof(s->iwram));
	memcpy(s->ewram, gba->ewram, sizeof(s->ewram));
	memcpy(s->palette, gba->palette, sizeof(s->palette));
	memcpy(s->vram, gba->vram, sizeof(s->vram));
	memcpy(s->oam, gba->oam, sizeof(s->oam));
	memcpy(s->frame, gba->renderer.frame, sizeof(s->frame));
}

static void setKeys(uint16_t k) {
	keys = k;
}

static void loadSave(const uint8_t* data, size_t size) {
	saveSize = size < sizeof(gba->savedata.storage) ? size : sizeof(gba->savedata.storage);
	save = malloc(saveSize);
	memcpy(save, data, saveSize);
	gba->savedata.backed = true;
	reset();
}

static int saveType(void) {
	return gba->savedata.type;
}

static void forceSaveType(int type) {
	savedataForceType(gba, type);
}

static const uint8_t* saveData(void) {
	return gba->savedata.storage;
}

const struct GameEmu GAME = { GAME_NAME, load, reset, runFrame, runCycles, now, frame, snapshot, setKeys, loadSave, saveType, forceSaveType, saveData, audioOut };
