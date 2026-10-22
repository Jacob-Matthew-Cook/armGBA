/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
// A test ROM for the BIOS's calls: it makes each with pseudo-random and edge-case inputs and records
// the registers it returns, a checksum of the memory it wrote, and how many cycles it took, in EWRAM
// from RESULTS on. Run on two BIOSes, the records are compared with compare.py.
#include <stdbool.h>
#include <stdint.h>

#include "data.h"

#define RESULTS 0x02020000u
#define COUNT (*(volatile uint32_t*) 0x0201FFF8u)
#define DONE (*(volatile uint32_t*) 0x0201FFFCu)
#define WORK 0x02000000u
// The affine calls' full outputs, copied here after each call, then the sine probe's.
#define PROBE 0x02010000u
#define PROBE_END (*(volatile uint32_t*) 0x0201FFF0u)
#define VRAM 0x06000000u
#define IO16(address) (*(volatile uint16_t*) (0x04000000u + (address)))

struct Record {
	uint32_t id;
	uint32_t in[4];
	uint32_t out[4];
	uint32_t cycles;
	uint32_t sum;
};

#define SEED (*(volatile uint32_t*) 0x0201FFF4u)

static uint32_t rnd(void) {
	uint32_t a = SEED * 1103515245u + 12345u;
	uint32_t b = a * 1103515245u + 12345u;
	SEED = b;
	return (a >> 16) | (b & 0xFFFF0000u);
}

// A random number below n, without the compiler's division helpers.
static uint32_t below(uint32_t n) {
	uint32_t v = rnd();
	uint32_t rest = 0;
	for (int bit = 31; bit >= 0; --bit) {
		rest = (rest << 1) | ((v >> bit) & 1);
		if (rest >= n) {
			rest -= n;
		}
	}
	return rest;
}

// A random number of a random size, so small and large values both come up.
static uint32_t anySize(void) {
	uint32_t v = rnd();
	return v >> (rnd() & 31);
}

static uint32_t now(void) {
	uint16_t high, low;
	do {
		high = IO16(0x10C);
		low = IO16(0x108);
	} while (high != IO16(0x10C));
	return (uint32_t) high << 16 | low;
}

static void clear(uint32_t address, uint32_t size) {
	for (uint32_t i = 0; i < size; i += 2) {
		*(volatile uint16_t*) (address + i) = 0;
	}
}

static uint32_t checksum(uint32_t address, uint32_t size) {
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < size; i += 2) {
		h = (h ^ *(volatile uint16_t*) (address + i)) * 16777619u;
	}
	return h;
}

#define SWI(n)                                                                                    \
	static void swi##n(uint32_t r[4]) {                                                           \
		register uint32_t r0 __asm__("r0") = r[0];                                                \
		register uint32_t r1 __asm__("r1") = r[1];                                                \
		register uint32_t r2 __asm__("r2") = r[2];                                                \
		register uint32_t r3 __asm__("r3") = r[3];                                                \
		__asm__ volatile("swi " #n : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "r12", "memory", "cc"); \
		r[0] = r0;                                                                                \
		r[1] = r1;                                                                                \
		r[2] = r2;                                                                                \
		r[3] = r3;                                                                                \
	}

SWI(0x01)
SWI(0x02)
SWI(0x04)
SWI(0x05)
SWI(0x06)
SWI(0x07)
SWI(0x08)
SWI(0x09)
SWI(0x0A)
SWI(0x0B)
SWI(0x0C)
SWI(0x0D)
SWI(0x0E)
SWI(0x0F)
SWI(0x10)
SWI(0x11)
SWI(0x12)
SWI(0x13)
SWI(0x14)
SWI(0x15)
SWI(0x19)
SWI(0x1F)
SWI(0x1A)
SWI(0x1B)
SWI(0x1C)
SWI(0x1D)
SWI(0x1E)
SWI(0x28)
SWI(0x29)
SWI(0x2A)
SWI(0x25)
SWI(0x16)
SWI(0x17)
SWI(0x18)

typedef void (*Call)(uint32_t r[4]);

// Sends a halfword in multiplayer mode as the parent, and returns the first child's.
static uint16_t multiplayerSend(uint16_t value) {
	IO16(0x12A) = value;
	IO16(0x128) |= 0x80;
	while (IO16(0x128) & 0x80) {
	}
	for (volatile int i = 0; i < 20; ++i) {
	}
	return IO16(0x122);
}

// A 32-bit normal-mode transfer on the internal clock at 256 KHz, as a MultiBoot parent does it.
static uint32_t normalSend(uint32_t value) {
	IO16(0x120) = value;
	IO16(0x122) = value >> 16;
	IO16(0x128) |= 0x80;
	while (IO16(0x128) & 0x80) {
	}
	for (volatile int i = 0; i < 20; ++i) {
	}
	return IO16(0x120) | (uint32_t) IO16(0x122) << 16;
}

static void record(uint32_t id, Call call, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t sumAt, uint32_t sumSize) {
	struct Record* rec = (struct Record*) (RESULTS + COUNT * sizeof(struct Record));
	uint32_t r[4] = { a, b, c, d };
	for (int i = 0; i < 4; ++i) {
		rec->in[i] = r[i];
	}
	uint32_t start = now();
	call(r);
	uint32_t end = now();
	rec->id = id;
	for (int i = 0; i < 4; ++i) {
		rec->out[i] = r[i];
	}
	rec->cycles = end - start;
	rec->sum = sumSize ? checksum(sumAt, sumSize) : 0;
	COUNT = COUNT + 1;
}

static void keep(uint32_t address, uint32_t size) {
	for (uint32_t i = 0; i < size; i += 2) {
		*(volatile uint16_t*) (PROBE_END + i) = *(volatile uint16_t*) (address + i);
	}
	PROBE_END = PROBE_END + size;
}

#define LOGEND (*(volatile uint32_t*) 0x0201FFECu)
#define IRQLOG 0x0201C000u
#define BIOSFLAGS (*(volatile uint16_t*) 0x03007FF8u)
#define HANDLER (*(volatile uint32_t*) 0x03007FFCu)
#define STATE 0x02009000u
extern void irqHandler(void);
extern void callWith(uint32_t* block);
extern void swiRegs(uint32_t* block, uint32_t number);
extern void resetWithStack(uint32_t flags, uint32_t stack, uint32_t* out);

