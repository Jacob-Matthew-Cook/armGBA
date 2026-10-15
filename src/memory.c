/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba.h"
#include "jit.h"

#include <string.h>

// Bus timing follows mGBA exactly, so the two can be compared cycle for cycle.

// Wait states by region (the address's top byte) for a 16- or 32-bit access, nonsequential (N) and
// sequential (S), as at power-on. An access costs these plus one cycle; a 32-bit access to a 16-bit
// bus is two accesses, which is where the larger numbers come from.
static const uint8_t BASE_WAIT_N16[16] = { 0, 0, 2, 0, 0, 0, 0, 0, 4, 4, 4, 4, 4, 4, 4 };
static const uint8_t BASE_WAIT_N32[16] = { 0, 0, 5, 0, 0, 1, 1, 0, 7, 7, 9, 9, 13, 13, 9 };
static const uint8_t BASE_WAIT_S16[16] = { 0, 0, 2, 0, 0, 0, 0, 0, 2, 2, 4, 4, 8, 8, 4 };
static const uint8_t BASE_WAIT_S32[16] = { 0, 0, 5, 0, 0, 1, 1, 0, 5, 5, 9, 9, 17, 17, 9 };
// WAITCNT's choices for the cartridge: first-access waits, then each of the three cartridge areas'
// sequential waits for its bit clear and set.
static const uint8_t ROM_WAIT_N[] = { 4, 3, 2, 8 };
static const uint8_t ROM_WAIT_S[] = { 2, 1, 4, 1, 8, 1 };

