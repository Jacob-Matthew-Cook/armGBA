/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "oracle.h"

#include <mgba/flags.h>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>
#include <mgba/internal/arm/arm.h>
#include <mgba/internal/arm/isa-inlines.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <string.h>

// cpufuzz's mGBA side: the same, on an mGBA core built from a tree with mgba-oracle.patch

static struct mCore* core;
static struct GBA* gba;
static uint8_t* romData;
static size_t romLength;
static bool stopped;

static bool init(const char* bios, uint8_t* rom, size_t romSize) {
	romData = rom;
	romLength = romSize;
	core = GBACoreCreate();
	core->init(core);
	mCoreInitConfig(core, NULL);
	core->loadROM(core, VFileFromMemory(rom, romSize));
	struct VFile* vf = VFileOpen(bios, O_RDONLY);
	if (!vf) {
		return false;
	}
	core->reset(core);
	gba = core->board;
	// Run the real BIOS for SWIs, not mGBA's high-level versions
	GBALoadBIOS(gba, vf);
	gba->idleOptimization = IDLE_LOOP_IGNORE;
	return gba->memory.fullBios;
}

static uint8_t* memory(uint32_t address) {
	switch (address >> 24) {
	case 2:
		return (uint8_t*) gba->memory.wram + (address & 0x3FFFF);
	case 3:
		return (uint8_t*) gba->memory.iwram + (address & 0x7FFF);
	default:
		return (uint8_t*) gba->memory.rom + (address & (romLength - 1));
	}
}

static void codeChanged(void) {
}

static void stop(struct mTiming* timing, void* context, uint32_t late) {
	stopped = true;
}

static uint32_t run(const struct FuzzState* in, uint32_t pc, uint16_t waitcnt, int32_t deadline, struct FuzzState* out) {
	struct ARMCore* cpu = core->cpu;
	GBAIOWrite(gba, GBA_REG_WAITCNT, waitcnt);
	memcpy(cpu->gprs, in->r, sizeof(in->r));
	memcpy(cpu->bankedRegisters, in->bank, sizeof(in->bank));
	memcpy(cpu->bankedSPSRs, in->bankSpsr, sizeof(in->bankSpsr));
	cpu->cpsr.packed = in->cpsr;
	cpu->spsr.packed = in->spsr;
	cpu->shifterCarryOut = in->shifterCarry;
	cpu->privilegeMode = in->cpsr & 0x1F;
	cpu->executionMode = (in->cpsr & 0x20) ? MODE_THUMB : MODE_ARM;
	cpu->halted = 0;
	gba->memory.activeRegion = -1;
	cpu->gprs[15] = pc;
	if (cpu->executionMode == MODE_THUMB) {
		ThumbWritePC(cpu);
	} else {
		ARMWritePC(cpu);
	}
	cpu->cycles = 0;
	cpu->nextEvent = 0;

	struct mTimingEvent event = { .context = NULL, .callback = stop, .name = "stop", .priority = 0x100 };
	uint32_t start = gba->timing.masterCycles + cpu->cycles;
	mTimingSchedule(&gba->timing, &event, deadline);
	stopped = false;
	while (!stopped) {
		ARMRunLoop(cpu);
	}
	uint32_t elapsed = gba->timing.masterCycles + cpu->cycles - start;
	mTimingDeschedule(&gba->timing, &event);

	memcpy(out->r, cpu->gprs, sizeof(out->r));
	memcpy(out->bank, cpu->bankedRegisters, sizeof(out->bank));
	memcpy(out->bankSpsr, cpu->bankedSPSRs, sizeof(out->bankSpsr));
	out->cpsr = cpu->cpsr.packed;
	out->spsr = cpu->spsr.packed;
	out->shifterCarry = cpu->shifterCarryOut;
	return elapsed;
}

const struct Emu mgbaEmu = { init, memory, codeChanged, run };
