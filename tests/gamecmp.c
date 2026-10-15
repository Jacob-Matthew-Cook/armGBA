/* Copyright (c) 2026 Jacob Cook
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Runs a game on armGBA and mGBA side by side, compares their whole state every frame and, on the
// first difference, finds the cycle where they part.
// usage: gamecmp bios.bin rom.gba frames [skipbios] [input seed]
// SAVE=<file> starts both from a save; SCRIPT=<frame>:<KEYINPUT bits, hex>[:<frames, 6 if left out>],...
// plays those buttons instead of the seed's; PAIR=mgba-ref or ref-armgba brings in the reference interpreter

#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct Snapshot a;
static struct Snapshot b;
// A is the side taken as correct.
static const struct GameEmu* A = &mgbaGame;
static const struct GameEmu* B = &armgbaGame;

static bool differ(bool report) {
	A->snapshot(&a);
	B->snapshot(&b);
	bool same = true;
	for (int i = 0; i < 16; ++i) {
		if (a.r[i] != b.r[i]) {
			same = false;
			if (report) {
				printf("  r%d %s %08X %s %08X\n", i, A->name, a.r[i], B->name, b.r[i]);
			}
		}
	}
	if (a.shifterCarry != b.shifterCarry) {
		same = false;
		if (report) {
			printf("  shifter carry %u/%u\n", a.shifterCarry, b.shifterCarry);
		}
	}
	if (a.cpsr != b.cpsr || a.spsr != b.spsr) {
		same = false;
		if (report) {
			printf("  cpsr %08X/%08X spsr %08X/%08X\n", a.cpsr, b.cpsr, a.spsr, b.spsr);
		}
	}
	if (memcmp(a.bank, b.bank, sizeof(a.bank)) || memcmp(a.bankSpsr, b.bankSpsr, sizeof(a.bankSpsr))) {
		same = false;
		if (report) {
			printf("  banked registers differ\n");
		}
	}
	for (size_t i = 0; i < sizeof(a.io) / 2; ++i) {
		if (a.io[i] != b.io[i]) {
			same = false;
			if (report) {
				printf("  io %03X %s %04X %s %04X\n", (unsigned) i * 2, A->name, a.io[i], B->name, b.io[i]);
			}
		}
	}
	static const struct {
		const char* name;
		size_t offset;
		size_t size;
		uint32_t base;
	} regions[] = {
		{ "IWRAM", offsetof(struct Snapshot, iwram), sizeof(a.iwram), 0x03000000 },
		{ "EWRAM", offsetof(struct Snapshot, ewram), sizeof(a.ewram), 0x02000000 },
		{ "palette", offsetof(struct Snapshot, palette), sizeof(a.palette), 0x05000000 },
		{ "VRAM", offsetof(struct Snapshot, vram), sizeof(a.vram), 0x06000000 },
		{ "OAM", offsetof(struct Snapshot, oam), sizeof(a.oam), 0x07000000 },
	};
	for (size_t r = 0; r < sizeof(regions) / sizeof(*regions); ++r) {
		const uint8_t* x = (const uint8_t*) &a + regions[r].offset;
		const uint8_t* y = (const uint8_t*) &b + regions[r].offset;
		if (memcmp(x, y, regions[r].size)) {
			same = false;
			if (report) {
				int shown = 0;
				for (size_t i = 0; i < regions[r].size && shown < 4; ++i) {
					if (x[i] != y[i]) {
						printf("  %s %08X %s %02X %s %02X\n", regions[r].name, regions[r].base + (uint32_t) i, A->name, x[i], B->name, y[i]);
						++shown;
					}
				}
			}
		}
	}
	if (memcmp(a.frame, b.frame, sizeof(a.frame))) {
		same = false;
		if (report) {
			int shown = 0;
			for (int y = 0; y < 160 && shown < 4; ++y) {
				for (int x = 0; x < 240 && shown < 4; ++x) {
					if (a.frame[y][x] != b.frame[y][x]) {
						printf("  pixel %d,%d %s %04X %s %04X\n", x, y, A->name, a.frame[y][x], B->name, b.frame[y][x]);
						++shown;
					}
				}
			}
		}
	}
	return !same;
}

static unsigned inputSeed;
static struct {
	int at;
	unsigned keys;
	int length;
} steps[256];
static int stepCount;

// Deterministic play: Start and A now and then, the D-pad wandering, each held for a few frames.
static uint16_t keysFor(int frame) {
	if (stepCount) {
		uint16_t keys = 0;
		for (int i = 0; i < stepCount; ++i) {
			if (frame >= steps[i].at && frame < steps[i].at + steps[i].length) {
				keys |= steps[i].keys;
			}
		}
		return keys;
	}
	if (!inputSeed) {
		return 0;
	}
	uint32_t slot = frame / 6;
	uint32_t h = (slot + inputSeed) * 0x9E3779B1u;
	h ^= h >> 15;
	h *= 0x85EBCA77u;
	h ^= h >> 13;
	uint16_t keys = 0;
	if ((h & 0xF) < 3) {
		keys |= 1 << 0;
	}
	if (((h >> 4) & 0x3F) == 0) {
		keys |= 1 << 3;
	}
	if (((h >> 10) & 0x7) == 0) {
		keys |= 1 << 1;
	}
	static const uint16_t dirs[8] = { 0, 0x10, 0x20, 0x40, 0x80, 0x50, 0xA0, 0 };
	keys |= dirs[(h >> 13) & 7];
	return keys;
}

static void resetBoth(void) {
	A->reset();
	B->reset();
}

// A frame ends for each emulator where its run loop next comes round after the line counter passes
// the last line. A game reading the counter just then makes that happen a few cycles sooner for one
// than the other, so the one behind is run up to the other, and they are compared at the same
// moment.
static void frameBoth(void) {
	A->runFrame();
	B->runFrame();
	for (int i = 0; i < 4 && A->now() != B->now(); ++i) {
		int32_t ahead = A->now() - B->now();
		if (ahead > 0) {
			B->runCycles(ahead);
		} else {
			A->runCycles(-ahead);
		}
	}
	if (A->now() == B->now()) {
		// A run loop that ends with the frame can leave events due at that very cycle for its next
		// turn; both run them before they are compared.
		A->runCycles(0);
		B->runCycles(0);
	}
}

static void framesBoth(int frames) {
	for (int f = 0; f < frames; ++f) {
		A->setKeys(keysFor(f));
		B->setKeys(keysFor(f));
		frameBoth();
	}
	A->setKeys(keysFor(frames));
	B->setKeys(keysFor(frames));
}

// Steps both through the next 300000 cycles and reports the first step where they differ.
static int scan(int32_t step) {
	printf("scan from %s %u, %s %u\n", A->name, A->now(), B->name, B->now());
	int32_t t = 0;
	while (t < 300000) {
		A->runCycles(step);
		B->runCycles(step);
		t += step;
		if (A->now() != B->now() || differ(false)) {
			printf("scan: differs around %d cycles in (%s now %u, %s now %u)\n", t, A->name, A->now(), B->name, B->now());
			differ(true);
			struct Snapshot* m = &a;
			bool thumb = m->cpsr & 0x20;
			uint32_t pc = m->r[15] - (thumb ? 2 : 4);
			printf("  %s next pc %08X (%s)\n", A->name, pc, thumb ? "Thumb" : "ARM");
			if ((pc >> 24) == 3) {
				for (int i = -10; i <= 2; ++i) {
					uint32_t at = (pc + i * 4) & 0x7FFC;
					uint32_t word;
					memcpy(&word, &m->iwram[at], 4);
					printf("    %08X: %08X\n", 0x03000000 | at, word);
				}
			}
			return 1;
		}
	}
	printf("scan: no difference found\n");
	return 1;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s bios.bin rom.gba frames [skipbios] [input seed]\n", argv[0]);
		return 1;
	}
	FILE* f = fopen(argv[2], "rb");
	if (!f) {
		fprintf(stderr, "can't open %s\n", argv[2]);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* rom = malloc(size);
	if (fread(rom, 1, size, f) != size) {
		return 1;
	}
	fclose(f);
	int frames = atoi(argv[3]);
	bool skipBios = argc > 4 && atoi(argv[4]);
	inputSeed = argc > 5 ? atoi(argv[5]) : 0;
	// PAIR=mgba-ref checks the reference interpreter against mGBA; PAIR=ref-armgba checks the JIT
	// against it.
	const char* pair = getenv("PAIR");
	if (pair && !strcmp(pair, "mgba-ref")) {
		B = &refGame;
	} else if (pair && !strcmp(pair, "ref-armgba")) {
		A = &refGame;
	}

	if (!A->load(argv[1], rom, size, skipBios) || !B->load(argv[1], rom, size, skipBios)) {
		fprintf(stderr, "setup failed\n");
		return 1;
	}
	if (getenv("SAVE")) {
		FILE* s = fopen(getenv("SAVE"), "rb");
		if (!s) {
			fprintf(stderr, "can't open %s\n", getenv("SAVE"));
			return 1;
		}
		uint8_t* data = malloc(0x20000);
		size_t read = fread(data, 1, 0x20000, s);
		fclose(s);
		A->loadSave(data, read);
		B->loadSave(data, read);
		free(data);
	}
	if (B->saveType() != -1) {
		A->forceSaveType(B->saveType());
	}
	if (getenv("SCRIPT")) {
		char* script = strdup(getenv("SCRIPT"));
		for (char* step = strtok(script, ","); step && stepCount < 256; step = strtok(NULL, ",")) {
			steps[stepCount].length = 6;
			sscanf(step, "%d:%x:%d", &steps[stepCount].at, &steps[stepCount].keys, &steps[stepCount].length);
			++stepCount;
		}
		free(script);
	}

	// FROM=<frame> with STEP=<cycles> scans that frame on the first run, for differences a rerun
	// doesn't repeat.
	int from = getenv("FROM") ? atoi(getenv("FROM")) : -1;
	int bad = -1;
	// LOOSE=1 runs both for the frames without comparing along the way, then lists what differs:
	// for games that part from mGBA early by a cycle or two and are still expected to end up alike.
	if (getenv("LOOSE")) {
		if (getenv("PROBE")) {
			// PROBE=<frame>:<from>:<to>: both run afresh to each cycle count into that frame,
			// showing where each stops.
			int frame = 0;
			int from = 0;
			int to = 0;
			int by = 1;
			sscanf(getenv("PROBE"), "%d:%d:%d:%d", &frame, &from, &to, &by);
			for (int32_t n = from; n <= to; n += by) {
				resetBoth();
				framesBoth(frame);
				uint32_t start = A->now();
				A->runCycles(n);
				B->runCycles(n);
				A->snapshot(&a);
				B->snapshot(&b);
				printf("%d: %s +%u pc %08X sp %08X cpsr %02X | %s +%u pc %08X sp %08X cpsr %02X%s\n", n, A->name, A->now() - start, a.r[15], a.r[13],
				       a.cpsr & 0xFF, B->name, B->now() - start, b.r[15], b.r[13], b.cpsr & 0xFF, differ(false) ? "  DIFFER" : "");
			}
			return 0;
		}
		if (getenv("SAMPLE")) {
			// SAMPLE=<frame>: from there, records where each one's PC is every 500 cycles, for SPAN
			// frames (4).
			framesBoth(atoi(getenv("SAMPLE")));
			int span = getenv("SPAN") ? atoi(getenv("SPAN")) : 4;
			for (int side = 0; side < 2; ++side) {
				const struct GameEmu* e = side ? B : A;
				struct Snapshot* snap = side ? &b : &a;
				printf("%s:", e->name);
				uint32_t last = ~0u;
				int run = 0;
				for (int i = 0; i < span * 562; ++i) {
					e->runCycles(500);
					e->snapshot(snap);
					uint32_t at = snap->r[15] >> 8;
					if (at != last) {
						if (run) {
							printf(" x%d", run);
						}
						printf("\n  %u vc %3u %06Xxx", e->now(), snap->io[3], at);
						run = 0;
					}
					last = at;
					++run;
				}
				printf(" x%d\n", run);
			}
			return 0;
		}
		if (getenv("WATCH")) {
			// WATCH=<register, hex>: shows its value on both at the end of every frame it changes
			// on.
			unsigned reg = strtoul(getenv("WATCH"), NULL, 16) / 2;
			unsigned was[2] = { ~0u, ~0u };
			for (int f = 0; f < frames; ++f) {
				A->setKeys(keysFor(f));
				B->setKeys(keysFor(f));
				frameBoth();
				A->snapshot(&a);
				B->snapshot(&b);
				if (a.io[reg] != was[0] || b.io[reg] != was[1]) {
					printf("frame %d: %s %04X %s %04X\n", f, A->name, a.io[reg], B->name, b.io[reg]);
				}
				was[0] = a.io[reg];
				was[1] = b.io[reg];
			}
			return 0;
		}
		framesBoth(frames);
		A->snapshot(&a);
		B->snapshot(&b);
		for (int i = 0; i < 0x20C / 2; ++i) {
			if (a.io[i] != b.io[i]) {
				printf("  io %03X %s %04X %s %04X\n", i * 2, A->name, a.io[i], B->name, b.io[i]);
			}
		}
		const struct { const char* name; const uint8_t* x; const uint8_t* y; size_t size; } parts[] = {
			{ "iwram", a.iwram, b.iwram, sizeof(a.iwram) }, { "ewram", a.ewram, b.ewram, sizeof(a.ewram) },
			{ "palette", a.palette, b.palette, sizeof(a.palette) }, { "vram", a.vram, b.vram, sizeof(a.vram) },
			{ "oam", a.oam, b.oam, sizeof(a.oam) }, { "frame", (uint8_t*) a.frame, (uint8_t*) b.frame, sizeof(a.frame) },
		};
		for (int p = 0; p < 6; ++p) {
			size_t count = 0;
			size_t first = 0;
			for (size_t i = parts[p].size; i--;) {
				if (parts[p].x[i] != parts[p].y[i]) {
					++count;
					first = i;
				}
			}
			for (size_t i = first, shown = 0; p < 5 && count && i < parts[p].size && shown < 24; ++i) {
				if (parts[p].x[i] != parts[p].y[i]) {
					printf("    %zX: %02X %02X\n", i, parts[p].x[i], parts[p].y[i]);
					++shown;
				}
			}
			printf("  %s: %zu bytes differ, first at %zX\n", parts[p].name, count, first);
		}
		printf("  pc %s %08X %s %08X\n", A->name, a.r[15], B->name, b.r[15]);
		if (getenv("IO")) {
			// IO=1: the second emulator's video registers.
			for (int i = 0; i < 0x60 / 2; ++i) {
				printf("%s%04X", i % 8 ? " " : "\n  ", b.io[i]);
			}
			printf("\n");
		}
		if (getenv("DUMP")) {
			// DUMP=<file>: the second emulator's IWRAM, EWRAM, OAM and VRAM.
			FILE* out = fopen(getenv("DUMP"), "wb");
			fwrite(b.iwram, 1, sizeof(b.iwram), out);
			fwrite(b.ewram, 1, sizeof(b.ewram), out);
			fwrite(b.oam, 1, sizeof(b.oam), out);
			fwrite(b.vram, 1, sizeof(b.vram), out);
			fclose(out);
		}
		return 0;
	}
	for (int frame = 0; frame < frames; ++frame) {
		A->setKeys(keysFor(frame));
		B->setKeys(keysFor(frame));
		if (frame == from) {
			return scan(getenv("STEP") ? atoi(getenv("STEP")) : 1000);
		}
		frameBoth();
		if (differ(false)) {
			bad = frame;
			printf("frame %d differs at its end (%s now %u, %s now %u):\n", frame, A->name, A->now(), B->name, B->now());
			differ(true);
			break;
		}
	}
	if (bad < 0) {
		printf("%d frames match\n", frames);
		return 0;
	}
	// Differences can heal, so scan forward through the frame for the first one.
	if (getenv("SCAN")) {
		resetBoth();
		framesBoth(bad);
		return scan(atoi(getenv("SCAN")));
	}
	// Both agree after `bad` frames; find the first cycle of frame `bad` where they don't.
	resetBoth();
	framesBoth(bad);
	uint32_t start = A->now();
	uint32_t length = 300000;
	int32_t lo = 0;
	int32_t hi = length;
	while (hi - lo > 1) {
		int32_t mid = (lo + hi) / 2;
		resetBoth();
		framesBoth(bad);
		A->runCycles(mid);
		B->runCycles(mid);
		if (differ(false)) {
			hi = mid;
		} else {
			lo = mid;
		}
	}
	resetBoth();
	framesBoth(bad);
	A->runCycles(hi);
	B->runCycles(hi);
	printf("frame %d: first differs %d cycles in (%s now %u, %s now %u, start %u)\n", bad, hi, A->name, A->now(), B->name, B->now(), start);
	differ(true);
	bool thumb = a.cpsr & 0x20;
	printf("  %s next pc %08X (%s), cpsr %08X\n", A->name, a.r[15] - (thumb ? 2 : 4), thumb ? "Thumb" : "ARM", a.cpsr);
	// The step before, where they still agreed.
	resetBoth();
	framesBoth(bad);
	A->runCycles(lo);
	B->runCycles(lo);
	A->snapshot(&a);
	thumb = a.cpsr & 0x20;
	printf("  before: DMA3 %04X%04X -> %04X%04X cnt %04X ctl %04X; DMA1 %04X%04X ctl %04X; DMA2 ctl %04X\n", a.io[0xD6 / 2], a.io[0xD4 / 2], a.io[0xDA / 2], a.io[0xD8 / 2], a.io[0xDC / 2], a.io[0xDE / 2],
	       a.io[0xBE / 2], a.io[0xBC / 2], a.io[0xC6 / 2], a.io[0xD2 / 2]);
	printf("  before: next pc %08X, regs", a.r[15] - (thumb ? 2 : 4));
	for (int i = 0; i < 16; ++i) {
		printf(" %08X", a.r[i]);
	}
	printf("\n");
	return 1;
}
