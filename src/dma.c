/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"

#include <string.h>

// A request starts a channel two cycles later. Started channels wait for the bus and run, highest
// priority first, before the CPU's next access; a cycle goes on starting up and one on handing back.

enum {
	DMA_INCREMENT,
	DMA_DECREMENT,
	DMA_FIXED,
	DMA_INCREMENT_RELOAD,
};

enum {
	DMA_TIMING_NOW,
	DMA_TIMING_VBLANK,
	DMA_TIMING_HBLANK,
	DMA_TIMING_CUSTOM,
};

static const int DMA_OFFSET[] = { 1, -1, 0, 1 };
static const uint32_t DMA_SRC_MASK[] = { 0x07FFFFFE, 0x0FFFFFFE, 0x0FFFFFFE, 0x0FFFFFFE };
static const uint32_t DMA_DST_MASK[] = { 0x07FFFFFE, 0x07FFFFFE, 0x07FFFFFE, 0x0FFFFFFE };

// The fields of a channel's control word: how the destination and source addresses move, whether it
// repeats, 16 or 32 bits a unit, what starts it, and whether it is on
static int destControl(uint16_t reg) {
	return (reg >> 5) & 3;
}

static int sourceControl(uint16_t reg) {
	return (reg >> 7) & 3;
}

static bool repeats(uint16_t reg) {
	return reg & 0x0200;
}

static bool wide(uint16_t reg) {
	return reg & 0x0400;
}

static int timingOf(uint16_t reg) {
	return (reg >> 12) & 3;
}

static bool enabled(uint16_t reg) {
	return reg & 0x8000;
}

// Units to move: a count of 0 means the most the channel can, 0x4000 or for channel 3 0x10000
static int32_t countOf(struct GBA* gba, int i) {
	int32_t count = gba->io[IO(REG_DMA0CNT_LO + i * 12)];
	if (i == 3) {
		return count ? count : 0x10000;
	}
	count &= 0x3FFF;
	return count ? count : 0x4000;
}

// A channel's start event: from now it takes the bus at the CPU's next access. A higher-priority
// channel starting inside a lower one makes that one stop and come back.
static void started(struct GBA* gba, void* context, uint32_t late) {
	(void) late;
	int i = (int) (intptr_t) context;
	if (!gba->dmaRunnable) {
		gba->activeDma = i;
	} else if (i < gba->activeDma) {
		gba->activeDma = i;
		gba->dmaReenter = true;
	}
	gba->dmaRunnable |= 1 << i;
}

// DMA starts two cycles after what asks for it
static void request(struct GBA* gba, int i, uint32_t late) {
	struct Dma* dma = &gba->dma[i];
	timingDeschedule(gba, &dma->start);
	timingSchedule(gba, &dma->start, 2 - (int32_t) late);
}

void dmaInit(struct GBA* gba) {
	for (int i = 0; i < 4; ++i) {
		gba->dma[i].start = (struct Event) { .callback = started, .context = (void*) (intptr_t) i, .priority = 0x40 + i };
	}
}

// Channels off, nothing running; the events themselves are kept
void dmaReset(struct GBA* gba) {
	for (int i = 0; i < 4; ++i) {
		struct Event start = gba->dma[i].start;
		memset(&gba->dma[i], 0, sizeof(gba->dma[i]));
		gba->dma[i].start = start;
	}
	gba->dmaRunnable = 0;
	gba->activeDma = -1;
	gba->dmaReenter = false;
	gba->performingDma = 0;
	gba->dmaLatch = 0;
	gba->lastAccessDma = false;
	gba->dmaIdleCredit = 0;
}

// The address registers, less the top bits a channel cannot address. They are copied to the working
// addresses when the channel is switched on.
uint32_t dmaWriteSource(struct GBA* gba, int dma, uint32_t address) {
	gba->dma[dma].source = address & DMA_SRC_MASK[dma];
	return gba->dma[dma].source;
}

uint32_t dmaWriteDest(struct GBA* gba, int dma, uint32_t address) {
	gba->dma[dma].dest = address & DMA_DST_MASK[dma];
	return gba->dma[dma].dest;
}

// Whether any channel is waiting for this kind of start
bool dmaTimingArmed(struct GBA* gba, int timing) {
	for (int i = 0; i < 4; ++i) {
		uint16_t reg = gba->dma[i].reg;
		if (enabled(reg) && timingOf(reg) == timing && !gba->dma[i].fifo) {
			return true;
		}
	}
	return false;
}

// Whether an event is a channel starting, which compiled code cannot have fall inside an instruction
bool dmaStartEvent(const struct Event* event) {
	return event->callback == started;
}

// Whether channel i is set up to refill a sound FIFO
bool dmaFifoArmed(struct GBA* gba, int i) {
	return enabled(gba->dma[i].reg) && gba->dma[i].fifo;
}

