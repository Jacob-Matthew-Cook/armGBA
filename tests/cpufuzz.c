/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Runs random ARM and Thumb programs on armGBA and on mGBA until the same cycle deadline and compares
// registers, banked registers, memory and elapsed cycles.
// usage: cpufuzz bios.bin [tests] [seed]

#include "oracle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	ROM_SIZE = 0x100000,
	MAX_PROGRAM = 48,
};

static uint64_t rngState;

static uint32_t rnd(void) {
	rngState ^= rngState << 13;
	rngState ^= rngState >> 7;
	rngState ^= rngState << 17;
	return rngState >> 11;
}

static uint32_t pick(uint32_t n) {
	return rnd() % n;
}

static bool chance(int percent) {
	return pick(100) < (uint32_t) percent;
}

// Register roles keep memory accesses inside the data window.
struct Layout {
	bool thumb;
	bool smc;
	uint32_t codeBase;
	uint32_t dataBase;
	int count;
	size_t size;
	uint32_t code[512];
};

static const uint32_t modes[] = { 0x10, 0x11, 0x12, 0x13, 0x17, 0x1B, 0x1F };
static const int armDest[] = { 0, 1, 2, 3, 4, 5, 6, 7, 14, 0 };
static const int armPtr[] = { 8, 9, 10, 11 };
static const int thumbDest[] = { 0, 1, 2, 3, 4 };
static const int thumbPtr[] = { 6, 7 };

static int armAny(void) {
	int r = pick(16);
	return r;
}

static int armSrc(void) {
	return chance(10) ? 15 : (int) pick(15);
}

static uint32_t armCond(void) {
	return chance(60) ? 0xE : pick(15);
}

static uint32_t armShifterOperand(bool allowRegShift) {
	if (chance(40)) {
		return 0x02000000 | (pick(16) << 8) | pick(256);
	}
	uint32_t op = armSrc() | (pick(4) << 5);
	if (allowRegShift && chance(30)) {
		return op | 0x10 | ((uint32_t) armSrc() << 8);
	}
	return op | (pick(32) << 7);
}

static uint32_t genArm(struct Layout* layout, int index) {
	uint32_t cond = armCond() << 28;
	int kind = pick(100);
	if (kind < 45) {
		uint32_t op = pick(16);
		uint32_t s = (op >= 8 && op <= 11) ? 1 : pick(2);
		int rd = armDest[pick(10)];
		return cond | (op << 21) | (s << 20) | ((uint32_t) armSrc() << 16) | ((uint32_t) rd << 12) | armShifterOperand(true);
	}
	if (kind < 52) {
		int variant = pick(6);
		int rd = armDest[pick(10)];
		int rn = armDest[pick(10)];
		int rs = armAny();
		int rm = armAny();
		if (rs == 15) rs = 0;
		if (rm == 15) rm = 1;
		uint32_t s = pick(2) << 20;
		if (variant < 2) {
			return cond | (variant << 21) | s | ((uint32_t) rd << 16) | ((uint32_t) rn << 12) | ((uint32_t) rs << 8) | 0x90 | rm;
		}
		while (rn == rd) {
			rn = armDest[pick(10)];
		}
		return cond | 0x00800000 | ((variant - 2) << 21) | s | ((uint32_t) rd << 16) | ((uint32_t) rn << 12) | ((uint32_t) rs << 8) | 0x90 | rm;
	}
	if (kind < 72) {
		int rn = armPtr[pick(4)];
		bool load = pick(2);
		int rd = load ? armDest[pick(10)] : armAny();
		uint32_t p = pick(2), u = pick(2), w = pick(2), b = pick(2);
		if (!p) {
			w = 0;
		}
		uint32_t offset;
		if (chance(70)) {
			offset = pick(64) * (b ? 1 : 4) + (chance(10) ? pick(4) : 0);
		} else {
			offset = 0x02000000 | 12 | (pick(3) << 7);
		}
		if (load && (rd == rn)) {
			rd = 0;
		}
		return cond | 0x04000000 | (p << 24) | (u << 23) | (b << 22) | (w << 21) | ((uint32_t) load << 20) | ((uint32_t) rn << 16) | ((uint32_t) rd << 12) | offset;
	}
	if (kind < 80) {
		int rn = armPtr[pick(4)];
		bool load = pick(2);
		int sh = load ? 1 + pick(3) : 1;
		int rd = load ? armDest[pick(10)] : armAny();
		uint32_t p = pick(2), u = pick(2), w = pick(2);
		if (!p) {
			w = 0;
		}
		if (load && rd == rn) {
			rd = 0;
		}
		uint32_t offset = chance(70) ? (0x00400000 | ((pick(64) & 0xF0) << 4) | (pick(16) & 0xE)) : 12;
		return cond | (p << 24) | (u << 23) | (w << 21) | ((uint32_t) load << 20) | ((uint32_t) rn << 16) | ((uint32_t) rd << 12) | offset | 0x90 | (sh << 5);
	}
	if (kind < 88) {
		int rn = armPtr[pick(4)];
		bool load = pick(2);
		uint32_t list = rnd() & 0x5FFF;
		if (load) {
			list &= ~(0x3F00u | 0x8000u);
		}
		list &= ~(1u << rn);
		if (!list) {
			list = 1;
		}
		uint32_t p = pick(2), u = pick(2), w = pick(2);
		return cond | 0x08000000 | (p << 24) | (u << 23) | (w << 21) | ((uint32_t) load << 20) | ((uint32_t) rn << 16) | list;
	}
	if (kind < 91) {
		int rn = armPtr[pick(4)];
		return cond | 0x01000090 | (pick(2) << 22) | ((uint32_t) rn << 16) | ((uint32_t) armDest[pick(10)] << 12) | (uint32_t) armSrc() % 15;
	}
	if (kind < 94) {
		return cond | 0x010F0000 | (pick(2) << 22) | ((uint32_t) armDest[pick(10)] << 12);
	}
	if (kind < 96) {
		// Flags only.
		if (chance(50)) {
			return cond | 0x0328F000 | (pick(16) << 8) | pick(256);
		}
		return cond | 0x0128F000 | (uint32_t) armDest[pick(10)];
	}
	// A forward branch inside the program.
	int remaining = layout->count - index;
	int skip = pick(remaining > 4 ? 4 : remaining);
	return cond | 0x0A000000 | (pick(2) << 24) | ((uint32_t) (skip - 1) & 0xFFFFFF);
}