// A call that waits: the record's checksum covers the scanline, the interrupt flags, the flags in
// 0x03007FF8 and DISPSTAT as it returns.
static void recordWait(uint32_t id, Call call, uint32_t a, uint32_t b) {
	struct Record* rec = (struct Record*) (RESULTS + COUNT * sizeof(struct Record));
	uint32_t r[4] = { a, b, rnd(), rnd() };
	for (int i = 0; i < 4; ++i) {
		rec->in[i] = r[i];
	}
	uint32_t start = now();
	call(r);
	uint32_t end = now();
	volatile uint16_t* state = (volatile uint16_t*) STATE;
	state[0] = IO16(0x006);
	state[1] = IO16(0x202);
	state[2] = BIOSFLAGS;
	state[3] = IO16(0x004);
	rec->id = id;
	for (int i = 0; i < 4; ++i) {
		rec->out[i] = r[i];
	}
	rec->cycles = end - start;
	rec->sum = checksum(STATE, 8);
	keep(STATE, 8);
	COUNT = COUNT + 1;
}

// Starts timer 0 to overflow, with its interrupt, after about the given number of cycles.
static void timerIn(uint32_t cycles) {
	IO16(0x102) = 0;
	IO16(0x100) = 0x10000 - cycles;
	IO16(0x102) = 0x00C0;
}

static void startClock(void) {
	IO16(0x10A) = 0;
	IO16(0x10E) = 0;
	IO16(0x108) = 0;
	IO16(0x10C) = 0;
	IO16(0x10E) = 0x0084;
	IO16(0x10A) = 0x0080;
}

static void fill(uint32_t address, uint32_t size, uint32_t pattern) {
	for (uint32_t i = 0; i < size; i += 4) {
		*(volatile uint32_t*) (address + i) = pattern ^ i;
	}
}

static uint32_t blobSize(const uint8_t* blob) {
	return blob[1] | blob[2] << 8 | blob[3] << 16;
}

// A source of 16-bit or 32-bit units to copy, in ROM.
#define SOURCE ((uint32_t) packed)

// A scan of one SWI number, when the header's byte 0xBE is set and 0xBF holds the number: the
// registers it leaves, how long it took, and what it changed.
#define SCAN (*(volatile uint8_t*) 0x080000BEu)
#define SCAN_NUMBER (*(volatile uint8_t*) 0x080000BFu)
extern void swiProbe(uint32_t number, uint32_t* out);

static void scan(uint32_t number) {
	volatile uint32_t* out = (volatile uint32_t*) RESULTS;
	fill(0x03000000, 0x7000, 0xA5A50000);
	fill(0x05000000, 0x400, 0x12340000);
	fill(0x06000000, 0x18000, 0x56780000);
	fill(0x07000000, 0x400, 0x9ABC0000);
	fill(0x02030000, 0x10000, 0xC3C30000);
	startClock();
	uint32_t start = now();
	swiProbe(number, (uint32_t*) out);
	uint32_t end = now();
	out[16] = end - start;
	out[17] = checksum(0x03000000, 0x7000);
	out[18] = checksum(0x05000000, 0x400) ^ checksum(0x06000000, 0x18000) * 3 ^ checksum(0x07000000, 0x400) * 5;
	out[19] = checksum(0x04000000, 4) ^ checksum(0x04000008, 0xF8) * 3 ^ checksum(0x04000110, 0x100) * 5;
	out[20] = checksum(0x02000000, 0x1F000) ^ checksum(0x02030000, 0x10000) * 3;
	COUNT = 1;
}

// MidiKey2Freq's results, when the header's byte 0xBE is 2: every key at a few fine adjustments and
// sample rates, then every fine adjustment at a few keys, as words from RESULTS.
static void midiSweep(void) {
	static const uint32_t rates[] = { 0xFFFFFFFF, 0x80000000, 0x12345678, 0x00AC4400 };
	static const uint8_t fines[] = { 0, 1, 128, 255 };
	static const uint8_t keys[] = { 0, 59, 60, 61, 127, 128, 200 };
	volatile uint32_t* out = (volatile uint32_t*) RESULTS;
	volatile uint32_t* wave = (volatile uint32_t*) 0x02030000;
	uint32_t n = 0;
	for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
		wave[1] = rates[r];
		for (unsigned f = 0; f < sizeof(fines) / sizeof(fines[0]); ++f) {
			for (uint32_t key = 0; key < 256; ++key) {
				uint32_t regs[4] = { (uint32_t) wave, key, fines[f], 0 };
				swi0x1F(regs);
				out[n++] = regs[0];
			}
		}
		for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
			for (uint32_t fine = 0; fine < 256; ++fine) {
				uint32_t regs[4] = { (uint32_t) wave, keys[k], fine, 0 };
				swi0x1F(regs);
				out[n++] = regs[0];
			}
		}
	}
	// The other registers, and the time, for a few calls.
	for (uint32_t key = 0; key < 256; key += 51) {
		uint32_t regs[4] = { (uint32_t) wave, key, key ^ 0x5A, 0x12345678 };
		uint32_t start = now();
		swi0x1F(regs);
		out[n++] = now() - start;
		for (int i = 0; i < 4; ++i) {
			out[n++] = regs[i];
		}
	}
	COUNT = n;
}