// The control word. Switching a channel on latches its addresses and count, and starts it straight
// away if that is its timing.
uint16_t dmaWriteControl(struct GBA* gba, int i, uint16_t control) {
	struct Dma* dma = &gba->dma[i];
	if (i == 1 || i == 2) {
		// Quiet timer overflows counted on this channel's requests
		timerReplan(gba, timingCurrentTime(gba));
	}
	bool wasEnabled = enabled(dma->reg);
	bool wasHblank = wasEnabled && timingOf(dma->reg) == DMA_TIMING_HBLANK;
	control &= i < 3 ? 0xF7E0 : 0xFFE0;
	dma->reg = control;
	if (wasHblank != (enabled(control) && timingOf(control) == DMA_TIMING_HBLANK)) {
		// H-blank events on the visible lines come and go with it
		videoHblankDmaArmed(gba);
	}
	if (!enabled(control)) {
		gba->dmaRunnable &= ~(1 << i);
		timingDeschedule(gba, &dma->start);
		if (i == gba->activeDma) {
			gba->dmaReenter = true;
		}
		return control;
	}
	if (wasEnabled) {
		// Changed while running, the rest of the transfer follows the new settings
		if (i == gba->activeDma && gba->performingDma) {
			gba->dmaReenter = true;
		}
		if (i == 3 && timingOf(control) == DMA_TIMING_CUSTOM) {
			videoCaptureArmed(gba);
		}
		return control;
	}
	dma->nextSource = dma->source;
	dma->nextDest = dma->dest;
	dma->fifo = timingOf(control) == DMA_TIMING_CUSTOM && (i == 1 || i == 2);
	if (dma->fifo) {
		// The FIFO a channel writes to sends it its requests
		if (dma->dest == (0x04000000 | REG_FIFO_A_LO)) {
			gba->audio.chA.dmaSource = i;
		} else if (dma->dest == (0x04000000 | REG_FIFO_B_LO)) {
			gba->audio.chB.dmaSource = i;
		}
		dma->nextCount = 4;
		dma->nextSource &= ~3u;
		dma->nextDest &= ~3u;
		return control;
	}
	uint32_t mask = wide(control) ? ~3u : ~1u;
	dma->nextSource &= mask;
	dma->nextDest &= mask;
	dma->nextCount = countOf(gba, i);
	if (timingOf(control) == DMA_TIMING_NOW) {
		request(gba, i, 0);
	} else if (i == 3 && timingOf(control) == DMA_TIMING_CUSTOM) {
		videoCaptureArmed(gba);
	}
	return control;
}

// Starts every channel waiting for this timing, the FIFO ones apart
static void requestTiming(struct GBA* gba, int timing, uint32_t late) {
	for (int i = 0; i < 4; ++i) {
		uint16_t reg = gba->dma[i].reg;
		if (enabled(reg) && timingOf(reg) == timing && !gba->dma[i].fifo) {
			request(gba, i, late);
		}
	}
}

// The video timings: every visible line's H-blank, V-blank, and for channel 3 each line from 2 to 161
void dmaRequestHblank(struct GBA* gba, uint32_t late) {
	requestTiming(gba, DMA_TIMING_HBLANK, late);
}

void dmaRequestVblank(struct GBA* gba, uint32_t late) {
	requestTiming(gba, DMA_TIMING_VBLANK, late);
}

void dmaRequestDisplayStart(struct GBA* gba, uint32_t late) {
	uint16_t reg = gba->dma[3].reg;
	if (enabled(reg) && timingOf(reg) == DMA_TIMING_CUSTOM) {
		request(gba, 3, late);
	}
}

// A sound FIFO has room for four more words
void dmaRequestFifo(struct GBA* gba, int i, uint32_t late) {
	struct Dma* dma = &gba->dma[i];
	if (enabled(dma->reg) && dma->fifo) {
		request(gba, i, late);
	}
}

// A channel's transfer is done: its interrupt, then off, or set up again if it repeats
static void finish(struct GBA* gba, int i) {
	struct Dma* dma = &gba->dma[i];
	gba->dmaRunnable &= ~(1 << i);
	if (dma->reg & 0x4000) {
		gbaRaiseIrq(gba, IRQ_DMA0 + i, 0);
	}
	int timing = timingOf(dma->reg);
	// The display start channel stops after the last visible line
	bool lastCapture = i == 3 && timing == DMA_TIMING_CUSTOM && videoLine(gba) == 161;
	if (!repeats(dma->reg) || timing == DMA_TIMING_NOW || lastCapture) {
		dma->reg &= ~0x8000;
		gba->io[IO(REG_DMA0CNT_HI + i * 12)] &= ~0x8000;
		return;
	}
	if (dma->fifo) {
		dma->nextCount = 4;
		return;
	}
	dma->nextCount = countOf(gba, i);
	if (destControl(dma->reg) == DMA_INCREMENT_RELOAD) {
		dma->nextDest = dma->dest & (wide(dma->reg) ? ~3u : ~1u);
	}
}

// Memory a transfer can be done on directly: no I/O, no cartridge
static bool plainSource(unsigned region) {
	return region >= REGION_EWRAM && region <= REGION_OAM && region != REGION_IO;
}