static uint16_t genThumb(struct Layout* layout, int index, bool* pair) {
	int td = thumbDest[pick(5)];
	int ts = pick(8);
	int kind = pick(100);
	if (kind < 10) {
		return (pick(3) << 11) | (pick(32) << 6) | (ts << 3) | td;
	}
	if (kind < 18) {
		return 0x1800 | (pick(4) << 9) | (pick(8) << 6) | (ts << 3) | td;
	}
	if (kind < 28) {
		return 0x2000 | (pick(4) << 11) | (td << 8) | pick(256);
	}
	if (kind < 42) {
		return 0x4000 | (pick(16) << 6) | (ts << 3) | td;
	}
	if (kind < 48) {
		// High registers, without writing SP or PC.
		static const int hiDest[] = { 0, 1, 2, 3, 4, 14 };
		int op = pick(3);
		int hd = op == 1 ? (int) pick(16) : hiDest[pick(6)];
		int hm = pick(16);
		return 0x4400 | (op << 8) | ((hd & 8) << 4) | (hm << 3) | (hd & 7);
	}
	if (kind < 52) {
		return 0x4800 | (td << 8) | pick(256);
	}
	if (kind < 62) {
		int rn = thumbPtr[pick(2)];
		int form = pick(8);
		int rd = (form >= 3) ? td : (int) pick(8);
		return 0x5000 | (form << 9) | (5 << 6) | (rn << 3) | rd;
	}
	if (kind < 74) {
		int rn = thumbPtr[pick(2)];
		int form = pick(6);
		int rd = (form & 1) ? td : (int) pick(8);
		return ((0x0C + form) << 11) | (pick(32) << 6) | (rn << 3) | rd;
	}
	if (kind < 78) {
		bool load = pick(2);
		return 0x9000 | (load << 11) | ((load ? td : (int) pick(8)) << 8) | pick(256);
	}
	if (kind < 82) {
		return 0xA000 | (pick(2) << 11) | (td << 8) | pick(256);
	}
	if (kind < 84) {
		return 0xB000 | (pick(2) << 7) | pick(32);
	}
	if (kind < 88) {
		bool load = pick(2);
		if (load) {
			int list = (rnd() & 0x1F) | 1;
			return 0xBC00 | list;
		}
		return 0xB400 | (pick(2) << 8) | (rnd() & 0xFF);
	}
	if (kind < 92) {
		int rn = thumbPtr[pick(2)];
		bool load = pick(2);
		int list = load ? ((rnd() & 0x1F) | 1) : ((rnd() & 0xFF) | 1);
		return 0xC000 | (load << 11) | (rn << 8) | list;
	}
	int remaining = layout->count - index;
	if (kind < 97) {
		int skip = pick(remaining > 4 ? 4 : remaining);
		return 0xD000 | (pick(14) << 8) | ((skip - 1) & 0xFF);
	}
	if (kind < 99 || remaining < 3) {
		int skip = pick(remaining > 4 ? 4 : remaining);
		return 0xE000 | ((skip - 1) & 0x7FF);
	}
	*pair = true;
	return 0xF000;
}

