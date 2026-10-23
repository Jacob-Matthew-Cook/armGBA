/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "oracle.h"

#include "gba.h"
#include "cpu/jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// cpufuzz's armGBA side: a bare machine that runs a program from a state to a cycle deadline.

static struct GBA* gba;
static uint8_t* romData;
static size_t romLength;
static bool stopped;

static bool init(const char* bios, uint8_t* rom, size_t romSize) {
	romData = rom;
	romLength = romSize;
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
	memoryReset(gba);
	gba->cpu.nextEvent = 0x7FFFFFFF;
	return true;
}

static uint8_t* memory(uint32_t address) {
	switch (address >> 24) {
	case 2:
		return gba->ewram + (address & 0x3FFFF);
	case 3:
		return gba->iwram + (address & 0x7FFF);
	default:
		return gba->rom + (address & (romLength - 1));
	}
}

static void codeChanged(void) {
	jitFlush(gba);
	// FUZZ_FRESH: every run compiles as if the code had never been rewritten.
	if (getenv("FUZZ_FRESH")) {
		jitForgetRewrites(gba);
	}
}

static void stop(struct GBA* g, void* context, uint32_t late) {
	stopped = true;
	g->returnRequested = true;
}

static uint32_t run(const struct FuzzState* in, uint32_t pc, uint16_t waitcnt, int32_t deadline, struct FuzzState* out) {
	struct CPU* cpu = &gba->cpu;
	ioWrite(gba, REG_WAITCNT, waitcnt);
	memcpy(cpu->r, in->r, sizeof(in->r));
	memcpy(cpu->bank, in->bank, sizeof(in->bank));
	memcpy(cpu->bankSpsr, in->bankSpsr, sizeof(in->bankSpsr));
	cpu->cpsr = in->cpsr;
	cpu->spsr = in->spsr;
	cpu->shifterCarry = (in->shifterCarry & 1) << 8;
	cpu->mode = in->cpsr & 0x1F;
	cpu->halted = 0;
	gba->activeRegion = -1;
	cpu->r[15] = pc;
	cpuWritePc(gba);
	stepReset(gba);
	cpu->cycles = 0;
	cpu->nextEvent = 0;

	struct Event event = { .callback = stop, .priority = 0x100, .endsRun = true };
	uint32_t start = gba->timing.masterCycles + cpu->cycles;
	timingSchedule(gba, &event, deadline);
	stopped = false;
	while (!stopped) {
		jitRunLoop(gba);
	}
	uint32_t elapsed = gba->timing.masterCycles + cpu->cycles - start;
	timingDeschedule(gba, &event);

	memcpy(out->r, cpu->r, sizeof(out->r));
	memcpy(out->bank, cpu->bank, sizeof(out->bank));
	memcpy(out->bankSpsr, cpu->bankSpsr, sizeof(out->bankSpsr));
	out->cpsr = cpu->cpsr;
	out->spsr = cpu->spsr;
	out->shifterCarry = (cpu->shifterCarry >> 8) & 1;
	return elapsed;
}

const struct Emu armgbaEmu = { init, memory, codeChanged, run };