static void __attribute__((noinline)) callTests(void) {
	for (int i = 0; i < 300; ++i) {
		uint32_t n = anySize(), d = anySize();
		if (rnd() & 1) {
			n = -n;
		}
		if (rnd() & 1) {
			d = -d;
		}
		if (!d) {
			d = 1;
		}
		record(0x06, swi0x06, n, d, rnd(), rnd(), 0, 0);
		record(0x07, swi0x07, d, n, rnd(), rnd(), 0, 0);
	}
	static const uint32_t edges[][2] = { { 0x80000000, 0xFFFFFFFF }, { 0x80000000, 1 }, { 0x7FFFFFFF, 0xFFFFFFFF },
		{ 0, 7 }, { 7, 7 }, { 0xFFFFFFF9, 7 }, { 1, 0x80000000 } };
	for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
		record(0x06, swi0x06, edges[i][0], edges[i][1], 0, 0, 0, 0);
	}

	// Every quotient length, for the timing: 2^j and 2^j - 1 over 1, -1 and 3, both ways round.
	for (uint32_t j = 0; j < 32; ++j) {
		static const uint32_t divisors[] = { 1, 0xFFFFFFFF, 3 };
		for (unsigned k = 0; k < sizeof(divisors) / sizeof(divisors[0]); ++k) {
			record(0x06, swi0x06, 1u << j, divisors[k], 0, 0, 0, 0);
			record(0x06, swi0x06, (1u << j) - 1, divisors[k], 0, 0, 0, 0);
			record(0x06, swi0x06, -(1u << j), divisors[k], 0, 0, 0, 0);
			record(0x07, swi0x07, divisors[k], 1u << j, 0, 0, 0, 0);
		}
	}
	for (int i = 0; i < 300; ++i) {
		record(0x08, swi0x08, i < 20 ? (uint32_t) i * i - (i & 1) : anySize(), rnd(), rnd(), rnd(), 0, 0);
	}
	record(0x08, swi0x08, 0xFFFFFFFF, 0, 0, 0, 0, 0);
	for (int i = 0; i < 300; ++i) {
		uint32_t t = below(0x8001);
		record(0x09, swi0x09, (rnd() & 1) ? -t : t, rnd(), rnd(), rnd(), 0, 0);
	}
	static const uint32_t tangents[] = { 0xFFFF, 0x10000, 0x20000, 0x7FFFFFFF, 0x80000000, 0xFFFEFFFF, 0x12345678 };
	for (unsigned i = 0; i < sizeof(tangents) / sizeof(tangents[0]); ++i) {
		record(0x09, swi0x09, tangents[i], 0, 0, 0, 0, 0);
	}
	for (int i = 0; i < 300; ++i) {
		int32_t x = (int32_t) (below(0x10000)) - 0x8000, y = (int32_t) (below(0x10000)) - 0x8000;
		if (i < 8) {
			x = (i & 1) ? 0 : x;
			y = (i & 2) ? 0 : y;
		}
		record(0x0A, swi0x0A, x, y, rnd(), rnd(), 0, 0);
	}
	for (int i = 0; i < 120; ++i) {
		uint32_t count = below(300);
		uint32_t mode = (rnd() & 1) << 24 | (rnd() & 1) << 26;
		uint32_t bytes = count * ((mode >> 26) & 1 ? 4 : 2);
		clear(WORK, 1600);
		record(0x0B, swi0x0B, SOURCE + (rnd() & 0x7C), WORK + (rnd() & 0x3C), count | mode, rnd(), WORK, bytes + 128);
	}
	for (int i = 0; i < 120; ++i) {
		uint32_t count = below(300);
		uint32_t mode = (rnd() & 1) << 24;
		clear(WORK, 1600);
		record(0x0C, swi0x0C, SOURCE + (rnd() & 0x7C), WORK + (rnd() & 0x3C), count | mode, rnd(), WORK, count * 4 + 160);
	}
	// Other memories, which wait differently, and sources the BIOS may refuse.
	fill(0x02006000, 0x400, 0x13570000);
	fill(0x03004000, 0x400, 0x24680000);
	fill(0x06004000, 0x400, 0x35790000);
	static const uint32_t sources[] = { SOURCE, 0x02006000, 0x03004000, 0x06004000, 0x00000100, 0x01000000, 0x01FFFFF0, 0x10006000 };
	static const uint32_t destinations[] = { 0x02007000, 0x03005000, 0x06008000 };
	for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
		for (unsigned k = 0; k < sizeof(destinations) / sizeof(destinations[0]); ++k) {
			for (uint32_t mode = 0; mode < 4; ++mode) {
				uint32_t bits = (mode & 1) << 24 | (mode & 2) << 25;
				clear(destinations[k], 0x200);
				record(0x0B, swi0x0B, sources[i], destinations[k], 37 | bits, 0x5A5A5A5A, destinations[k], 0x200);
				if (mode < 2) {
					clear(destinations[k], 0x200);
					record(0x0C, swi0x0C, sources[i], destinations[k], 40 | bits, 0x5A5A5A5A, destinations[k], 0x200);
				}
			}
		}
	}
	record(0x0D, swi0x0D, 0, 0, 0, 0, 0, 0);
	for (int i = 0; i < 100; ++i) {
		uint32_t entries = 1 + below(4);
		uint32_t source = WORK + 0x8000;
		for (uint32_t k = 0; k < entries * 20; k += 4) {
			*(volatile uint32_t*) (source + k) = rnd();
		}
		clear(WORK, 128);
		record(0x0E, swi0x0E, source, WORK, entries, rnd(), WORK, 80);
		keep(source, 80);
		keep(WORK, 64);
	}
	// Scales of -0x8000 at the right angles, where a matrix entry is 0x8000, past an int16.
	for (uint32_t angle = 0; angle < 0x10000; angle += 0x4000) {
		volatile int16_t* h = (volatile int16_t*) (WORK + 0x8000);
		*(volatile int32_t*) (WORK + 0x8000) = 0x1234;
		*(volatile int32_t*) (WORK + 0x8004) = -0x5678;
		h[4] = 0x7FFF;
		h[5] = -0x7FFF;
		h[6] = -0x8000;
		h[7] = -0x8000;
		h[8] = angle;
		clear(WORK, 128);
		record(0x0E, swi0x0E, WORK + 0x8000, WORK, 1, 0, WORK, 16);
		clear(WORK, 128);
		record(0x0F, swi0x0F, WORK + 0x800C, WORK, 1, 2, WORK, 8);
	}
	for (int i = 0; i < 100; ++i) {
		uint32_t entries = 1 + below(4);
		uint32_t step = (rnd() & 1) ? 8 : 2;
		uint32_t source = WORK + 0x8000;
		for (uint32_t k = 0; k < entries * 8; k += 4) {
			*(volatile uint32_t*) (source + k) = rnd();
		}
		clear(WORK, 256);
		record(0x0F, swi0x0F, source, WORK, entries, step, WORK, 160);
		keep(source, 32);
		keep(WORK, 128);
	}
	// The angles' sines and cosines: ObjAffineSet with both scales at 1.0 in 1.14 fixed point, then at
	// -1.0, and at a scale that is odd.
	static const uint16_t scales[] = { 0x4000, 0xC000, 0x7FFF };
	for (unsigned k = 0; k < 3; ++k) {
		for (uint32_t angle = 0; angle < 256; ++angle) {
			uint32_t source = WORK + 0x8000;
			*(volatile uint16_t*) source = scales[k];
			*(volatile uint16_t*) (source + 2) = scales[k];
			*(volatile uint16_t*) (source + 4) = angle << 8;
			uint32_t regs[4] = { source, WORK, 1, 2 };
			swi0x0F(regs);
			keep(WORK, 8);
		}
	}
	static const uint8_t widths[][2] = { { 1, 1 }, { 1, 4 }, { 1, 8 }, { 2, 4 }, { 2, 8 }, { 4, 8 }, { 4, 16 },
		{ 8, 16 }, { 8, 32 }, { 1, 32 }, { 2, 2 }, { 4, 4 } };
	for (int i = 0; i < 60; ++i) {
		uint32_t info = WORK + 0x8000;
		uint32_t length = 1 + below(240);
		const uint8_t* w = widths[below(12)];
		*(volatile uint16_t*) info = length;
		*(volatile uint16_t*) (info + 2) = w[0] | w[1] << 8;
		*(volatile uint32_t*) (info + 4) = (i & 1) ? rnd() : rnd() & 0xF;
		clear(WORK, 0x2000);
		record(0x10, swi0x10, SOURCE + (rnd() & 0x7C), WORK, info, rnd(), WORK, 0x2000);
	}
	for (int n = 0; n < 8; ++n) {
		const struct {
			uint32_t id;
			Call call;
			const uint8_t* blob;
			uint32_t dest;
		} runs[] = {
			{ 0x11, swi0x11, lzAll[n], WORK }, { 0x12, swi0x12, lzAll[n], VRAM }, { 0x13, swi0x13, h4All[n], WORK },
			{ 0x13, swi0x13, h8All[n], WORK }, { 0x14, swi0x14, rlAll[n], WORK }, { 0x15, swi0x15, rlAll[n], VRAM },
			{ 0x16, swi0x16, d8All[n], WORK }, { 0x17, swi0x17, d8All[n], VRAM }, { 0x18, swi0x18, d16All[n], WORK },
		};
		for (unsigned k = 0; k < sizeof(runs) / sizeof(runs[0]); ++k) {
			// A pattern under the output shows what's written past it, as padding.
			uint32_t size = blobSize(runs[k].blob) + 64;
			fill(runs[k].dest, (size + 3) & ~3, 0xA5A50000);
			record(runs[k].id, runs[k].call, (uint32_t) runs[k].blob, runs[k].dest, rnd(), rnd(), runs[k].dest, size);
		}
	}
	// SoundBias, from levels on both sides, with the bits above the level set.
	static const uint16_t biases[] = { 0x0200, 0x0000, 0xC200, 0x4123, 0x0001, 0x03FE, 0x83FF };
	for (unsigned i = 0; i < sizeof(biases) / sizeof(biases[0]); ++i) {
		for (uint32_t level = 0; level < 2; ++level) {
			IO16(0x088) = biases[i];
			struct Record* rec = (struct Record*) (RESULTS + COUNT * sizeof(struct Record));
			uint32_t regs[4] = { level ? rnd() | 1 : 0, rnd(), rnd(), rnd() };
			for (int k = 0; k < 4; ++k) {
				rec->in[k] = regs[k];
			}
			uint32_t start = now();
			swi0x19(regs);
			uint32_t end = now();
			rec->id = 0x19;
			for (int k = 0; k < 4; ++k) {
				rec->out[k] = regs[k];
			}
			rec->cycles = end - start;
			rec->sum = IO16(0x088);
			COUNT = COUNT + 1;
		}
	}

	// Interrupts: VBlankIntrWait, IntrWait, Halt, and the interrupt path's latency, logged by the
	// handler with the cycle counter.
	HANDLER = (uint32_t) irqHandler;
	LOGEND = IRQLOG;
	BIOSFLAGS = 0;
	IO16(0x202) = 0xFFFF;
	IO16(0x004) = 0x0008;
	IO16(0x200) = 0x0001;
	IO16(0x208) = 1;
	for (int i = 0; i < 20; ++i) {
		recordWait(0x05, swi0x05, rnd(), rnd());
	}
	recordWait(0x04, swi0x04, 1, 1);
	BIOSFLAGS = 1;
	recordWait(0x04, swi0x04, 0, 1);
	BIOSFLAGS = 0;
	recordWait(0x04, swi0x04, 0, 1);
	IO16(0x200) = 0x0009;
	static const uint16_t delays[] = { 64, 100, 257, 1000, 4096, 30000, 60000 };
	for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); ++i) {
		// IntrWait(1, ...) would discard a timer interrupt taken before it starts waiting.
		recordWait(0x05, swi0x05, 0, 0);
		BIOSFLAGS = 0;
		timerIn(delays[i]);
		recordWait(0x04, swi0x04, 0, 0x08);
		recordWait(0x05, swi0x05, 0, 0);
		timerIn(delays[i]);
		recordWait(0x02, swi0x02, 0, 0);
		recordWait(0x05, swi0x05, 0, 0);
		timerIn(delays[i]);
		for (volatile int spin = 0; spin < 2000; ++spin) {
		}
		// Waiting for more than the interrupt that comes.
		recordWait(0x05, swi0x05, 0, 0);
		BIOSFLAGS = 0;
		timerIn(delays[i]);
		recordWait(0x04, swi0x04, 0, 0x09);
	}
	// The interrupt path alone, with the game in a loop: from starting the timer to the handler, and
	// from the handler back to the game. Recorded as call 0xF0.
	for (uint32_t i = 0; i < 8; ++i) {
		struct Record* rec = (struct Record*) (RESULTS + COUNT * sizeof(struct Record));
		uint32_t delay = 200 + i * 37;
		BIOSFLAGS = 0;
		volatile uint32_t* entry = (volatile uint32_t*) LOGEND;
		uint32_t start = now();
		timerIn(delay);
		while (!(BIOSFLAGS & 0x08)) {
		}
		uint32_t end = now();
		rec->id = 0xF0;
		rec->in[0] = delay;
		rec->in[1] = rec->in[2] = rec->in[3] = 0;
		rec->out[0] = entry[1] - start;
		rec->out[1] = end - entry[1];
		rec->out[2] = entry[0];
		rec->out[3] = 0;
		rec->cycles = 0;
		rec->sum = 0;
		COUNT = COUNT + 1;
	}
	IO16(0x102) = 0;
	IO16(0x208) = 0;
	IO16(0x200) = 0;
	IO16(0x004) = 0;
	*(volatile uint32_t*) 0x0201FFE8u = LOGEND - IRQLOG;
	LOGEND = 0;
}