// Generates a segment of one instruction set. Templates add indirect jumps, BIOS calls, mode
// switches and self-modifying stores. Branches never land inside a template or on the second half
// of a BL.
struct Segment {
	bool thumb;
	uint32_t base;
	int count;
	uint32_t ops[MAX_PROGRAM + 8];
	bool fixed[MAX_PROGRAM + 8];
	bool inner[MAX_PROGRAM + 8];
	int jumpTarget[MAX_PROGRAM + 8];
	bool plain[MAX_PROGRAM + 8];
	int jumpLength[MAX_PROGRAM + 8];
	uint32_t pool;
};

static uint32_t armAddPc(int rd, uint32_t from, uint32_t to) {
	// rd = PC + offset, the offset a multiple of 4 up to 1020.
	return 0xE28F0F00 | (rd << 12) | ((to - (from + 8)) >> 2);
}

static uint16_t thumbAddPc(int rd, uint32_t from, uint32_t to) {
	return 0xA000 | (rd << 8) | ((to - ((from + 4) & ~3u)) >> 2);
}

static void markTemplate(struct Segment* seg, int start, int length) {
	for (int i = start; i < start + length; ++i) {
		seg->fixed[i] = true;
		seg->inner[i] = i != start;
	}
}

static void markJump(struct Segment* seg, int start, int length, int target) {
	markTemplate(seg, start, length);
	seg->jumpTarget[start] = target;
	seg->jumpLength[start] = length;
}

static bool isSecondHalf(const struct Segment* seg, int t) {
	return seg->thumb && t > 0 && (seg->ops[t] & 0xF800) == 0xF800 && (seg->ops[t - 1] & 0xF800) == 0xF000;
}

static int branchTarget(const struct Segment* seg, int i) {
	uint32_t op = seg->ops[i];
	if (!seg->thumb) {
		if ((op & 0x0E000000) == 0x0A000000 && (op >> 28) != 0xF) {
			return i + 2 + (((int32_t) (op << 8)) >> 8);
		}
		return -1;
	}
	if ((op & 0xF000) == 0xD000 && (op & 0x0F00) < 0x0E00) {
		return i + 2 + (int8_t) op;
	}
	if ((op & 0xF800) == 0xE000) {
		return i + 2 + (((int32_t) (op << 21)) >> 21);
	}
	if ((op & 0xF800) == 0xF800 && i > 0 && (seg->ops[i - 1] & 0xF800) == 0xF000) {
		return i + 1 + (op & 0x7FF);
	}
	return -1;
}

static void fixBranches(struct Segment* seg) {
	// Indirect jumps whose target turned out bad become no-ops.
	for (int i = 0; i < seg->count; ++i) {
		int t = seg->jumpLength[i] ? seg->jumpTarget[i] : -1;
		if (t < 0 || t >= seg->count || !(seg->inner[t] || isSecondHalf(seg, t))) {
			continue;
		}
		for (int j = i; j < i + seg->jumpLength[i]; ++j) {
			seg->ops[j] = seg->thumb ? 0x46C0 : 0xE1A00000;
			seg->fixed[j] = false;
			seg->inner[j] = false;
		}
		seg->jumpLength[i] = 0;
	}
	for (int i = 0; i < seg->count; ++i) {
		int t = branchTarget(seg, i);
		if (t < 0 || t >= seg->count) {
			continue;
		}
		bool bad = seg->inner[t];
		if (seg->thumb && (seg->ops[t] & 0xF800) == 0xF800 && t > 0 && (seg->ops[t - 1] & 0xF800) == 0xF000) {
			bad = true;
		}
		if (!bad) {
			continue;
		}
		if (seg->thumb) {
			if ((seg->ops[i] & 0xF800) == 0xF800) {
				// The second half of a BL: make the pair a no-op pair.
				seg->ops[i - 1] = 0x46C0;
				seg->ops[i] = 0x46C0;
			} else {
				seg->ops[i] = 0x46C0;
			}
		} else {
			seg->ops[i] = 0xE1A00000;
		}
	}
}

