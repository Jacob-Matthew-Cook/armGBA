/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "game.h"
#include "mgba-state.h"

#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/config.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

static struct mCore* core;
static struct GBA* gba;
static bool stopped;
static bool skip;
static const char* biosPath;
static uint16_t keys;
// As large as the libretro core's, which RetroArch save files match.
static uint8_t save[0x20000];
static uint8_t* saveFile;
static size_t saveFileSize;
static int forcedSaveType = -1;

// Both emulators read the same fixed clock.
static time_t fixedTime(struct mRTCSource* source) {
	return 1700000000;
}

static struct mRTCSource fixedClock = { .unixTime = fixedTime };

// mGBA's log messages cost time to format and print, and nothing reads them.
static void quietLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
}

static struct mLogger quietLogger = { .log = quietLog };

// libmgba is built with COLOR_16_BIT, so this is BGR555.
static uint16_t video[160][240];

static void reset(void) {
	// The renderer only fills lines as it draws them, so start from white, as armGBA does.
	for (int y = 0; y < 160; ++y) {
		for (int x = 0; x < 240; ++x) {
			video[y][x] = 0x7FFF;
		}
	}
	gba->rtcSource = &fixedClock;
	if (saveFile) {
		memset(save, 0xFF, sizeof(save));
		memcpy(save, saveFile, saveFileSize);
	}
	core->reset(core);
	size_t stateSize;
	const uint8_t* state = mgbaStateFile(&stateSize);
	if (state && !core->loadState(core, state)) {
		fprintf(stderr, "MGBA_STATE isn't an mGBA GBA state\n");
	}
	// MGBA_IDLE keeps mGBA's removal of the idle loops it knows, as its frontends run it; that
	// isn't cycle-exact.
	if (!getenv("MGBA_IDLE")) {
		gba->idleOptimization = IDLE_LOOP_IGNORE;
	}
	if (forcedSaveType != -1) {
		GBASavedataForceType(&gba->memory.savedata, forcedSaveType);
	}
}

static bool load(const char* bios, const uint8_t* rom, size_t romSize, bool skipBios) {
	biosPath = bios;
	skip = skipBios;
	mLogSetDefaultLogger(&quietLogger);
	core = GBACoreCreate();
	core->init(core);
	mCoreInitConfig(core, NULL);
	struct mCoreOptions opts = { .useBios = true, .skipBios = skipBios };
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);
	// The software renderer masks display registers, so it has to be attached as in the libretro
	// core.
	if (!getenv("NO_RENDERER")) {
		core->setVideoBuffer(core, (void*) video, 240);
	}
	uint8_t* copy = malloc(romSize);
	memcpy(copy, rom, romSize);
	core->loadROM(core, VFileFromMemory(copy, romSize));
	core->loadBIOS(core, VFileOpen(bios, O_RDONLY), 0);
	gba = core->board;
	reset();
	return gba->memory.fullBios;
}

static void runFrame(void) {
	core->setKeys(core, keys);
	core->runFrame(core);
}

static void stop(struct mTiming* timing, void* context, uint32_t late) {
	stopped = true;
}

static void runCycles(int32_t cycles) {
	struct mTimingEvent event = { .context = NULL, .callback = stop, .name = "stop", .priority = 0x100 };
	core->setKeys(core, keys);
	mTimingSchedule(&gba->timing, &event, cycles);
	stopped = false;
	while (!stopped) {
		ARMRunLoop(core->cpu);
	}
	mTimingDeschedule(&gba->timing, &event);
}

static uint32_t now(void) {
	return gba->timing.masterCycles + gba->cpu->cycles;
}

static uint32_t frame(void) {
	return gba->video.frameCounter;
}

static void snapshot(struct Snapshot* s) {
	struct ARMCore* cpu = gba->cpu;
	memcpy(s->r, cpu->gprs, sizeof(s->r));
	s->cpsr = cpu->cpsr.packed;
	s->spsr = cpu->spsr.packed;
	memcpy(s->bank, cpu->bankedRegisters, sizeof(s->bank));
	memcpy(s->bankSpsr, cpu->bankedSPSRs, sizeof(s->bankSpsr));
	s->shifterCarry = cpu->shifterCarryOut & 1;
	memcpy(s->io, gba->memory.io, sizeof(s->io));
	memcpy(s->iwram, gba->memory.iwram, sizeof(s->iwram));
	memcpy(s->ewram, gba->memory.wram, sizeof(s->ewram));
	memcpy(s->palette, gba->video.palette, sizeof(s->palette));
	memcpy(s->vram, gba->video.vram, sizeof(s->vram));
	memcpy(s->oam, gba->video.oam.raw, sizeof(s->oam));
	memcpy(s->frame, video, sizeof(s->frame));
}

static void setKeys(uint16_t k) {
	keys = k;
}

static void loadSave(const uint8_t* data, size_t size) {
	saveFileSize = size < sizeof(save) ? size : sizeof(save);
	saveFile = malloc(saveFileSize);
	memcpy(saveFile, data, saveFileSize);
	reset();
	core->loadSave(core, VFileFromMemory(save, sizeof(save)));
}

static int saveType(void) {
	return gba->memory.savedata.type;
}

static void forceSaveType(int type) {
	forcedSaveType = type;
	GBASavedataForceType(&gba->memory.savedata, type);
}

const struct GameEmu mgbaGame = { "mGBA", load, reset, runFrame, runCycles, now, frame, snapshot, setKeys, loadSave, saveType, forceSaveType, NULL, NULL };
