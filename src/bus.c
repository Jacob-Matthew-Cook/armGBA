/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"
#include "cpu/jit-internal.h"

// Timed bus accesses. Each moves the clock on by its own cycles and lets events due by then happen
// before it reads or writes, so it sees what the hardware would. A DMA waiting for the bus goes
// before the CPU's next access.

// The six regions the cartridge ROM is mirrored in.
static bool inRom(uint32_t address) {
	return (address >> 24) >= REGION_ROM0 && (address >> 24) <= REGION_ROM2_EX;
}

// 1 plus the wait states.
int32_t busCycles(struct GBA* gba, uint32_t address, int size, bool seq) {
	unsigned region = address >> 24;
	switch (region) {
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		// An 8-bit bus that takes the same time whatever the size.
		return 1 + gba->waitN16[region];
	}
	if (size == 4) {
		return 1 + (seq ? gba->waitS32[region] : gba->waitN32[region]);
	}
	return 1 + (seq ? gba->waitS16[region] : gba->waitN16[region]);
}

// Moves the clock on and runs the events that have come due.
void busTick(struct GBA* gba, int32_t cycles) {
	struct CPU* cpu = &gba->cpu;
	cpu->cycles += cycles;
	if (cpu->cycles >= cpu->nextEvent) {
		processEvents(gba);
	}
}

// A sequential cartridge access reads on from the cartridge's address counter, whatever address it
// asks for; a nonsequential one sets the counter. The counter covers 128 KiB, so the GBA starts a
// new burst at a boundary: for the CPU, at an access there; for DMA, at the access after one that
// ended there, wherever the counter has really got to.
static uint32_t cartridge(struct GBA* gba, uint32_t address, int size, bool* seq, bool dma) {
	if (!((dma ? gba->romNext : address) & 0x1FFFF)) {
		*seq = false;
	}
	gba->romNext = (address + size) & (SIZE_ROM - 1);
	if (!*seq) {
		gba->romLatch = address & (SIZE_ROM - size);
	}
	uint32_t latched = (address & ~(SIZE_ROM - 1)) | gba->romLatch;
	gba->romLatch = (gba->romLatch + size) & (SIZE_ROM - 1);
	return latched;
}

// Reads or writes size bytes, without timing.
static uint32_t load(struct GBA* gba, uint32_t address, int size) {
	switch (size) {
	case 4:
		return memoryLoad32(gba, address, NULL);
	case 2:
		return memoryLoad16(gba, address, NULL);
	default:
		return memoryLoad8(gba, address, NULL);
	}
}

static void store(struct GBA* gba, uint32_t address, uint32_t value, int size) {
	switch (size) {
	case 4:
		memoryStore32(gba, address, value, NULL);
		break;
	case 2:
		memoryStore16(gba, address, value, NULL);
		break;
	default:
		memoryStore8(gba, address, value, NULL);
		break;
	}
}

// Data on the cartridge bus stops the prefetcher, which costs a cycle if a read was just finishing.
// A code fetch from the cartridge already has the PC there.
static int32_t prefetcherStop(struct GBA* gba, bool codeFetch) {
	struct Prefetcher* p = &gba->prefetcher;
	if (!gba->cpu.prefetching) {
		return 0;
	}
	gba->cpu.prefetching = false;
	int32_t lead = timingCurrentTime(gba) - p->base;
	if (lead >= p->full) {
		return 0;
	}
	if (!codeFetch) {
		// Compiled code leaves the PC to be worked out when something asks.
		jitSyncPc(gba);
		if (!inRom(gba->cpu.r[15])) {
			return 0;
		}
	}
	// Less than full: eight reads at most.
	int32_t into = lead;
	while (into >= p->duty) {
		into -= p->duty;
	}
	return into == p->duty - 1 || (p->width == 4 && into == p->duty - p->duty / 2 - 1);
}

// Returns what prefetcherStop would cost now for a code fetch, without stopping anything.
static int32_t stopCost(struct GBA* gba) {
	struct Prefetcher* p = &gba->prefetcher;
	if (!gba->cpu.prefetching) {
		return 0;
	}
	int32_t lead = timingCurrentTime(gba) - p->base;
	if (lead >= p->full) {
		return 0;
	}
	int32_t into = lead;
	while (into >= p->duty) {
		into -= p->duty;
	}
	return into == p->duty - 1 || (p->width == 4 && into == p->duty - p->duty / 2 - 1);
}

// Makes the two fetches that refill the pipeline at pc in the cartridge, as busFetch makes them,
// when nothing can come between them: no DMA waiting, no event due, the buffer not already reading
// there, and no new 128 KiB burst for the second. Otherwise returns false, having done nothing.
bool busRefillCartridge(struct GBA* gba, uint32_t pc, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	struct Prefetcher* p = &gba->prefetcher;
	int size = thumb ? 2 : 4;
	if (gba->dmaRunnable || gba->prefetchDisabled || (gba->cpu.prefetching && p->head == pc) || !((pc + size) & 0x1FFFF)) {
		return false;
	}
	unsigned region = pc >> 24;
	int32_t n = 1 + (thumb ? gba->waitN16[region] : gba->waitN32[region]);
	int32_t s = 1 + (thumb ? gba->waitS16[region] : gba->waitS32[region]);
	int32_t stop = stopCost(gba);
	if (cpu->cycles + stop + n + s >= cpu->nextEvent) {
		return false;
	}
	uint32_t now = timingCurrentTime(gba);
	gba->dmaIdleCredit = 0;
	gba->romNext = (pc + 2 * size) & (SIZE_ROM - 1);
	gba->romLatch = gba->romNext;
	gba->cpu.prefetching = false;
	if (gba->prefetchEnabled) {
		// Started by the first fetch, which the second waits on.
		gba->cpu.prefetching = true;
		p->width = size;
		p->duty = s;
		p->full = (thumb ? 8 : 4) * s;
		p->head = pc + 2 * size;
		p->base = now + stop + n + s;
	}
	cpu->cycles += stop + n + s;
	cpu->fetchSeq = true;
	gba->lastAccessDma = false;
	cpu->prefetch[0] = thumb ? codeFetch16(gba, pc) : codeFetch32(gba, pc);
	cpu->prefetch[1] = thumb ? codeFetch16(gba, pc + size) : codeFetch32(gba, pc + size);
	return true;
}