static void armSegment(struct Segment* seg, bool smc, int reserved) {
	int n = seg->count - reserved;
	struct Layout fake = { .count = n };
	for (int i = 0; i < n; ++i) {
		int r = seg->plain[i] ? 100 : (int) pick(100);
		int rx = pick(8);
		uint32_t at = seg->base + i * 4;
		if (r < 5 && i + 2 < n && !seg->plain[i + 1]) {
			int target = i + 2 + pick(n - i - 1 > 4 ? 4 : n - i - 1);
			seg->ops[i] = armAddPc(rx, at, seg->base + target * 4);
			switch (pick(4)) {
			case 0:
				seg->ops[i + 1] = 0xE1A0F000 | rx;
				break;
			case 1:
				seg->ops[i + 1] = 0xE12FFF10 | rx;
				break;
			case 2:
				seg->ops[i + 1] = 0xE280F000 | (rx << 16);
				break;
			default:
				seg->ops[i + 1] = (armCond() << 28) | 0x01A0F000 | rx;
				break;
			}
			markJump(seg, i, 2, target);
			i += 1;
		} else if (r < 8 && i + 4 < n && !seg->plain[i + 1] && !seg->plain[i + 2] && !seg->plain[i + 3]) {
			int target = i + 4 + pick(n - i - 3 > 4 ? 4 : n - i - 3);
			int ptr = armPtr[pick(4)];
			seg->ops[i] = 0xE3C00003 | (ptr << 16) | (ptr << 12);
			seg->ops[i + 1] = armAddPc(rx, at + 4, seg->base + target * 4);
			if (pick(2)) {
				uint32_t offset = pick(16) * 4;
				seg->ops[i + 2] = 0xE5800000 | (ptr << 16) | (rx << 12) | offset;
				seg->ops[i + 3] = 0xE590F000 | (ptr << 16) | offset;
			} else {
				seg->ops[i + 2] = 0xE5800004 | (ptr << 16) | (rx << 12);
				seg->ops[i + 3] = 0xE9908000 | (ptr << 16);
			}
			markJump(seg, i, 4, target);
			i += 3;
		} else if (r < 10 && i + 1 < n && !seg->plain[i + 1]) {
			static const uint32_t calls[] = { 0x06, 0x07, 0x08, 0x09, 0x0A };
			seg->ops[i] = 0xE3A01000 | (1 + pick(255));
			seg->ops[i + 1] = 0xEF000000 | (calls[pick(5)] << 16);
			markTemplate(seg, i, 2);
			i += 1;
		} else if (r < 11) {
			seg->ops[i] = 0xE321F0C0 | modes[pick(7)];
			markTemplate(seg, i, 1);
		} else if (smc && r < 15 && i + 4 < n && !seg->plain[i + 1] && !seg->plain[i + 2]) {
			// Copies the pool word over a later plain instruction, sometimes one already
			// prefetched.
			int target = i + 3 + pick(n - i - 3 > 5 ? 5 : n - i - 3);
			seg->plain[target] = true;
			int ry = (rx + 1 + pick(7)) & 7;
			seg->ops[i] = armAddPc(rx, at, seg->base + target * 4);
			seg->ops[i + 1] = 0xE59F0000 | (ry << 12) | (seg->pool - (at + 4 + 8));
			seg->ops[i + 2] = 0xE5800000 | (rx << 16) | (ry << 12);
			markTemplate(seg, i, 3);
			i += 2;
		} else {
			seg->ops[i] = genArm(&fake, i);
		}
	}
	fixBranches(seg);
}