// RegisterRamReset, except EWRAM, which holds the results: then the memories and the I/O registers
// it may have cleared. It clears IWRAM up to 0x03007E00, so it runs with a shallow stack.
static void __attribute__((noinline)) resetTests(void) {
	// IWRAM cleared with the stack just above it, as Tringo does: the BIOS has 0x2C bytes for itself.
	{
		struct Record* rec = (struct Record*) (RESULTS + COUNT * sizeof(struct Record));
		rec->id = 0x01;
		rec->in[0] = 0x02;
		rec->in[1] = 0x03007E2C;
		rec->in[2] = rec->in[3] = 0;
		resetWithStack(0x02, 0x03007E2C, rec->out);
		rec->cycles = 0;
		rec->sum = 0;
		COUNT = COUNT + 1;
	}
	// Where the registers it leaves point, with the stack in other places.
	static const uint32_t stacks[] = { 0x03006000, 0x03006004, 0x03006010, 0x03006100, 0x03005FF8 };
	static const uint16_t flags[] = { 0x20, 0x40, 0x80, 0xA0, 0x00, 0x100, 0x180 };
	for (unsigned i = 0; i < sizeof(stacks) / sizeof(stacks[0]); ++i) {
		for (unsigned k = 0; k < sizeof(flags) / sizeof(flags[0]); ++k) {
			struct Record* rec = (struct Record*) (RESULTS + COUNT * sizeof(struct Record));
			rec->id = 0x01;
			rec->in[0] = flags[k];
			rec->in[1] = stacks[i];
			rec->in[2] = 0;
			rec->in[3] = 0;
			resetWithStack(flags[k], stacks[i], rec->out);
			rec->cycles = 0;
			rec->sum = 0;
			COUNT = COUNT + 1;
		}
	}
	static const uint16_t resets[] = { 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x9C, 0xFE, 0x00, 0x100, 0x40, 0x40, 0x40, 0x40 };
	static const uint16_t biasesBefore[] = { 0xC123, 0x0000, 0x83FF, 0x4200, 0x0001, 0xC123, 0x83FF, 0x0000, 0x4200, 0x0201, 0x8001, 0x0000, 0x4200, 0x0001, 0x83FF };
	for (unsigned i = 0; i < sizeof(resets) / sizeof(resets[0]); ++i) {
		fill(0x03000000, 0x7000, 0xA5A50000 + i);
		fill(0x05000000, 0x400, 0x12340000 + i);
		fill(0x06000000, 0x18000, 0x56780000 + i);
		fill(0x07000000, 0x400, 0x9ABC0000 + i);
		IO16(0x000) = 0x1F40;
		IO16(0x008) = 0x1234;
		IO16(0x060) = 0x0077;
		IO16(0x128) = 0x2000;
		IO16(0x088) = biasesBefore[i];
		struct Record* rec = (struct Record*) (RESULTS + COUNT * sizeof(struct Record));
		uint32_t regs[4] = { resets[i], rnd(), rnd(), rnd() };
		for (int k = 0; k < 4; ++k) {
			rec->in[k] = regs[k];
		}
		swi0x01(regs);
		startClock();
		rec->id = 0x01;
		for (int k = 0; k < 4; ++k) {
			rec->out[k] = regs[k];
		}
		rec->cycles = 0;
		// Not DISPSTAT or VCOUNT, which depend on how long it took.
		uint32_t h = checksum(0x03000000, 0x7000) ^ checksum(0x05000000, 0x400) ^ checksum(0x06000000, 0x18000);
		h ^= checksum(0x07000000, 0x400) ^ checksum(0x04000000, 4) * 7 ^ checksum(0x04000008, 0xF8) * 3;
		h ^= checksum(0x04000100, 0x110) * 5;
		rec->sum = h;
		keep(0x04000000, 0x210);
		COUNT = COUNT + 1;
	}
}