// Units memoryDmaCopy can move: between RAM, palette, VRAM and OAM, or read forwards from cartridge
// ROM once the burst has started and the cartridge's counter is at the source
static bool plain(struct GBA* gba, uint32_t source, uint32_t dest, int32_t sourceStep, bool cartridge, bool eeprom) {
	unsigned region = source >> 24;
	if (!plainSource(dest >> 24)) {
		return false;
	}
	if (plainSource(region)) {
		return true;
	}
	return cartridge && sourceStep > 0 && region >= REGION_ROM0 && region <= REGION_ROM2_EX && !(eeprom && region == REGION_ROM2_EX) &&
	       !gba->cpu.prefetching && gba->romLatch == (source & (SIZE_ROM - 1));
}

// Moves the active channel's units until it is done or a higher channel takes over. Each unit is a
// read and a write, each costing what that access costs on the bus.
static void runChannel(struct GBA* gba) {
	int i = gba->activeDma;
	struct Dma* dma = &gba->dma[i];
	int size = dma->fifo || wide(dma->reg) ? 4 : 2;
	int32_t sourceStep = DMA_OFFSET[sourceControl(dma->reg)] * size;
	int32_t destStep = dma->fifo ? 0 : DMA_OFFSET[destControl(dma->reg)] * size;
	bool eeprom = gba->savedata.type == SAVEDATA_EEPROM || gba->savedata.type == SAVEDATA_EEPROM512;
	// The first cartridge access starts a burst
	bool cartridge = false;
	gba->performingDma = 1 | (i << 1);
	while (dma->nextCount) {
		if (gba->dmaReenter) {
			gba->dmaReenter = false;
			return;
		}
		uint32_t source = dma->nextSource;
		uint32_t dest = dma->nextDest;
		if (plain(gba, source, dest, sourceStep, cartridge, eeprom)) {
			int moved = memoryDmaCopy(gba, dma, dma->nextCount, size == 4, sourceStep, destStep);
			if (moved) {
				dma->nextCount -= moved;
				continue;
			}
		}
		bool sourceSeq = true;
		bool destSeq = true;
		if (!cartridge) {
			if (source >= 0x08000000) {
				sourceSeq = false;
				cartridge = true;
			} else if (dest >= 0x08000000) {
				destSeq = false;
				cartridge = true;
			}
		}
		if (source < 0x02000000) {
			busTick(gba, 1);
		} else if (eeprom && size == 2 && (source >> 24) == REGION_ROM2_EX) {
			busTick(gba, busCycles(gba, source, 2, sourceSeq));
			dma->latch = savedataReadEeprom(gba) * 0x10001;
		} else if (size == 2) {
			dma->latch = busDmaRead(gba, source, 2, sourceSeq) * 0x10001;
		} else {
			dma->latch = busDmaRead(gba, source, 4, sourceSeq);
		}
		gba->dmaLatch = dma->latch;
		if (size == 4) {
			busDmaWrite(gba, dest, dma->latch, 4, destSeq);
		} else if ((dest >> 24) == REGION_ROM2_EX && (eeprom || gba->savedata.type == SAVEDATA_AUTODETECT)) {
			busTick(gba, busCycles(gba, dest, 2, destSeq));
			if (gba->savedata.type == SAVEDATA_AUTODETECT) {
				// The rest of this transfer goes to the EEPROM too
				savedataInitEeprom(gba);
				eeprom = true;
			}
			savedataWriteEeprom(gba, dma->latch, dma->nextCount);
		} else {
			busDmaWrite(gba, dest, (uint16_t) (dma->latch >> (8 * (dest & 2))), 2, destSeq);
		}
		dma->nextSource += sourceStep;
		dma->nextDest += destStep;
		--dma->nextCount;
	}
	finish(gba, i);
}

// The lowest-numbered channel has priority
static int lowest(uint8_t bits) {
	return __builtin_ctz(bits);
}

// Runs whatever channels are ready, in priority order, with the CPU held. Returns the cycles it
// took, which the CPU's internal cycles under way can be counted against.
int32_t dmaRun(struct GBA* gba) {
	uint32_t start = timingCurrentTime(gba);
	struct CPU* cpu = &gba->cpu;
	bool wasHalted = cpu->halted;
	gba->activeDma = lowest(gba->dmaRunnable);
	busTick(gba, 1);
	while (gba->dmaRunnable) {
		gba->activeDma = lowest(gba->dmaRunnable);
		runChannel(gba);
	}
	gba->performingDma = 0;
	gba->activeDma = -1;
	// A DMA that halted the CPU hands the bus back once the CPU wakes
	while (!wasHalted && cpu->halted) {
		if (cpu->cycles < cpu->nextEvent) {
			cpu->cycles = cpu->nextEvent;
		}
		processEvents(gba);
	}
	busTick(gba, 1);
	return timingCurrentTime(gba) - start;
}