static void thumbSegment(struct Segment* seg, bool smc, int reserved) {
	int n = seg->count - reserved;
	struct Layout fake = { .count = n };
	for (int i = 0; i < n; ++i) {
		int r = seg->plain[i] ? 100 : (int) pick(100);
		int rx = thumbDest[pick(5)];
		uint32_t at = seg->base + i * 2;
		uint32_t pcBase = (at + 4) & ~3u;
		int target = -1;
		for (int tries = 0; tries < 6 && target < 0; ++tries) {
			int t = i + 3 + pick(6);
			uint32_t address = seg->base + t * 2;
			if (t <= n && !(address & 2) && address >= pcBase) {
				target = t;
			}
		}
		bool room2 = i + 1 < n && !seg->plain[i + 1];
		bool room3 = room2 && i + 2 < n && !seg->plain[i + 2];
		if (r < 6 && target >= 0 && room2) {
			seg->ops[i] = thumbAddPc(rx, at, seg->base + target * 2);
			seg->ops[i + 1] = 0x4687 | (rx << 3);
			markJump(seg, i, 2, target);
			i += 1;
		} else if (r < 10 && target >= 0 && room3) {
			seg->ops[i] = thumbAddPc(rx, at, seg->base + target * 2);
			seg->ops[i + 1] = 0xB400 | (1 << rx);
			seg->ops[i + 2] = 0xBD00;
			markJump(seg, i, 3, target);
			i += 2;
		} else if (r < 13 && room2) {
			static const uint32_t calls[] = { 0x06, 0x07, 0x08, 0x09, 0x0A };
			seg->ops[i] = 0x2100 | (1 + pick(255));
			seg->ops[i + 1] = 0xDF00 | calls[pick(5)];
			markTemplate(seg, i, 2);
			i += 1;
		} else if (smc && r < 18 && room3 && i + 4 < n) {
			int t = i + 3 + pick(n - i - 3 > 5 ? 5 : n - i - 3);
			seg->plain[t] = true;
			uint32_t address = seg->base + t * 2;
			int ry = rx == 0 ? 1 : 0;
			seg->ops[i] = thumbAddPc(rx, at, address & ~3u);
			seg->ops[i + 1] = 0x4800 | (ry << 8) | ((seg->pool - ((at + 2 + 4) & ~3u)) >> 2);
			seg->ops[i + 2] = 0x8000 | ((address & 2) << 5) | (rx << 3) | ry;
			markTemplate(seg, i, 3);
			i += 2;
		} else {
			bool pair = false;
			uint16_t op = genThumb(&fake, i, &pair);
			if (pair && room2 && !seg->plain[i]) {
				int remaining = n - i - 2;
				int skip = pick(remaining > 3 ? 4 : remaining + 1);
				seg->ops[i] = 0xF000;
				seg->ops[i + 1] = 0xF800 | (skip & 0x7FF);
				++i;
			} else {
				seg->ops[i] = pair ? 0x46C0 : op;
			}
		}
	}
	fixBranches(seg);
}

static void emitSegment(struct Layout* layout, struct Segment* seg) {
	uint8_t* bytes = (uint8_t*) layout->code;
	int width = seg->thumb ? 2 : 4;
	size_t at = seg->base - layout->codeBase;
	for (int i = 0; i < seg->count; ++i) {
		memcpy(&bytes[at + i * width], &seg->ops[i], width);
	}
	uint32_t terminator = seg->thumb ? 0xE7FEE7FE : 0xEAFFFFFE;
	for (int i = 0; i < 2; ++i) {
		memcpy(&bytes[at + (seg->count + i) * width], &terminator, width);
	}
	uint32_t poolWord = seg->thumb ? 0x30013001 : 0xE2800001;
	memcpy(&bytes[seg->pool - layout->codeBase], &poolWord, 4);
	size_t end = seg->pool - layout->codeBase + 4;
	if (end > layout->size) {
		layout->size = end;
	}
}