// A misaligned 32-bit load reads the aligned word and rotates it.
static inline uint32_t ror32(uint32_t value, unsigned rotate) {
	rotate &= 31;
	return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

// Guest memory is little-endian whatever the host is.
static inline uint32_t le32(const uint8_t* p) {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static inline uint16_t le16(const uint8_t* p) {
	uint16_t v;
	memcpy(&v, p, 2);
	return v;
}

static inline void put32(uint8_t* p, uint32_t v) {
	memcpy(p, &v, 4);
}

static inline void put16(uint8_t* p, uint16_t v) {
	memcpy(p, &v, 2);
}

// Works out what a write to the PC costs in each region the PC can land in: the pipeline refill is
// one nonsequential and one sequential fetch there. Compiled code looks these up.
static void updateWritePcCosts(struct GBA* gba) {
	for (int i = 0; i < 256; ++i) {
		bool code = i < 16 && i != 1 && i != REGION_IO && i < REGION_SRAM;
		gba->writePcCost16[i] = code ? 2 + gba->waitN16[i] + gba->waitS16[i] : 2;
		gba->writePcCost32[i] = code ? 2 + gba->waitN32[i] + gba->waitS32[i] : 2;
	}
}

// Sets the power-on wait states. No region is active until the first PC write says where code runs
// from.
void memoryInit(struct GBA* gba) {
	for (int i = 0; i < 256; ++i) {
		gba->waitN16[i] = i < 16 ? BASE_WAIT_N16[i] : 0;
		gba->waitS16[i] = i < 16 ? BASE_WAIT_S16[i] : 0;
		gba->waitN32[i] = i < 16 ? BASE_WAIT_N32[i] : 0;
		gba->waitS32[i] = i < 16 ? BASE_WAIT_S32[i] : 0;
	}
	updateWritePcCosts(gba);
	gba->activeRegion = -1;
	gba->fetchRegion = REGION_BIOS;
}

// Clears work RAM and I/O, resets the wait states, and turns the prefetch buffer off.
void memoryReset(struct GBA* gba) {
	if (gba->rom) {
		memset(gba->ewram, 0, sizeof(gba->ewram));
	}
	memset(gba->iwram, 0, sizeof(gba->iwram));
	memset(gba->io, 0, sizeof(gba->io));
	memoryAdjustWaitstates(gba, 0);
	memoryAdjustEwramWaitstates(gba, 0x0D00);
	gba->prefetchEnabled = false;
	gba->lastPrefetchedPc = 0;
	memset(&gba->prefetcher, 0, sizeof(gba->prefetcher));
	gba->prefetchDisabled = false;
}

// Whether mGBA's GBASetActiveRegion finds memory to run code from at address.
bool memoryCanFetch(struct GBA* gba, uint32_t address) {
	int region = address >> 24;
	switch (region) {
	case REGION_BIOS:
	case REGION_EWRAM:
	case REGION_IWRAM:
	case REGION_PALETTE:
	case REGION_VRAM:
	case REGION_OAM:
		return true;
	default:
		return region >= REGION_ROM0 && region <= REGION_ROM2_EX && (address & (SIZE_ROM - 1)) < gba->romSize;
	}
}

// Called when the PC moves to another region, or off the end of the cartridge. When leaving the
// BIOS, the last word fetched there is kept: it is what reads of the BIOS return from outside it.
void memorySetActiveRegion(struct GBA* gba, uint32_t address) {
	int region = address >> 24;
	gba->lastPrefetchedPc = 0;
	if (region == gba->activeRegion && (region < REGION_ROM0 || (address & (SIZE_ROM - 1)) < gba->romSize)) {
		return;
	}
	if (gba->activeRegion == REGION_BIOS) {
		gba->biosPrefetch = gba->cpu.prefetch[1];
	}
	if (memoryCanFetch(gba, address)) {
		gba->activeRegion = region;
		gba->fetchRegion = region;
	} else {
		gba->activeRegion = -1;
	}
}

// Called when WAITCNT is written: sets the cartridge's and SRAM's wait states, and switches the
// prefetch buffer on or off.
void memoryAdjustWaitstates(struct GBA* gba, uint16_t parameters) {
	int sram = parameters & 3;
	int ws[3] = { (parameters >> 2) & 3, (parameters >> 5) & 3, (parameters >> 8) & 3 };
	int wsSeq[3] = { (parameters >> 4) & 1, (parameters >> 7) & 1, (parameters >> 10) & 1 };

	// SRAM has an 8-bit bus and no sequential accesses.
	for (int r = REGION_SRAM; r <= REGION_SRAM_MIRROR; ++r) {
		gba->waitN16[r] = gba->waitS16[r] = ROM_WAIT_N[sram];
		gba->waitN32[r] = gba->waitS32[r] = 2 * ROM_WAIT_N[sram] + 1;
	}
	// Each cartridge area is two regions. A 32-bit access is a 16-bit one followed by a sequential
	// one.
	for (int i = 0; i < 3; ++i) {
		for (int r = REGION_ROM0 + 2 * i; r < REGION_ROM0 + 2 * i + 2; ++r) {
			gba->waitN16[r] = ROM_WAIT_N[ws[i]];
			gba->waitS16[r] = ROM_WAIT_S[wsSeq[i] + 2 * i];
			gba->waitN32[r] = gba->waitN16[r] + 1 + gba->waitS16[r];
			gba->waitS32[r] = 2 * gba->waitS16[r] + 1;
		}
	}
	if (gba->prefetchEnabled && !(parameters & 0x4000)) {
		gba->prefetchDisabled = true;
		// The read in flight finishes, and that is as far ahead as it gets.
		struct Prefetcher* p = &gba->prefetcher;
		int32_t lead = timingCurrentTime(gba) - p->base;
		if (gba->cpu.prefetching && lead < p->full) {
			p->full = (lead / p->duty + 1) * p->duty;
		}
	}
	gba->prefetchEnabled = parameters & 0x4000;
	updateWritePcCosts(gba);
}

// The undocumented register at 0x04000800: external work RAM's wait states, 2 by default.
void memoryAdjustEwramWaitstates(struct GBA* gba, uint16_t parameters) {
	int wait = 15 - ((parameters >> 8) & 0xF);
	if (!wait) {
		return;
	}
	gba->waitN16[REGION_EWRAM] = gba->waitS16[REGION_EWRAM] = wait;
	gba->waitN32[REGION_EWRAM] = gba->waitS32[REGION_EWRAM] = 2 * wait + 1;
	updateWritePcCosts(gba);
}

// mGBA's model of the prefetch buffer during a data access outside the cartridge, for accesses
// counted its way: the buffer fills while the bus is busy elsewhere, up to eight halfwords, and
// what it loaded is taken off what the following fetches cost.
int32_t memoryStall(struct GBA* gba, int32_t wait) {
	if (gba->activeRegion < REGION_ROM0 || !gba->prefetchEnabled) {
		return wait;
	}
	int32_t previousLoads = 0;
	uint32_t dist = gba->lastPrefetchedPc - gba->cpu.r[15];
	int32_t maxLoads = 8;
	if (dist < 16) {
		previousLoads = dist >> 1;
		maxLoads -= previousLoads;
	}
	int32_t s = gba->waitS16[gba->activeRegion];
	int32_t stall = s + 1;
	int32_t loads = 1;
	while (stall < wait && loads < maxLoads) {
		stall += s;
		++loads;
	}
	gba->lastPrefetchedPc = gba->cpu.r[15] + 2 * (loads + previousLoads - 1);
	if (stall > wait) {
		wait = stall;
	}
	wait -= gba->waitN16[gba->activeRegion] - s;
	wait -= stall;
	return wait;
}

// Open bus: a read of nothing returns what was last on the bus, which is the most recent prefetch.
// In Thumb, the two halves of the word depend on where the code runs and its alignment.
uint32_t memoryLoadBad(struct GBA* gba) {
	struct CPU* cpu = &gba->cpu;
	jitSyncPc(gba);
	bool thumb = cpu->cpsr & PSR_T;
	// Straight after a DMA access, it's the last value DMA moved.
	if (gba->lastAccessDma) {
		return gba->dmaLatch;
	}
	uint32_t value = cpu->prefetch[1];
	if (thumb) {
		switch (cpu->r[15] >> 24) {
		case REGION_BIOS:
		case REGION_OAM:
			value = (value << 16) | cpu->prefetch[0];
			break;
		case REGION_IWRAM:
			if (cpu->r[15] & 2) {
				value = (value << 16) | cpu->prefetch[0];
			} else {
				value |= cpu->prefetch[0] << 16;
			}
			break;
		default:
			value |= value << 16;
		}
	}
	return value;
}

// In the bitmap modes, the mirror of the first 32 KiB above the sprite tiles isn't there: reads
// give 0.
static bool vramHole(struct GBA* gba, uint32_t address) {
	return (address & (SIZE_VRAM | 0x14000)) == SIZE_VRAM && (gba->io[0] & 7) >= 3;
}

// VRAM is 96 KiB in a 128 KiB space; the last 32 KiB mirrors the sprite tiles.
static uint32_t vramOffset(uint32_t address) {
	return (address & 0x1FFFF) >= SIZE_VRAM ? address & 0x17FFF : address & 0x1FFFF;
}

// VRAM changes are tracked by page.
static inline void videoChanged(struct GBA* gba, uint8_t* p, uint8_t what) {
	if (what == RENDER_VRAM) {
		gba->renderer.vramChanged[(p - gba->vram) >> 11] = 1;
	} else {
		gba->renderer.memoryChanged |= what;
	}
}

// Lines whose H-blank has come are drawn before what they show changes.
static inline void drawFirst(struct GBA* gba) {
	if ((int32_t) (timingCurrentTime(gba) - gba->video.drawDue) >= 0) {
		videoSync(gba);
	}
}

// The renderer only hears about these stores when they change something, as in mGBA.
static inline void videoStore16(struct GBA* gba, uint8_t* p, uint16_t value, uint8_t what) {
	uint16_t old;
	memcpy(&old, p, 2);
	if (old != value) {
		drawFirst(gba);
		memcpy(p, &value, 2);
		videoChanged(gba, p, what);
	}
}

static inline void videoStore32(struct GBA* gba, uint8_t* p, uint32_t value, uint8_t what) {
	uint32_t old;
	memcpy(&old, p, 4);
	if (old != value) {
		drawFirst(gba);
		memcpy(p, &value, 4);
		videoChanged(gba, p, what);
	}
}

// After a store, compiled code may cover the word.
static inline void codeWritten(struct GBA* gba, uint32_t address, int size) {
	switch (address >> 24) {
	// RAM code is tracked a word at a time: the map says whether compiled code covers this one.
	case REGION_EWRAM:
		if (gba->ewramCode[(address & (SIZE_EWRAM - 1)) >> 2]) {
			jitCodeWritten(gba, address, size);
		}
		break;
	case REGION_IWRAM:
		if (gba->iwramCode[(address & (SIZE_IWRAM - 1)) >> 2]) {
			jitCodeWritten(gba, address, size);
		}
		break;
	// Code run from video memory is rare enough to look up the slow way.
	case REGION_PALETTE:
	case REGION_VRAM:
	case REGION_OAM:
		if (gba->videoCode && jitCovers(gba, address)) {
			jitInvalidate(gba, address & ~3u, size < 4 ? 4 : size);
		}
		break;
	}
}

// DMA units between RAM, palette, VRAM, OAM and cartridge ROM: the accesses memoryLoad and
// memoryStore make, without the cycle counting.
static inline uint32_t plainLoad(struct GBA* gba, uint32_t address, bool wide) {
	uint32_t align = wide ? ~3u : ~1u;
	const uint8_t* p;
	switch (address >> 24) {
	case REGION_EWRAM:
		p = &gba->ewram[address & (SIZE_EWRAM - 1) & align];
		break;
	case REGION_IWRAM:
		p = &gba->iwram[address & (SIZE_IWRAM - 1) & align];
		break;
	case REGION_PALETTE:
		p = &gba->palette[address & (SIZE_PALETTE - 1) & align];
		break;
	case REGION_VRAM:
		if (vramHole(gba, address)) {
			return 0;
		}
		p = &gba->vram[vramOffset(address) & align];
		break;
	case REGION_OAM:
		p = &gba->oam[address & (SIZE_OAM - 1) & align];
		break;
	default:
		if ((address & (SIZE_ROM - 1) & align) >= gba->romSize) {
			uint32_t value = wide ? (((address & ~3u) >> 1) & 0xFFFF) | ((((address & ~3u) + 2) >> 1) << 16) : (address >> 1) & 0xFFFF;
			if (gba->cart.vfame) {
				value = cartVfameRead(address, wide ? 4 : 2);
			}
			return ror32(value, (address & (wide ? 3 : 1)) << 3);
		}
		p = &gba->rom[address & (SIZE_ROM - 1) & align];
		break;
	}
	return wide ? ror32(le32(p), (address & 3) << 3) : ror32(le16(p), (address & 1) << 3);
}

static inline void plainStore(struct GBA* gba, uint32_t address, uint32_t value, bool wide) {
	uint32_t align = wide ? ~3u : ~1u;
	uint8_t* p = NULL;
	switch (address >> 24) {
	case REGION_EWRAM:
		p = &gba->ewram[address & (SIZE_EWRAM - 1) & align];
		break;
	case REGION_IWRAM:
		p = &gba->iwram[address & (SIZE_IWRAM - 1) & align];
		break;
	case REGION_PALETTE:
		p = &gba->palette[address & (SIZE_PALETTE - 1) & align];
		break;
	case REGION_VRAM:
		if (!vramHole(gba, address)) {
			p = &gba->vram[vramOffset(address) & align];
		}
		break;
	default:
		p = &gba->oam[address & (SIZE_OAM - 1) & align];
		break;
	}
	if (p) {
		uint8_t what = 0;
		switch (address >> 24) {
		case REGION_PALETTE:
			what = RENDER_PALETTE;
			break;
		case REGION_VRAM:
			what = RENDER_VRAM;
			break;
		case REGION_OAM:
			what = RENDER_OAM;
			break;
		}
		if (!what) {
			if (wide) {
				put32(p, value);
			} else {
				put16(p, value);
			}
		} else if (wide) {
			videoStore32(gba, p, value, what);
		} else {
			videoStore16(gba, p, value, what);
		}
	}
	codeWritten(gba, address, wide ? 4 : 2);
}

// Moves up to count DMA units between plain memory, each timed and done as busDmaRead and
// busDmaWrite would. Stops before an access in another region or one that starts a new cartridge
// burst, and once an event has asked DMA to stop. Returns the units moved.
int memoryDmaCopy(struct GBA* gba, struct Dma* dma, int count, bool wide, int32_t sourceStep, int32_t destStep) {
	struct CPU* cpu = &gba->cpu;
	int size = wide ? 4 : 2;
	uint32_t s = dma->nextSource;
	uint32_t d = dma->nextDest;
	unsigned sourceRegion = s >> 24;
	unsigned destRegion = d >> 24;
	bool rom = sourceRegion >= REGION_ROM0;
	int32_t sourceCycles = busCycles(gba, s, size, true);
	int32_t destCycles = busCycles(gba, d, size, true);
	int n = 0;
	for (; n < count && !gba->dmaReenter; ++n) {
		if ((s >> 24) != sourceRegion || (d >> 24) != destRegion || (rom && !(gba->romNext & 0x1FFFF))) {
			break;
		}
		cpu->cycles += sourceCycles;
		if (cpu->cycles >= cpu->nextEvent) {
			processEvents(gba);
		}
		uint32_t latch = plainLoad(gba, s, wide);
		if (rom) {
			gba->romNext = (s + size) & (SIZE_ROM - 1);
			gba->romLatch = gba->romNext;
		}
		if (!wide) {
			latch *= 0x10001;
		}
		dma->latch = latch;
		gba->dmaLatch = latch;
		cpu->cycles += destCycles;
		if (cpu->cycles >= cpu->nextEvent) {
			processEvents(gba);
		}
		plainStore(gba, d, wide ? latch : (uint16_t) (latch >> (8 * (d & 2))), wide);
		gba->lastAccessDma = true;
		s += sourceStep;
		d += destStep;
	}
	dma->nextSource = s;
	dma->nextDest = d;
	return n;
}

// A 32-bit load without the final cycle counting: adds the region's wait from waits (the N or the S
// table) to *wait. Shared by single loads and block transfers, which differ in what they add.
static uint32_t load32Raw(struct GBA* gba, uint32_t address, int* wait, const uint8_t* waits) {
	switch (address >> 24) {
	case REGION_BIOS:
		if (address < SIZE_BIOS) {
			// The BIOS can only be read by code running in it.
			return gba->activeRegion == REGION_BIOS ? le32(&gba->bios[address & ~3u]) : gba->biosPrefetch;
		}
		return memoryLoadBad(gba);
	case REGION_EWRAM:
		*wait += waits[REGION_EWRAM];
		return le32(&gba->ewram[address & (SIZE_EWRAM - 4)]);
	case REGION_IWRAM:
		return le32(&gba->iwram[address & (SIZE_IWRAM - 4)]);
	case REGION_IO:
		return ioRead(gba, address & 0xFFFFFC) | (ioRead(gba, (address & 0xFFFFFE) | 2) << 16);
	case REGION_PALETTE:
		*wait += waits[REGION_PALETTE];
		return le32(&gba->palette[address & (SIZE_PALETTE - 4)]);
	case REGION_VRAM: {
		uint32_t value = vramHole(gba, address) ? 0 : le32(&gba->vram[vramOffset(address) & ~3u]);
		++*wait;
		return value;
	}
	case REGION_OAM:
		return le32(&gba->oam[address & (SIZE_OAM - 4)]);
	case REGION_ROM0 ... REGION_ROM2_EX:
		*wait += waits[address >> 24];
		if ((address & (SIZE_ROM - 4)) < gba->romSize) {
			return le32(&gba->rom[address & (SIZE_ROM - 4)]);
		}
		if (gba->cart.vfame) {
			return cartVfameRead(address, 4);
		}
		// Past the end of the cartridge, the bus holds the address it was given, halved, in each
		// halfword.
		return (((address & ~3u) >> 1) & 0xFFFF) | ((((address & ~3u) + 2) >> 1) << 16);
	case REGION_SRAM:
	case REGION_SRAM_MIRROR: {
		// The 8-bit save memory answers every byte of a wider read with the same byte.
		*wait = gba->waitN16[address >> 24];
		uint32_t value = memoryLoad8(gba, address, NULL);
		value |= value << 8;
		return value | (value << 16);
	}
	default:
		return memoryLoadBad(gba);
	}
}

// The loads and stores the exact CPU and compiled code's slow paths use. With cycles, the access is
// counted into it: wait states, the access itself, and a load's extra internal cycle. Without
// cycles, as for the debugger's and DMA's reads, nothing is counted.
uint32_t memoryLoad32(struct GBA* gba, uint32_t address, int* cycles) {
	int wait = 0;
	uint32_t value = load32Raw(gba, address, &wait, gba->waitN32);
	if (cycles) {
		wait += 2;
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
	return ror32(value, (address & 3) << 3);
}

// A 16-bit load: the aligned halfword, rotated by a byte for an odd address as the CPU sees it.
uint32_t memoryLoad16(struct GBA* gba, uint32_t address, int* cycles) {
	uint32_t value;
	int wait = 0;
	switch (address >> 24) {
	case REGION_BIOS:
		if (address < SIZE_BIOS) {
			value = gba->activeRegion == REGION_BIOS ? le16(&gba->bios[address & ~1u]) : (gba->biosPrefetch >> ((address & 2) * 8)) & 0xFFFF;
		} else {
			value = (memoryLoadBad(gba) >> ((address & 2) * 8)) & 0xFFFF;
		}
		break;
	case REGION_EWRAM:
		value = le16(&gba->ewram[address & (SIZE_EWRAM - 2)]);
		wait = gba->waitN16[REGION_EWRAM];
		break;
	case REGION_IWRAM:
		value = le16(&gba->iwram[address & (SIZE_IWRAM - 2)]);
		break;
	case REGION_IO:
		value = ioRead(gba, address & 0xFFFFFE);
		break;
	case REGION_PALETTE:
		value = le16(&gba->palette[address & (SIZE_PALETTE - 2)]);
		break;
	case REGION_VRAM:
		if (vramHole(gba, address)) {
			value = 0;
			break;
		}
		value = le16(&gba->vram[vramOffset(address) & ~1u]);
		break;
	case REGION_OAM:
		value = le16(&gba->oam[address & (SIZE_OAM - 2)]);
		break;
	case REGION_ROM0 ... REGION_ROM2_EX:
		wait = gba->waitN16[address >> 24];
		if ((address >> 24) == REGION_ROM2_EX && (gba->savedata.type == SAVEDATA_EEPROM || gba->savedata.type == SAVEDATA_EEPROM512)) {
			// An EEPROM sits at the top of the cartridge space and is read a bit at a time.
			value = savedataReadEeprom(gba);
		} else if ((address & (SIZE_ROM - 2)) < gba->romSize) {
			value = le16(&gba->rom[address & (SIZE_ROM - 2)]);
		} else if (gba->cart.vfame) {
			value = cartVfameRead(address, 2);
		} else {
			uint16_t print;
			value = cartPrintRead(gba, address, &print) ? print : (address >> 1) & 0xFFFF;
		}
		break;
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		wait = gba->waitN16[address >> 24];
		value = memoryLoad8(gba, address, NULL);
		value |= value << 8;
		break;
	default:
		value = (memoryLoadBad(gba) >> ((address & 2) * 8)) & 0xFFFF;
		break;
	}
	if (cycles) {
		wait += 2;
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
	return ror32(value, (address & 1) << 3);
}

// An 8-bit load. Save memory is only reached this way, and the first access to it says what kind it
// is.
uint32_t memoryLoad8(struct GBA* gba, uint32_t address, int* cycles) {
	uint32_t value;
	int wait = 0;
	switch (address >> 24) {
	case REGION_BIOS:
		if (address < SIZE_BIOS) {
			value = gba->activeRegion == REGION_BIOS ? gba->bios[address] : (gba->biosPrefetch >> ((address & 3) * 8)) & 0xFF;
		} else {
			value = (memoryLoadBad(gba) >> ((address & 3) * 8)) & 0xFF;
		}
		break;
	case REGION_EWRAM:
		value = gba->ewram[address & (SIZE_EWRAM - 1)];
		wait = gba->waitN16[REGION_EWRAM];
		break;
	case REGION_IWRAM:
		value = gba->iwram[address & (SIZE_IWRAM - 1)];
		break;
	case REGION_IO:
		value = (ioRead(gba, address & 0xFFFFFE) >> ((address & 1) << 3)) & 0xFF;
		break;
	case REGION_PALETTE:
		value = gba->palette[address & (SIZE_PALETTE - 1)];
		break;
	case REGION_VRAM:
		if (vramHole(gba, address)) {
			value = 0;
			break;
		}
		value = gba->vram[vramOffset(address)];
		break;
	case REGION_OAM:
		value = gba->oam[address & (SIZE_OAM - 1)];
		break;
	case REGION_ROM0 ... REGION_ROM2_EX:
		wait = gba->waitN16[address >> 24];
		if ((address & (SIZE_ROM - 1)) < gba->romSize) {
			value = gba->rom[address & (SIZE_ROM - 1)];
		} else if (gba->cart.vfame) {
			value = cartVfameRead(address, 1);
		} else {
			value = ((address >> 1) >> ((address & 1) * 8)) & 0xFF;
		}
		break;
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		wait = gba->waitN16[address >> 24];
		// A read before any Flash command means SRAM.
		if (gba->savedata.type == SAVEDATA_AUTODETECT) {
			savedataInitSram(gba);
		}
		// DMA 0 cannot read the cartridge's save memory.
		if (gba->performingDma == 1) {
			value = 0;
			break;
		}
		switch (gba->savedata.type) {
		case SAVEDATA_SRAM:
			value = gba->savedata.data[address & (SIZE_SRAM - 1)];
			break;
		case SAVEDATA_FLASH512:
		case SAVEDATA_FLASH1M:
			value = savedataReadFlash(gba, address);
			break;
		case SAVEDATA_SRAM512:
			value = gba->savedata.data[address & (SIZE_SRAM512 - 1)];
			break;
		default:
			value = gba->gpio.devices & HW_TILT ? gpioTiltRead(gba, address & 0xFFFFFF) : 0xFF;
			break;
		}
		value &= 0xFF;
		break;
	default:
		value = (memoryLoadBad(gba) >> ((address & 3) * 8)) & 0xFF;
		break;
	}
	if (cycles) {
		wait += 2;
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
	return value;
}

// A 32-bit store without the final cycle counting, like load32Raw. Video memory goes through the
// renderer's change tracking, and any store may have landed on compiled code.
static void store32Raw(struct GBA* gba, uint32_t address, uint32_t value, int* wait, int* cycles, const uint8_t* waits) {
	switch (address >> 24) {
	case REGION_EWRAM:
		put32(&gba->ewram[address & (SIZE_EWRAM - 4)], value);
		*wait += waits[REGION_EWRAM];
		break;
	case REGION_IWRAM:
		put32(&gba->iwram[address & (SIZE_IWRAM - 4)], value);
		break;
	case REGION_IO:
		ioWrite32(gba, address & 0xFFFFFC, value);
		break;
	case REGION_PALETTE:
		videoStore32(gba, &gba->palette[address & (SIZE_PALETTE - 4)], value, RENDER_PALETTE);
		*wait += waits[REGION_PALETTE];
		break;
	case REGION_VRAM:
		if (!vramHole(gba, address)) {
			videoStore32(gba, &gba->vram[vramOffset(address) & ~3u], value, RENDER_VRAM);
		}
		++*wait;
		break;
	case REGION_OAM:
		videoStore32(gba, &gba->oam[address & (SIZE_OAM - 4)], value, RENDER_OAM);
		break;
	case REGION_ROM0 ... REGION_ROM2_EX:
		*wait += waits[address >> 24];
		if (gba->cart.matrix && (address & 0x01FFFF00) == 0x00800100) {
			cartMatrixWrite(gba, address & 0x3C, value);
		}
		break;
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		memoryStore8(gba, address, value >> (8 * (address & 3)), cycles);
		break;
	default:
		break;
	}
	codeWritten(gba, address, 4);
}

void memoryStore32(struct GBA* gba, uint32_t address, uint32_t value, int* cycles) {
	int wait = 0;
	store32Raw(gba, address, value, &wait, cycles, gba->waitN32);
	if (cycles) {
		++wait;
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
}

// A 16-bit store.
void memoryStore16(struct GBA* gba, uint32_t address, uint16_t value, int* cycles) {
	int wait = 0;
	if (((address >> 24) | 1) == (REGION_ROM0 | 1) && (address & 0x00FFFFFF) >= 0x00FD0000 && (address & (SIZE_ROM - 2)) >= gba->romSize) {
		// A development cartridge's debug output.
		cartPrintWrite(gba, address, value);
	}
	switch (address >> 24) {
	case REGION_EWRAM:
		put16(&gba->ewram[address & (SIZE_EWRAM - 2)], value);
		wait = gba->waitN16[REGION_EWRAM];
		break;
	case REGION_IWRAM:
		put16(&gba->iwram[address & (SIZE_IWRAM - 2)], value);
		break;
	case REGION_IO:
		ioWrite(gba, address & 0xFFFFFE, value);
		break;
	case REGION_PALETTE:
		videoStore16(gba, &gba->palette[address & (SIZE_PALETTE - 2)], value, RENDER_PALETTE);
		break;
	case REGION_VRAM:
		if (vramHole(gba, address)) {
			break;
		}
		videoStore16(gba, &gba->vram[vramOffset(address) & ~1u], value, RENDER_VRAM);
		break;
	case REGION_ROM0:
		if ((address & 0xFFFFFE) == 0xC4 || (address & 0xFFFFFE) == 0xC6 || (address & 0xFFFFFE) == 0xC8) {
			if (gba->gpio.devices & (HW_RTC | HW_RUMBLE | HW_LIGHT_SENSOR | HW_GYRO | HW_TILT)) {
				gpioWrite(gba, address & 0xFFFFFE, value);
			}
		}
		if (gba->cart.matrix && (address & 0x01FFFF00) == 0x00800100) {
			cartMatrixWrite16(gba, address & 0x3C, value);
		}
		break;
	case REGION_ROM2_EX:
		if (gba->savedata.type == SAVEDATA_AUTODETECT) {
			savedataInitEeprom(gba);
		}
		if (gba->savedata.type == SAVEDATA_EEPROM512 || gba->savedata.type == SAVEDATA_EEPROM) {
			savedataWriteEeprom(gba, value, 1);
		}
		break;
	case REGION_OAM:
		videoStore16(gba, &gba->oam[address & (SIZE_OAM - 2)], value, RENDER_OAM);
		break;
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		if (address & 1) {
			value >>= 8;
		}
		memoryStore8(gba, address, value, cycles);
		break;
	default:
		break;
	}
	codeWritten(gba, address, 2);
	if (cycles) {
		++wait;
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
}

// An 8-bit store. For save memory, a write to 0x0E005555 starts a Flash command; any other first
// write means SRAM.
void memoryStore8(struct GBA* gba, uint32_t address, uint8_t value, int* cycles) {
	int wait = 0;
	switch (address >> 24) {
	case REGION_EWRAM:
		gba->ewram[address & (SIZE_EWRAM - 1)] = value;
		wait = gba->waitN16[REGION_EWRAM];
		break;
	case REGION_IWRAM:
		gba->iwram[address & (SIZE_IWRAM - 1)] = value;
		break;
	case REGION_IO:
		ioWrite8(gba, address & 0xFFFFFF, value);
		break;
	case REGION_PALETTE:
		memoryStore16(gba, address & ~1u, value | (value << 8), cycles);
		break;
	case REGION_VRAM:
		if ((address & 0x1FFFF) >= ((gba->io[0] & 7) >= 3 ? 0x14000u : 0x10000u)) {
			break;
		}
		videoStore16(gba, &gba->vram[address & 0x1FFFE], value | (value << 8), RENDER_VRAM);
		break;
	case REGION_SRAM:
	case REGION_SRAM_MIRROR:
		if (cartMultiWrite(gba, address & 0xFFFF, value)) {
			break;
		}
		if (gba->savedata.type == SAVEDATA_AUTODETECT) {
			if (address == 0x0E005555) {
				savedataInitFlash(gba);
			} else {
				savedataInitSram(gba);
			}
		}
		switch (gba->savedata.type) {
		case SAVEDATA_FLASH512:
		case SAVEDATA_FLASH1M:
			savedataWriteFlash(gba, address, value);
			break;
		case SAVEDATA_SRAM:
			if (gba->cart.vfame) {
				cartVfameSramWrite(gba, address & 0xFFFF, value);
				break;
			}
			gba->savedata.data[address & (SIZE_SRAM - 1)] = value;
			gba->savedata.dirty = true;
			break;
		case SAVEDATA_SRAM512:
			gba->savedata.data[address & (SIZE_SRAM512 - 1)] = value;
			gba->savedata.dirty = true;
			break;
		default:
			if (gba->gpio.devices & HW_TILT) {
				gpioTiltWrite(gba, address & 0xFFFFFF, value);
			}
			break;
		}
		wait = gba->waitN16[REGION_SRAM];
		break;
	default:
		break;
	}
	codeWritten(gba, address, 1);
	if (cycles) {
		++wait;
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
}

enum {
	// Block transfer directions: before (the address moves first) and down.
	LSM_B = 1,
	LSM_D = 2,
};

// LDM as mGBA counts it: loads the registers in mask from ascending addresses, the first access
// nonsequential and the rest sequential. Returns the address the base register is left with.
uint32_t memoryLoadMultiple(struct GBA* gba, uint32_t address, int mask, int direction, int* cycles) {
	struct CPU* cpu = &gba->cpu;
	int offset = 4;
	int popcount = 0;
	// Going down, the lowest address still comes first.
	if (direction & LSM_D) {
		offset = -4;
		// An empty list moves the address as sixteen registers would.
		popcount = mask ? __builtin_popcount(mask) : 16;
		address -= (popcount << 2) - 4;
	}
	if (direction & LSM_B) {
		address += offset;
	}
	// The transfers are word-aligned; the base keeps its low bits.
	uint32_t misalign = address & 3;
	int region = address >> 24;
	if (region < REGION_SRAM) {
		address &= ~3u;
	}
	// Counted as all sequential, minus the difference for the first.
	int wait = gba->waitS32[region] - gba->waitN32[region];
	// An empty list transfers the PC and moves the base by 64.
	if (!mask) {
		cpu->r[15] = load32Raw(gba, address, &wait, gba->waitS32);
		wait += 16;
		address += 64;
	}
	for (int i = 0; i < 16; ++i) {
		if (mask & (1 << i)) {
			cpu->r[i] = load32Raw(gba, address, &wait, gba->waitS32);
			++wait;
			address += 4;
		}
	}
	if (cycles) {
		++wait;
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
	if (direction & LSM_B) {
		address -= offset;
	}
	if (direction & LSM_D) {
		address -= (popcount << 2) + 4;
	}
	return address | misalign;
}

// STM, counted the same way. A stored PC is 12 ahead of the instruction in ARM.
uint32_t memoryStoreMultiple(struct GBA* gba, uint32_t address, int mask, int direction, int* cycles) {
	struct CPU* cpu = &gba->cpu;
	int offset = 4;
	int popcount = 0;
	if (direction & LSM_D) {
		offset = -4;
		// An empty list moves the address as sixteen registers would.
		popcount = mask ? __builtin_popcount(mask) : 16;
		address -= (popcount << 2) - 4;
	}
	if (direction & LSM_B) {
		address += offset;
	}
	uint32_t misalign = address & 3;
	int region = address >> 24;
	if (region < REGION_SRAM) {
		address &= ~3u;
	}
	int wait = gba->waitS32[region] - gba->waitN32[region];
	if (!mask) {
		store32Raw(gba, address, cpu->r[15] + ((cpu->cpsr & PSR_T) ? 2 : 4), &wait, cycles, gba->waitS32);
		wait += 16;
		address += 64;
	}
	for (int i = 0; i < 16; ++i) {
		if (mask & (1 << i)) {
			uint32_t value = cpu->r[i];
			if (i == 15) {
				value += 4;
			}
			store32Raw(gba, address, value, &wait, cycles, gba->waitS32);
			++wait;
			address += 4;
		}
	}
	if (cycles) {
		if (address < 0x08000000) {
			wait = memoryStall(gba, wait);
		}
		*cycles += wait;
	}
	if (direction & LSM_B) {
		address -= offset;
	}
	if (direction & LSM_D) {
		address -= (popcount << 2) + 4;
	}
	return address | misalign;
}

// A read with no side effects on timing, for the test harness and save states. The BIOS is made
// readable for it.
uint32_t memoryView32(struct GBA* gba, uint32_t address) {
	int wait = 0;
	int region = gba->activeRegion;
	gba->activeRegion = REGION_BIOS;
	uint32_t value = load32Raw(gba, address, &wait, gba->waitN32);
	gba->activeRegion = region;
	return value;
}

// Returns the halfword at address out of its word.
uint16_t memoryView16(struct GBA* gba, uint32_t address) {
	uint32_t value = memoryView32(gba, address & ~3u);
	return address & 2 ? value >> 16 : value;
}
