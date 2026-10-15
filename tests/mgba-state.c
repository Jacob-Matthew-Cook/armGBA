/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "mgba-state.h"

#include "gba.h"
#include "jit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// mGBA's GBASerializedState (mgba/internal/gba/serialize.h, 0x61000 bytes), the part armGBA can
// take: the CPU, memory, I/O registers, timers, DMA and the video line. That's enough to continue
// from the same spot in a game, but isn't an exact copy: the sound FIFOs, prescaler phases and the
// place in the line start afresh.
enum {
	ST_GPRS = 0x20,
	ST_CPSR = 0x60,
	ST_SPSR = 0x64,
	ST_BANKED = 0x70,
	ST_BANKED_SPSR = 0x118,
	ST_VIDEO_NEXT = 0x1F4,
	ST_VIDEO_FLAGS = 0x1F8,
	ST_TIMERS = 0x200,
	ST_TIMER_SIZE = 0x14,
	ST_DMA = 0x250,
	ST_BIOS_PREFETCH = 0x2F4,
	ST_PREFETCH = 0x2F8,
	ST_MISC = 0x31C,
	ST_IO = 0x400,
	ST_PALETTE = 0x800,
	ST_OAM = 0xC00,
	ST_VRAM = 0x1000,
	ST_IWRAM = 0x19000,
	ST_EWRAM = 0x21000,
	ST_SIZE = 0x61000,
};

static uint32_t get32(const uint8_t* state, size_t offset) {
	uint32_t value;
	memcpy(&value, state + offset, 4);
	return value;
}

static uint16_t io(const uint8_t* state, uint32_t reg) {
	uint16_t value;
	memcpy(&value, state + ST_IO + reg, 2);
	return value;
}

static void write(struct GBA* gba, const uint8_t* state, uint32_t from, uint32_t to) {
	for (uint32_t reg = from; reg <= to; reg += 2) {
		ioWrite(gba, reg, io(state, reg));
	}
}