// One segment, or two joined by an ARM-Thumb switch at the end of the first.
static void generate(struct Layout* layout) {
	memset(layout->code, 0, sizeof(layout->code));
	layout->size = 0;
	struct Segment first = { .thumb = layout->thumb, .base = layout->codeBase, .count = 6 + pick(MAX_PROGRAM - 6) };
	int width = first.thumb ? 2 : 4;
	first.pool = (first.base + (first.count + 2) * width + 3) & ~3u;
	bool interwork = chance(25);
	int reserved = interwork ? (first.thumb ? 2 : 6) : 0;
	if (first.thumb) {
		thumbSegment(&first, layout->smc, reserved);
	} else {
		armSegment(&first, layout->smc, reserved);
	}
	struct Segment second = { .thumb = !first.thumb };
	if (interwork) {
		second.base = first.pool + 4;
		second.count = 4 + pick(MAX_PROGRAM / 2);
		int width2 = second.thumb ? 2 : 4;
		second.pool = (second.base + (second.count + 2) * width2 + 3) & ~3u;
		int n = first.count;
		if (first.thumb) {
			uint32_t at = first.base + (n - 2) * 2;
			first.ops[n - 2] = thumbAddPc(0, at, second.base);
			first.ops[n - 1] = 0x4700;
		} else {
			// Thumb's small offset register and pointers come from ARM's.
			first.ops[n - 6] = 0xE3A05000 | (pick(16) * 4);
			first.ops[n - 5] = 0xE1A06008;
			first.ops[n - 4] = 0xE1A07009;
			first.ops[n - 3] = armAddPc(0, first.base + (n - 3) * 4, second.base);
			first.ops[n - 2] = 0xE2800001;
			first.ops[n - 1] = 0xE12FFF10;
		}
		if (second.thumb) {
			thumbSegment(&second, layout->smc, 0);
		} else {
			armSegment(&second, layout->smc, 0);
		}
		emitSegment(layout, &second);
	}
	emitSegment(layout, &first);
	layout->count = first.count + (interwork ? second.count : 0);
}

static uint8_t* rom;

static const char* const regionNames[16] = { "BIOS", "?", "EWRAM", "IWRAM", "IO", "PAL", "VRAM", "OAM", "ROM", "ROM", "ROM", "ROM", "ROM", "ROM", "SRAM", "SRAM" };

static void dump(const struct Layout* layout, uint16_t waitcnt, int32_t deadline) {
	printf("  %s start at %08X (%s), data at %08X, WAITCNT %04X, deadline %d%s\n", layout->thumb ? "Thumb" : "ARM", layout->codeBase,
	       regionNames[layout->codeBase >> 24], layout->dataBase, waitcnt, deadline, layout->smc ? ", self-modifying" : "");
	const uint8_t* bytes = (const uint8_t*) layout->code;
	for (size_t i = 0; i < layout->size; i += 4) {
		uint32_t word;
		memcpy(&word, &bytes[i], 4);
		printf("    %08X: %08X\n", (uint32_t) (layout->codeBase + i), word);
	}
}


struct Case {
	struct Layout layout;
	struct FuzzState init;
	uint16_t waitcnt;
	uint8_t data[0x2000];
};

struct Result {
	struct FuzzState m;
	struct FuzzState j;
	uint32_t elapsedM;
	uint32_t elapsedJ;
	uint8_t dataM[0x2000];
	uint8_t dataJ[0x2000];
};

static size_t codeBytes(const struct Layout* layout) {
	return layout->size;
}

static void execute(const struct Case* tc, int32_t deadline, struct Result* res) {
	const struct Layout* layout = &tc->layout;
	memcpy(mgbaEmu.memory(layout->dataBase), tc->data, sizeof(tc->data));
	memcpy(armgbaEmu.memory(layout->dataBase), tc->data, sizeof(tc->data));
	memcpy(mgbaEmu.memory(layout->codeBase), layout->code, codeBytes(layout));
	memcpy(armgbaEmu.memory(layout->codeBase), layout->code, codeBytes(layout));
	armgbaEmu.codeChanged();
	res->elapsedM = mgbaEmu.run(&tc->init, layout->codeBase, tc->waitcnt, deadline, &res->m);
	res->elapsedJ = armgbaEmu.run(&tc->init, layout->codeBase, tc->waitcnt, deadline, &res->j);
	memcpy(res->dataM, mgbaEmu.memory(layout->dataBase), sizeof(res->dataM));
	memcpy(res->dataJ, armgbaEmu.memory(layout->dataBase), sizeof(res->dataJ));
	memset(mgbaEmu.memory(layout->codeBase), 0, codeBytes(layout));
	memset(armgbaEmu.memory(layout->codeBase), 0, codeBytes(layout));
}