// A code fetch from the cartridge: from the buffer, or once the read in flight finishes, or else as
// an ordinary access, after which the prefetcher reads on from there.
static int32_t prefetcherFetch(struct GBA* gba, uint32_t address, int size, bool seq) {
	struct Prefetcher* p = &gba->prefetcher;
	uint32_t now = timingCurrentTime(gba);
	if (gba->cpu.prefetching && address == p->head) {
		uint32_t base = p->base;
		if ((int32_t) (now - base) > p->full) {
			base = now - p->full;
		}
		int32_t cycles = base + p->duty - now;
		p->base = base + p->duty;
		p->head += p->width;
		return cycles < 1 ? 1 : cycles;
	}
	int32_t cycles = prefetcherStop(gba, true);
	if (gba->prefetchDisabled) {
		gba->prefetchDisabled = false;
		seq = false;
	}
	cycles += busCycles(gba, address, size, seq);
	if (gba->prefetchEnabled) {
		gba->cpu.prefetching = true;
		p->width = size;
		p->duty = busCycles(gba, address, size, true);
		p->full = (size == 2 ? 8 : 4) * p->duty;
		p->head = address + size;
		p->base = now + cycles;
	}
	return cycles;
}

// A data access's cycles; one on the cartridge bus stops the prefetcher first.
static int32_t dataCycles(struct GBA* gba, uint32_t address, int size, bool seq) {
	int32_t cycles = busCycles(gba, address, size, seq);
	unsigned region = address >> 24;
	if (region >= REGION_ROM0 && region <= REGION_SRAM_MIRROR) {
		cycles += prefetcherStop(gba, false);
	} else if (region == REGION_VRAM && gba->vramStall) {
		// The display has VRAM until H-blank.
		cycles += videoVramStall(gba, cycles);
	}
	return cycles;
}

// Before any CPU access, DMA that is ready goes first, and the access after DMA is nonsequential.
static bool cpuAccess(struct GBA* gba, bool seq) {
	if (gba->dmaRunnable) {
		dmaRun(gba);
	}
	gba->dmaIdleCredit = 0;
	return seq && !gba->lastAccessDma;
}

// A data read by the CPU: events up to the end of the access happen first, then the value is read.
uint32_t busRead(struct GBA* gba, uint32_t address, int size, bool seq) {
	seq = cpuAccess(gba, seq);
	uint32_t at = address;
	if (inRom(address)) {
		at = cartridge(gba, address & -size, size, &seq, false) | (address & (size - 1));
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	uint32_t value = load(gba, at, size);
	gba->lastAccessDma = false;
	if (gba->idleProbing) {
		jitIdleRead(gba, address, size, value);
	}
	return value;
}

// A data write by the CPU, likewise.
void busWrite(struct GBA* gba, uint32_t address, uint32_t value, int size, bool seq) {
	if (gba->idleProbing) {
		jitIdleWrite(gba, address, value, size);
	}
	seq = cpuAccess(gba, seq);
	if (inRom(address)) {
		cartridge(gba, address & -size, size, &seq, false);
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	store(gba, address, value, size);
	gba->lastAccessDma = false;
}

// An instruction fetch. From the cartridge, it goes through the prefetch buffer, which may already
// have the word and so make the fetch cost one cycle.
uint32_t busFetch(struct GBA* gba, uint32_t address, bool thumb) {
	struct CPU* cpu = &gba->cpu;
	int size = thumb ? 2 : 4;
	bool seq = cpuAccess(gba, cpu->fetchSeq);
	cpu->fetchSeq = true;
	address &= -size;
	if (inRom(address)) {
		cartridge(gba, address, size, &seq, false);
		busTick(gba, prefetcherFetch(gba, address, size, seq));
	} else {
		busTick(gba, busCycles(gba, address, size, seq));
	}
	gba->lastAccessDma = false;
	return thumb ? codeFetch16(gba, address) : codeFetch32(gba, address);
}

// An internal cycle: a waiting DMA runs alongside, and covers the CPU's internal cycles while it
// lasts.
void busIdle(struct GBA* gba) {
	if (gba->dmaRunnable) {
		gba->dmaIdleCredit = dmaRun(gba);
	}
	if (gba->dmaIdleCredit) {
		--gba->dmaIdleCredit;
		return;
	}
	busTick(gba, 1);
}

// DMA's reads and writes: the same bus, without the CPU's prefetch.
uint32_t busDmaRead(struct GBA* gba, uint32_t address, int size, bool seq) {
	uint32_t at = address;
	if (inRom(address)) {
		at = cartridge(gba, address & -size, size, &seq, true);
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	uint32_t value = load(gba, at, size);
	gba->lastAccessDma = true;
	return value;
}

void busDmaWrite(struct GBA* gba, uint32_t address, uint32_t value, int size, bool seq) {
	if (inRom(address)) {
		cartridge(gba, address & -size, size, &seq, true);
	}
	busTick(gba, dataCycles(gba, address & -size, size, seq));
	store(gba, address, value, size);
	gba->lastAccessDma = true;
}