bool mgbaStateLoad(struct GBA* gba, const uint8_t* state, size_t size) {
	if (size < ST_SIZE || (get32(state, 0) & 0xFF000000) != 0x01000000) {
		return false;
	}
	memcpy(gba->palette, state + ST_PALETTE, sizeof(gba->palette));
	memcpy(gba->oam, state + ST_OAM, sizeof(gba->oam));
	memcpy(gba->vram, state + ST_VRAM, sizeof(gba->vram));
	memcpy(gba->iwram, state + ST_IWRAM, sizeof(gba->iwram));
	memcpy(gba->ewram, state + ST_EWRAM, sizeof(gba->ewram));
	gba->renderer.memoryChanged = RENDER_VRAM | RENDER_OAM | RENDER_PALETTE;
	memset(gba->renderer.vramChanged, 1, sizeof(gba->renderer.vramChanged));

	// Wait states first, then sound on before its registers, which it gates.
	ioWrite(gba, REG_WAITCNT, io(state, REG_WAITCNT));
	write(gba, state, 0x000, 0x054);
	ioWrite(gba, REG_SOUNDCNT_X, io(state, REG_SOUNDCNT_X));
	write(gba, state, REG_SOUND1CNT_LO, 0x082);
	ioWrite(gba, REG_SOUNDBIAS, io(state, REG_SOUNDBIAS));
	// Timers count on from where they were: games keep clocks in them.
	static const int prescaleBits[4] = { 0, 6, 8, 10 };
	for (int i = 0; i < 4; ++i) {
		struct Timer* timer = &gba->timers[i];
		uint16_t control = io(state, REG_TM0CNT_LO + 4 * i + 2);
		memcpy(&timer->reload, state + ST_TIMERS + i * ST_TIMER_SIZE, 2);
		timer->pendingReload = timer->reload;
		timer->prescaleBits = prescaleBits[control & 3];
		timer->doIrq = control & 0x0040;
		timer->enable = control & 0x0080;
		timer->countUp = i && (control & 0x0004);
		timer->counter = io(state, REG_TM0CNT_LO + 4 * i);
		timer->running = timer->enable && !timer->countUp;
		timer->started = timingCurrentTime(gba);
		timer->quiet = 0;
		gba->io[IO(REG_TM0CNT_LO + 4 * i + 2)] = control & 0x00C7;
		timingDeschedule(gba, &timer->event);
		if (timer->running) {
			timingSchedule(gba, &timer->event, (0x10000 - timer->counter) << timer->prescaleBits);
		}
	}
	// A channel already running continues where it got to; one that would start now doesn't.
	for (int i = 0; i < 4; ++i) {
		uint32_t reg = REG_DMA0SAD_LO + 12 * i;
		write(gba, state, reg, reg + 8);
		uint16_t control = io(state, reg + 10);
		if ((control & 0x8000) && !(control & 0x3000)) {
			control &= ~0x8000;
		}
		ioWrite(gba, reg + 10, control);
		if (control & 0x8000) {
			const uint8_t* dma = state + ST_DMA + i * 16;
			gba->dma[i].nextSource = get32(dma, 0);
			gba->dma[i].nextDest = get32(dma, 4);
			gba->dma[i].nextCount = (int32_t) get32(dma, 8);
		}
	}
	ioWrite(gba, REG_KEYCNT, io(state, REG_KEYCNT));
	gba->irq.iflags = io(state, REG_IF);
	gba->io[IO(REG_IF)] = gba->irq.iflags;
	ioWrite(gba, REG_IE, io(state, REG_IE));
	ioWrite(gba, REG_IME, io(state, REG_IME));
	gba->io[IO(REG_POSTFLG)] = io(state, REG_POSTFLG);

	// The line: mGBA's next video event is the H-blank in mode 0, or the next line's start in
	// H-blank.
	uint32_t next = get32(state, ST_VIDEO_NEXT);
	bool hblank = get32(state, ST_VIDEO_FLAGS) & 3;
	int32_t untilHblank = hblank ? (int32_t) next - 1232 + 1007 : (int32_t) next;
	videoStartAt(gba, io(state, REG_VCOUNT) & 0xFF, untilHblank);

	struct CPU* cpu = &gba->cpu;
	memcpy(cpu->r, state + ST_GPRS, sizeof(cpu->r));
	memcpy(cpu->bank, state + ST_BANKED, sizeof(cpu->bank));
	memcpy(cpu->bankSpsr, state + ST_BANKED_SPSR, sizeof(cpu->bankSpsr));
	cpu->cpsr = get32(state, ST_CPSR);
	cpu->spsr = get32(state, ST_SPSR);
	cpu->mode = cpu->cpsr & 0x1F;
	cpu->prefetch[0] = get32(state, ST_PREFETCH);
	cpu->prefetch[1] = get32(state, ST_PREFETCH + 4);
	cpu->irqMasked = cpu->cpsr & PSR_I;
	cpu->halted = get32(state, ST_MISC) & 1;
	cpu->fetchSeq = true;
	gba->biosPrefetch = get32(state, ST_BIOS_PREFETCH);
	memorySetActiveRegion(gba, cpu->r[15]);
	jitFlush(gba);
	return true;
}

const uint8_t* mgbaStateFile(size_t* size) {
	static uint8_t* data;
	static size_t length;
	static bool read;
	if (!read) {
		read = true;
		FILE* f = getenv("MGBA_STATE") ? fopen(getenv("MGBA_STATE"), "rb") : NULL;
		if (f) {
			fseek(f, 0, SEEK_END);
			length = ftell(f);
			fseek(f, 0, SEEK_SET);
			data = malloc(length);
			if (fread(data, 1, length, f) != length) {
				free(data);
				data = NULL;
			}
			fclose(f);
		}
	}
	*size = length;
	return data;
}