static bool compare(const struct Case* tc, const struct Result* res, bool report) {
	bool ok = true;
	for (int i = 0; i < 16; ++i) {
		if (res->m.r[i] != res->j.r[i]) {
			if (report) {
				printf("  r%d mGBA %08X armGBA %08X\n", i, res->m.r[i], res->j.r[i]);
			}
			ok = false;
		}
	}
	if (res->m.cpsr != res->j.cpsr) {
		if (report) {
			printf("  cpsr mGBA %08X armGBA %08X\n", res->m.cpsr, res->j.cpsr);
		}
		ok = false;
	}
	if ((res->m.shifterCarry & 1) != (res->j.shifterCarry & 1)) {
		if (report) {
			printf("  shifter carry mGBA %u armGBA %u\n", res->m.shifterCarry, res->j.shifterCarry);
		}
		ok = false;
	}
	if (res->m.spsr != res->j.spsr) {
		if (report) {
			printf("  spsr mGBA %08X armGBA %08X\n", res->m.spsr, res->j.spsr);
		}
		ok = false;
	}
	if (memcmp(res->m.bank, res->j.bank, sizeof(res->m.bank)) || memcmp(res->m.bankSpsr, res->j.bankSpsr, sizeof(res->m.bankSpsr))) {
		if (report) {
			printf("  banked registers differ\n");
		}
		ok = false;
	}
	if (res->elapsedM != res->elapsedJ) {
		if (report) {
			printf("  cycles mGBA %u armGBA %u\n", res->elapsedM, res->elapsedJ);
		}
		ok = false;
	}
	for (int i = 0; i < 0x2000; ++i) {
		if (res->dataM[i] != res->dataJ[i]) {
			if (report) {
				printf("  memory %08X mGBA %02X armGBA %02X\n", tc->layout.dataBase + i, res->dataM[i], res->dataJ[i]);
			}
			ok = false;
			break;
		}
	}
	return ok;
}

static void makeCase(struct Case* tc) {
	struct Layout* layout = &tc->layout;
	memset(tc, 0, sizeof(*tc));
	layout->thumb = pick(2);
	int codeRegion = pick(3);
	layout->smc = codeRegion < 2 && chance(30);
	layout->codeBase = codeRegion == 0 ? 0x03000000 + pick(64) * 64 : codeRegion == 1 ? 0x02000000 + pick(1024) * 64 : 0x08000000 + pick(4096) * 64;
	layout->dataBase = pick(2) ? 0x03005000 : 0x02030000;
	generate(layout);
	tc->waitcnt = rnd() & 0x5FFF;

	struct FuzzState* init = &tc->init;
	for (int i = 0; i < 16; ++i) {
		init->r[i] = chance(30) ? pick(64) : rnd() ^ (rnd() << 21);
	}
	for (int b = 0; b < 6; ++b) {
		for (int i = 0; i < 7; ++i) {
			init->bank[b][i] = rnd();
		}
		init->bankSpsr[b] = (rnd() & 0xF00000C0) | modes[pick(7)];
	}
	// Pointers into the middle of the data window, in every bank.
	uint32_t window = layout->dataBase + 0x800;
	// Both instruction sets' roles, since a program may switch.
	if (layout->thumb) {
		init->r[6] = window + pick(64) * 4;
		init->r[7] = window + pick(64) * 4;
		init->r[5] = pick(16) * 4;
	}
	for (int i = 8; i < 12; ++i) {
		init->r[i] = window + pick(64) * 4;
	}
	for (int i = 2; i < 6; ++i) {
		init->bank[1][i] = window + pick(64) * 4;
	}
	init->r[12] = pick(16) * 4;
	init->bank[1][6] = pick(16) * 4;
	// Stacks live in the second 4 KiB, far enough apart that pushes don't collide.
	for (int b = 0; b < 6; ++b) {
		init->bank[b][0] = layout->dataBase + 0x1000 + 0x280 * (b + 1);
	}
	uint32_t mode = layout->thumb ? 0x1F : modes[pick(7)];
	init->cpsr = (rnd() & 0xF0000000) | 0xC0 | mode | (layout->thumb ? 0x20 : 0);
	init->shifterCarry = pick(2);
	init->spsr = (rnd() & 0xF00000C0) | modes[pick(7)];
	if (mode == 0x10 || mode == 0x1F) {
		init->spsr = 0;
	}
	int bankIndex = mode == 0x11 ? 1 : mode == 0x12 ? 2 : mode == 0x13 ? 3 : mode == 0x17 ? 4 : mode == 0x1B ? 5 : 0;
	init->bankSpsr[bankIndex] = init->spsr;
	// Every mode gets its own stack.
	init->r[13] = init->bank[bankIndex][0];
	if (mode == 0x11) {
		for (int i = 0; i < 5; ++i) {
			init->bank[0][2 + i] = init->r[8 + i];
		}
	}
	for (size_t i = 0; i < sizeof(tc->data); ++i) {
		tc->data[i] = rnd();
	}
}