int main(void) {
	IO16(0x208) = 0;
	IO16(0x10A) = 0;
	IO16(0x10E) = 0;
	IO16(0x108) = 0;
	IO16(0x10C) = 0;
	IO16(0x10E) = 0x0084;
	IO16(0x10A) = 0x0080;
	COUNT = 0;
	DONE = 0;
	SEED = 0x1234567;
	PROBE_END = PROBE;
	if (SCAN == 2) {
		midiSweep();
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 19) {
		// MultiBoot as a game starts it, from GBATEK: the handshake in multiplayer mode with the
		// consoles on the cable, one to three, then SWI 0x25 with the image at 0x08060000, whose
		// length is at 0x0805FFF8. From RESULTS: the tries before the other console answered, the
		// answers along the way, the client data, then the SWI's r0-r3 and time.
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		const volatile uint16_t* image = (const volatile uint16_t*) 0x08060000u;
		uint32_t length = *(volatile uint32_t*) 0x0805FFF8u;
		uint8_t palette = 0xC1;
		IO16(0x134) = 0;
		IO16(0x128) = 0x2003;
		uint32_t tries = 0;
		uint16_t r;
		uint8_t bits;
		bool ready;
		do {
			r = multiplayerSend(0x6200);
			// Every console there answers 0x72 and its bit.
			bits = 0;
			ready = true;
			for (int c = 1; c < 4; ++c) {
				uint16_t a = IO16(0x120 + 2 * c);
				if (a == (0x7200 | 1 << c)) {
					bits |= 1 << c;
				} else if (a != 0xFFFF) {
					ready = false;
				}
			}
			ready &= bits != 0;
			if (!ready) {
				while (IO16(0x006) != 160) {
				}
				while (IO16(0x006) == 160) {
				}
			}
		} while (!ready && ++tries < 2000);
		out[0] = tries;
		out[1] = multiplayerSend(0x6100 | bits);
		for (int i = 0; i < 0x60; ++i) {
			r = multiplayerSend(image[i]);
			if (i == 0 || i == 0x5F) {
				out[2 + (i != 0)] = r;
			}
		}
		out[4] = multiplayerSend(0x6200);
		out[5] = multiplayerSend(0x6200 | bits);
		uint32_t waits = 0;
		uint8_t clients[4];
		do {
			r = multiplayerSend(0x6300 | palette);
			ready = true;
			for (int c = 1; c < 4; ++c) {
				uint16_t a = IO16(0x120 + 2 * c);
				clients[c] = bits & 1 << c ? a : 0xFF;
				ready &= !(bits & 1 << c) || (a & 0xFF00) == 0x7300;
			}
		} while (!ready && ++waits < 1000);
		out[6] = r;
		uint8_t handshake = 0x11 + clients[1] + clients[2] + clients[3];
		out[7] = multiplayerSend(0x6400 | handshake);
		uint32_t* param = (uint32_t*) 0x02031000u;
		uint8_t* bytes = (uint8_t*) param;
		for (int i = 0; i < 0x4C; ++i) {
			bytes[i] = 0;
		}
		bytes[0x14] = handshake;
		bytes[0x19] = clients[1];
		bytes[0x1A] = clients[2];
		bytes[0x1B] = clients[3];
		bytes[0x1C] = palette;
		bytes[0x1E] = bits;
		param[0x20 / 4] = 0x080600C0u;
		param[0x24 / 4] = 0x08060000u + length;
		uint32_t regs[4] = { (uint32_t) param, 1, 0, 0 };
		uint32_t start = now();
		swi0x25(regs);
		uint32_t end = now();
		for (int i = 0; i < 4; ++i) {
			out[8 + i] = regs[i];
		}
		out[12] = end - start;
		COUNT = 13;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 20 || SCAN == 21) {
		// MultiBoot in normal mode to one console, as GBATEK has it: each word sent has the handshake
		// value in its lower half, and the answer comes in the upper half. Then SWI 0x25 with r1 0
		// (256 KHz), or 2 (2 MHz) in mode 21. RESULTS as for mode 19, the answers' upper halves.
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		uint32_t length = *(volatile uint32_t*) 0x0805FFF8u;
		const volatile uint16_t* image = (const volatile uint16_t*) 0x08060000u;
		uint8_t palette = 0xC1;
		IO16(0x134) = 0;
		IO16(0x128) = SCAN == 21 ? 0x1003 : 0x1001;
		uint32_t tries = 0;
		uint32_t r;
		do {
			r = normalSend(0x6200);
			if (r >> 16 != 0x7202) {
				while (IO16(0x006) != 160) {
				}
				while (IO16(0x006) == 160) {
				}
			}
		} while (r >> 16 != 0x7202 && ++tries < 4000);
		out[0] = tries;
		out[1] = normalSend(0x6102) >> 16;
		for (int i = 0; i < 0x60; ++i) {
			r = normalSend(image[i]) >> 16;
			if (i == 0 || i == 0x5F) {
				out[2 + (i != 0)] = r;
			}
		}
		out[4] = normalSend(0x6200) >> 16;
		out[5] = normalSend(0x6202) >> 16;
		uint32_t waits = 0;
		do {
			r = normalSend(0x6300 | palette) >> 16;
		} while ((r & 0xFF00) != 0x7300 && ++waits < 1000);
		out[6] = r;
		uint8_t client = r & 0xFF;
		uint8_t handshake = 0x11 + client + 0xFF + 0xFF;
		out[7] = normalSend(0x6400 | handshake) >> 16;
		uint8_t* bytes = (uint8_t*) 0x02031000u;
		for (int i = 0; i < 0x4C; ++i) {
			bytes[i] = 0;
		}
		bytes[0x14] = handshake;
		bytes[0x19] = client;
		bytes[0x1A] = 0xFF;
		bytes[0x1B] = 0xFF;
		bytes[0x1C] = palette;
		bytes[0x1E] = 2;
		*(uint32_t*) (bytes + 0x20) = 0x080600C0u;
		*(uint32_t*) (bytes + 0x24) = 0x08060000u + length;
		uint32_t regs[4] = { 0x02031000u, SCAN == 21 ? 2 : 0, 0, 0 };
		uint32_t start = now();
		swi0x25(regs);
		uint32_t end = now();
		for (int i = 0; i < 4; ++i) {
			out[8 + i] = regs[i];
		}
		out[12] = end - start;
		COUNT = 13;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 18) {
		// Runs the script at 0x08070000, 12-byte steps of an operation and two values, and writes
		// what it asks for to RESULTS: 1-3 write a byte, halfword or word b at a; 4 calls SWI b
		// with r0 = a (r0-r3 and the time go out); 5 copies b bytes from a out; 6 waits for b
		// VBlanks; 7 waits for scanline b; 8 puts out the lowest of b readings of the halfword at a;
		// 9 calls a function and 10 a SWI with r0-r11 set (see below); 0 ends.
		volatile uint32_t* step = (volatile uint32_t*) 0x08070000u;
		uint32_t out = RESULTS;
		for (;; step += 3) {
			uint32_t a = step[1], b = step[2];
			if (step[0] == 0) {
				break;
			} else if (step[0] == 1) {
				*(volatile uint8_t*) a = b;
			} else if (step[0] == 2) {
				*(volatile uint16_t*) a = b;
			} else if (step[0] == 3) {
				*(volatile uint32_t*) a = b;
			} else if (step[0] == 4) {
				static const Call sound[] = { swi0x1A, swi0x1B, swi0x1C, swi0x1D, swi0x1E };
				uint32_t regs[4] = { a, 0, 0, 0 };
				Call call = b >= 0x1A && b <= 0x1E ? sound[b - 0x1A] : b == 0x28 ? swi0x28 : b == 0x29 ? swi0x29 : b == 0x2A ? swi0x2A : swi0x1F;
				uint32_t start = now();
				call(regs);
				uint32_t end = now();
				for (int i = 0; i < 4; ++i) {
					*(volatile uint32_t*) (out + i * 4) = regs[i];
				}
				*(volatile uint32_t*) (out + 16) = end - start;
				out += 20;
			} else if (step[0] == 10) {
				// SWI b with r0-r11 from the block at a; r0-r12, lr, sp, the CPSR and the time go out.
				uint32_t start = now();
				swiRegs((uint32_t*) a, b);
				uint32_t end = now();
				for (int i = 0; i < 16; ++i) {
					*(volatile uint32_t*) (out + i * 4) = ((volatile uint32_t*) a)[i];
				}
				*(volatile uint32_t*) (out + 64) = end - start;
				out += 68;
			} else if (step[0] == 9) {
				// Calls a function with r0-r3 from the block at a: b is a jump list entry (a list
				// SoundDriverGetJumpList left at 0x02031000), a pointer's address with bit 31 set,
				// or the function itself.
				volatile uint32_t* block = (volatile uint32_t*) a;
				block[4] = b < 0x100 ? ((volatile uint32_t*) 0x02031000u)[b] : b >> 31 ? *(volatile uint32_t*) (b & 0x7FFFFFFFu) : b;
				uint32_t start = now();
				callWith((uint32_t*) block);
				uint32_t end = now();
				for (int i = 0; i < 4; ++i) {
					*(volatile uint32_t*) (out + i * 4) = block[i];
				}
				*(volatile uint32_t*) (out + 16) = end - start;
				out += 20;
			} else if (step[0] == 5) {
				for (uint32_t i = 0; i < b; i += 2) {
					*(volatile uint16_t*) (out + i) = *(volatile uint16_t*) (a + i);
				}
				out += (b + 3) & ~3u;
			} else if (step[0] == 8) {
				// The lowest of b readings of the halfword at a, which for a timer is its reload.
				uint16_t low = 0xFFFF;
				for (uint32_t i = 0; i < b; ++i) {
					uint16_t v = *(volatile uint16_t*) a;
					low = v < low ? v : low;
				}
				*(volatile uint32_t*) out = low;
				out += 4;
			} else if (step[0] == 7) {
				while (IO16(0x006) != b) {
				}
			} else if (step[0] == 6) {
				for (uint32_t i = 0; i < b; ++i) {
					while (IO16(0x006) != 160) {
					}
					while (IO16(0x006) == 160) {
					}
				}
			}
		}
		COUNT = out - RESULTS;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 17) {
		// SoundDriverInit on a work area at 0x02030000, zeroed or (0xA0 of the header set) filled
		// with a pattern: from RESULTS, the area's first 0x1000 bytes, then the I/O registers, then
		// r0-r3 and the time.
		uint32_t area = 0x02030000;
		if (*(volatile uint8_t*) 0x080000A0u) {
			fill(area, 0x1000, 0x5A5A0000);
		} else {
			clear(area, 0x1000);
		}
		uint32_t regs[4] = { area, 0x11111111, 0x22222222, 0x33333333 };
		uint32_t start = now();
		swi0x1A(regs);
		uint32_t end = now();
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		for (uint32_t i = 0; i < 0x1000; i += 4) {
			out[i / 4] = *(volatile uint32_t*) (area + i);
		}
		for (uint32_t i = 0; i < 0x400; i += 2) {
			*(volatile uint16_t*) (RESULTS + 0x1000 + i) = IO16(i);
		}
		for (int i = 0; i < 4; ++i) {
			out[0x500 + i] = regs[i];
		}
		out[0x504] = end - start;
		COUNT = 1;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 16) {
		// When a timer interrupt reaches the handler while a call runs (its number at 0xBF of the
		// header), for each delay from 40 to 299 cycles: the time from starting the timer to the
		// handler, and the call's, as halfword pairs from RESULTS.
		static const uint8_t numbers[] = { 0x0B, 0x0C, 0x06, 0x11, 0x14, 0x13, 0x10 };
		HANDLER = (uint32_t) irqHandler;
		IO16(0x200) = 0x0008;
		IO16(0x208) = 1;
		fill(0x03004000, 0x400, 0x24680000);
		uint32_t info = 0x02030100;
		*(volatile uint16_t*) info = 64;
		*(volatile uint16_t*) (info + 2) = 1 | 4 << 8;
		*(volatile uint32_t*) (info + 4) = 1;
		volatile uint16_t* out = (volatile uint16_t*) RESULTS;
		uint32_t n = 0;
		for (unsigned c = 0; c < sizeof(numbers) / sizeof(numbers[0]); ++c) {
			uint8_t number = numbers[c];
			for (uint32_t delay = 40; delay < 300; ++delay) {
				uint32_t regs[4];
				Call call = swi0x0B;
				switch (number) {
				case 0x0B: regs[0] = 0x03004000; regs[1] = 0x03005000; regs[2] = 64; call = swi0x0B; break;
				case 0x0C: regs[0] = 0x03004000; regs[1] = 0x03005000; regs[2] = 64; call = swi0x0C; break;
				case 0x06: regs[0] = 0x7FFFFFFF; regs[1] = 3; call = swi0x06; break;
				case 0x11: regs[0] = (uint32_t) lzAll[1]; regs[1] = 0x02031000; call = swi0x11; break;
				case 0x14: regs[0] = (uint32_t) rlAll[1]; regs[1] = 0x02031000; call = swi0x14; break;
				case 0x13: regs[0] = (uint32_t) h4All[1]; regs[1] = 0x02031000; call = swi0x13; break;
				default: regs[0] = (uint32_t) packed; regs[1] = 0x02031000; regs[2] = info; call = swi0x10; break;
				}
				regs[3] = 0;
				LOGEND = IRQLOG;
				uint32_t start = now();
				timerIn(delay);
				call(regs);
				uint32_t end = now();
				out[n++] = LOGEND > IRQLOG ? *(volatile uint32_t*) (IRQLOG + 4) - start : 0xFFFF;
				out[n++] = end - start;
			}
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 15) {
		// A decompressor (its number at 0xBF of the header) from the source at 0xA0, into VRAM:
		// r0-r3, the time and a checksum of what it wrote, from RESULTS.
		static const Call calls[] = { swi0x11, swi0x12, swi0x13, swi0x14, swi0x15, swi0x16, swi0x17, swi0x18 };
		clear(0x06000000, 0x1000);
		uint32_t regs[4] = { *(volatile uint32_t*) 0x080000A0u, 0x06000000, 0x5A5A5A5A, 0x5A5A5A5A };
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		uint32_t start = now();
		calls[(SCAN_NUMBER - 0x11) & 7](regs);
		out[4] = now() - start;
		for (int i = 0; i < 4; ++i) {
			out[i] = regs[i];
		}
		out[5] = checksum(0x06000000, 0x1000);
		COUNT = 1;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 14) {
		// Huffman of data.py's first 4-bit set with the bits per symbol at 0xA0 of the header, into
		// VRAM: r0-r3, the time and a checksum of what it wrote, from RESULTS.
		volatile uint8_t* copy = (volatile uint8_t*) 0x02031000;
		for (uint32_t i = 0; i < sizeof(h4_0); ++i) {
			copy[i] = h4_0[i];
		}
		copy[0] = (copy[0] & 0xF0) | *(volatile uint8_t*) 0x080000A0u;
		clear(0x06000000, 0x1000);
		uint32_t regs[4] = { 0x02031000, 0x06000000, 0, 0x5A5A5A5A };
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		uint32_t start = now();
		swi0x13(regs);
		out[4] = now() - start;
		for (int i = 0; i < 4; ++i) {
			out[i] = regs[i];
		}
		out[5] = checksum(0x06000000, 0x1000);
		COUNT = 1;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 13) {
		// The decompressors' times over data.py's timing set, as words from RESULTS: LZ77 to work
		// RAM and VRAM, run-length to both, Diff8 to both, Diff16, then Huffman of 4 and 8 bits.
		static const struct {
			Call call;
			const uint8_t* const* blobs;
			uint32_t dest;
		} runs[] = {
			{ swi0x11, lzTiming, 0x02030000 }, { swi0x12, lzTiming, 0x06000000 },
			{ swi0x14, rlTiming, 0x02030000 }, { swi0x15, rlTiming, 0x06000000 },
			{ swi0x16, d8Timing, 0x02030000 }, { swi0x17, d8Timing, 0x06000000 },
			{ swi0x18, d16Timing, 0x02030000 }, { swi0x13, h4Timing, 0x02030000 },
			{ swi0x13, h8Timing, 0x02030000 },
		};
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		uint32_t n = 0;
		// From the cartridge, then from a copy in work RAM, which waits differently.
		for (int copy = 0; copy < 2; ++copy) {
			for (unsigned r = 0; r < sizeof(runs) / sizeof(runs[0]); ++r) {
				for (unsigned k = 0; k < 24; ++k) {
					uint32_t source = (uint32_t) runs[r].blobs[k];
					if (copy) {
						for (uint32_t i = 0; i < 2048; i += 4) {
							*(volatile uint32_t*) (0x02032000 + i) = *(const uint32_t*) (source + i);
						}
						source = 0x02032000;
					}
					uint32_t regs[4] = { source, runs[r].dest, 0, 0 };
					uint32_t start = now();
					runs[r].call(regs);
					out[n++] = now() - start;
				}
			}
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 12) {
		// BitUnPack with the widths at 0xA0 and 0xA1 of the header, into VRAM: r0-r3, the time and a
		// checksum of what it wrote, from RESULTS.
		volatile uint32_t* info = (volatile uint32_t*) 0x02030100;
		info[0] = 8 | *(volatile uint8_t*) 0x080000A0u << 16 | *(volatile uint8_t*) 0x080000A1u << 24;
		info[1] = 1;
		clear(0x06000000, 0x1000);
		uint32_t regs[4] = { SOURCE, 0x06000000, 0x02030100, 0x5A5A5A5A };
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		uint32_t start = now();
		swi0x10(regs);
		out[4] = now() - start;
		for (int i = 0; i < 4; ++i) {
			out[i] = regs[i];
		}
		out[5] = checksum(0x06000000, 0x1000);
		COUNT = 1;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 11) {
		// BitUnPack's time, as halfwords from RESULTS, for each width pair, length, bias and pattern.
		static const uint8_t widths[] = { 1, 2, 4, 8, 16, 32 };
		static const uint8_t patterns[] = { 0x00, 0xFF, 0x5A, 0x01, 0x80 };
		static const uint32_t biases[] = { 0, 3, 0x80000000, 0x80000005 };
		volatile uint16_t* out = (volatile uint16_t*) RESULTS;
		volatile uint8_t* source = (volatile uint8_t*) 0x02030000;
		volatile uint32_t* info = (volatile uint32_t*) 0x02030100;
		uint32_t n = 0;
		for (unsigned in = 0; in < 4; ++in) {
			for (unsigned outw = 0; outw < 6; ++outw) {
				for (uint32_t length = 1; length <= 9; length += 4) {
					for (unsigned b = 0; b < 4; ++b) {
						for (unsigned p = 0; p < 5; ++p) {
							for (uint32_t k = 0; k < 16; ++k) {
								source[k] = patterns[p];
							}
							info[0] = length | widths[in] << 16 | widths[outw] << 24;
							info[1] = biases[b];
							uint32_t regs[4] = { 0x02030000, 0x02031000, 0x02030100, 0 };
							uint32_t start = now();
							swi0x10(regs);
							out[n++] = now() - start;
						}
					}
				}
			}
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 9 || SCAN == 10) {
		// BgAffineSet's (9) or ObjAffineSet's (10) time for one entry, over scales and centres
		// either side of the multiplier's 8-bit step and every angle, as halfwords from RESULTS.
		static const int16_t scales[] = { 0x40, 0x1FF, -0x50, 0x7000 };
		static const int16_t centres[] = { 0x10, 0x200, -0x30, -0x7F00 };
		volatile uint16_t* out = (volatile uint16_t*) RESULTS;
		volatile int32_t* in = (volatile int32_t*) 0x02030000;
		uint32_t n = 0;
		for (uint32_t k = 0; k < 4096; ++k) {
			in[0] = 0x12345;
			in[1] = -0x5000;
			volatile int16_t* h = (volatile int16_t*) (in + 2);
			h[0] = centres[k & 3];
			h[1] = centres[(k >> 2) & 3];
			h[2] = scales[(k >> 4) & 3];
			h[3] = scales[(k >> 6) & 3];
			h[4] = (k >> 8) * 0x1100 + k * 0x37;
			uint32_t regs[4] = { (uint32_t) in, 0x02031000, 1, 2 };
			if (SCAN == 10) {
				regs[0] = (uint32_t) (h + 2);
			}
			uint32_t start = now();
			if (SCAN == 9) {
				swi0x0E(regs);
			} else {
				swi0x0F(regs);
			}
			out[n++] = now() - start;
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 8) {
		// ArcTan2's result and time for points around the origin, as words from RESULTS.
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		uint32_t n = 0;
		for (int32_t y = -0x900; y <= 0x900; y += 0x60) {
			for (int32_t x = -0x900; x <= 0x900; x += 0x60) {
				uint32_t regs[4] = { x, y, 0, 0 };
				uint32_t start = now();
				swi0x0A(regs);
				out[n++] = now() - start;
				out[n++] = regs[0];
			}
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 7) {
		// ArcTan's time from -0x8000 to 0x8000 in steps of 16, as halfwords from RESULTS.
		volatile uint16_t* out = (volatile uint16_t*) RESULTS;
		uint32_t n = 0;
		for (int32_t i = -0x8000; i <= 0x8000; i += 16) {
			uint32_t regs[4] = { i, 0, 0, 0 };
			uint32_t start = now();
			swi0x09(regs);
			out[n++] = now() - start;
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 6) {
		// Sqrt's time for 0-2999 and for 3000 numbers spread to the top, as halfwords from RESULTS.
		volatile uint16_t* out = (volatile uint16_t*) RESULTS;
		uint32_t n = 0;
		for (uint32_t x = 0; x < 6000; ++x) {
			uint32_t regs[4] = { x < 3000 ? x : (x - 3000) * 1431655u + x * 7, 0, 0, 0 };
			uint32_t start = now();
			swi0x08(regs);
			out[n++] = now() - start;
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 5) {
		// Div's time for every numerator 0-299 over every divisor 1-20, as halfwords from RESULTS.
		volatile uint16_t* out = (volatile uint16_t*) RESULTS;
		uint32_t n = 0;
		for (uint32_t d = 1; d <= 20; ++d) {
			for (uint32_t x = 0; x < 300; ++x) {
				uint32_t regs[4] = { x, d, 0, 0 };
				uint32_t start = now();
				swi0x06(regs);
				out[n++] = now() - start;
			}
		}
		COUNT = n;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 4) {
		// Div (0xBF 6) or DivArm (7) of the words at 0xA0 and 0xA4, with interrupts logged as in 3:
		// r0-r3 and the time from RESULTS.
		HANDLER = (uint32_t) irqHandler;
		LOGEND = IRQLOG;
		IO16(0x004) = 0x0008;
		IO16(0x200) = 0x0001;
		IO16(0x208) = 1;
		uint32_t regs[4] = { *(volatile uint32_t*) 0x080000A0u, *(volatile uint32_t*) 0x080000A4u, 0, 0 };
		volatile uint32_t* out = (volatile uint32_t*) RESULTS;
		uint32_t start = now();
		if (SCAN_NUMBER == 6) {
			swi0x06(regs);
		} else {
			swi0x07(regs);
		}
		out[4] = now() - start;
		for (int i = 0; i < 4; ++i) {
			out[i] = regs[i];
		}
		COUNT = 1;
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	if (SCAN == 3) {
		// As 1, with VBlank interrupts on and logged, to see whether they still come.
		HANDLER = (uint32_t) irqHandler;
		LOGEND = IRQLOG;
		IO16(0x004) = 0x0008;
		IO16(0x200) = 0x0001;
		IO16(0x208) = 1;
	}
	if (SCAN) {
		scan(SCAN_NUMBER);
		DONE = 0x454E4F44;
		for (;;) {
		}
	}
	callTests();
	resetTests();
	DONE = 0x454E4F44;
	for (;;) {
	}
}