static bool runTest(int test) {
	static struct Case tc;
	static struct Result res;
	makeCase(&tc);
	int32_t deadline = 20 + pick(tc.layout.count * 12);
	execute(&tc, deadline, &res);
	if (compare(&tc, &res, false)) {
		return true;
	}
	// Finds the earliest deadline that shows a difference.
	int32_t first = deadline;
	for (int32_t d = 1; d < deadline; ++d) {
		execute(&tc, d, &res);
		if (!compare(&tc, &res, false)) {
			first = d;
			break;
		}
	}
	if (getenv("FUZZ_TRACE")) {
		for (int32_t d = 1; d <= first; ++d) {
			execute(&tc, d, &res);
			printf("  deadline %3d: pc %08X/%08X cycles %u/%u cpsr %08X/%08X |", d, res.m.r[15], res.j.r[15], res.elapsedM, res.elapsedJ, res.m.cpsr, res.j.cpsr);
			for (int i = 0; i < 15; ++i) {
				printf(" %08X", res.m.r[i]);
				if (res.m.r[i] != res.j.r[i]) {
					printf("/%08X", res.j.r[i]);
				}
			}
			printf("\n");
		}
	}
	execute(&tc, first > 1 ? first - 1 : 1, &res);
	uint32_t pcBefore = res.m.r[15];
	execute(&tc, first, &res);
	int width = tc.layout.thumb ? 2 : 4;
	printf("test %d: first differs at deadline %d, after the instruction at %08X (mGBA next %08X)\n", test, first, pcBefore - width, res.m.r[15] - width);
	compare(&tc, &res, true);
	const char* peek = getenv("FUZZ_PEEK");
	if (peek) {
		uint32_t offset = strtoul(peek, NULL, 16) - tc.layout.dataBase;
		printf("  at %s: mGBA", peek);
		for (int i = 0; i < 8; ++i) {
			printf(" %02X", res.dataM[offset + i]);
		}
		printf(" / armGBA");
		for (int i = 0; i < 8; ++i) {
			printf(" %02X", res.dataJ[offset + i]);
		}
		printf(" / initial");
		for (int i = 0; i < 8; ++i) {
			printf(" %02X", tc.data[offset + i]);
		}
		printf("\n");
	}
	printf("  initial cpsr %08X\n", tc.init.cpsr);
	for (int i = 0; i < 16; ++i) {
		printf("  r%d=%08X%s", i, tc.init.r[i], (i & 3) == 3 ? "\n" : "");
	}
	dump(&tc.layout, tc.waitcnt, deadline);
	return false;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s bios.bin [tests] [seed]\n", argv[0]);
		return 1;
	}
	int tests = argc > 2 ? atoi(argv[2]) : 10000;
	rngState = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x9E3779B97F4A7C15ull;

	rom = calloc(1, ROM_SIZE);
	if (!mgbaEmu.init(argv[1], rom, ROM_SIZE) || !armgbaEmu.init(argv[1], rom, ROM_SIZE)) {
		fprintf(stderr, "setup failed\n");
		return 1;
	}

	int failures = 0;
	const char* only = getenv("FUZZ_ONLY");
	for (int test = 0; test < tests; ++test) {
		if (only && atoi(only) != test) {
			// Keeps the generator in step.
			static struct Case skipped;
			makeCase(&skipped);
			pick(skipped.layout.count * 12);
			continue;
		}
		if (!runTest(test)) {
			if (++failures >= 5) {
				break;
			}
		}
	}
	printf("%d of %d tests matched\n", tests - failures, tests);
	return failures != 0;
}
