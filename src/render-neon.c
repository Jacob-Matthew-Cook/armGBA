/* Copyright (c) 2026 Magnominus
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// NEON versions of the renderer's hot paths, line for line with render-x86.c: eight pixels as two
// four-lane vectors, with the same compositing order, flags and 5-bit blending.

#include "render-private.h"

#include <arm_neon.h>
#include <string.h>

// NEON is part of AArch64, and of the ARMv7 CPUs this is built for (-mfpu=neon).
bool renderHasSimd(void) {
	return true;
}

#ifndef __aarch64__
// AArch64's intrinsics that 32-bit NEON lacks, from the ones it has. A table lookup gives 0 for an
// index past the table, as AArch64's does.
static inline uint8x8_t vqtbl1_u8(uint8x16_t table, uint8x8_t index) {
	uint8x8x2_t t = { { vget_low_u8(table), vget_high_u8(table) } };
	return vtbl2_u8(t, index);
}

// The second half's lookup leaves bytes whose index isn't in it as the first half's gave them.
static inline uint8x8_t vqtbl4_u8(uint8x16x4_t table, uint8x8_t index) {
	uint8x8x4_t first = { { vget_low_u8(table.val[0]), vget_high_u8(table.val[0]), vget_low_u8(table.val[1]), vget_high_u8(table.val[1]) } };
	uint8x8x4_t second = { { vget_low_u8(table.val[2]), vget_high_u8(table.val[2]), vget_low_u8(table.val[3]), vget_high_u8(table.val[3]) } };
	return vtbx4_u8(vtbl4_u8(first, index), second, vsub_u8(index, vdup_n_u8(32)));
}

static inline uint8x8_t vzip1_u8(uint8x8_t a, uint8x8_t b) {
	return vzip_u8(a, b).val[0];
}

static inline uint8x8_t vzip2_u8(uint8x8_t a, uint8x8_t b) {
	return vzip_u8(a, b).val[1];
}

static inline uint16x8_t vzip1q_u16(uint16x8_t a, uint16x8_t b) {
	return vzipq_u16(a, b).val[0];
}

static inline uint16x8_t vzip2q_u16(uint16x8_t a, uint16x8_t b) {
	return vzipq_u16(a, b).val[1];
}

static inline uint32_t vaddvq_u32(uint32x4_t a) {
	uint32x2_t pair = vadd_u32(vget_low_u32(a), vget_high_u32(a));
	return vget_lane_u32(vpadd_u32(pair, pair), 0);
}

static inline uint32_t vmaxvq_u32(uint32x4_t a) {
	uint32x2_t pair = vmax_u32(vget_low_u32(a), vget_high_u32(a));
	return vget_lane_u32(vpmax_u32(pair, pair), 0);
}
#endif

// Eight 32-bit lanes; masks are all ones or all zeros per lane.
typedef struct {
	uint32x4_t v[2];
} v8;

// The eight-lane operations the shared SIMD code is written in, here as pairs of NEON's four-lane
// ones.
static inline v8 set8(uint32_t x) {
	return (v8) { { vdupq_n_u32(x), vdupq_n_u32(x) } };
}

static inline v8 load8(const uint32_t* p) {
	return (v8) { { vld1q_u32(p), vld1q_u32(p + 4) } };
}

static inline void store8(uint32_t* p, v8 a) {
	vst1q_u32(p, a.v[0]);
	vst1q_u32(p + 4, a.v[1]);
}

#define OP8(name, op) \
	static inline v8 name(v8 a, v8 b) { \
		return (v8) { { op(a.v[0], b.v[0]), op(a.v[1], b.v[1]) } }; \
	}
OP8(and8, vandq_u32)
OP8(or8, vorrq_u32)
OP8(xor8, veorq_u32)
OP8(add8, vaddq_u32)
OP8(mul8, vmulq_u32)
OP8(eq8, vceqq_u32)

// ~a & b
static inline v8 andnot8(v8 a, v8 b) {
	return (v8) { { vbicq_u32(b.v[0], a.v[0]), vbicq_u32(b.v[1], a.v[1]) } };
}

// mask ? b : a
static inline v8 blend8(v8 a, v8 b, v8 mask) {
	return (v8) { { vbslq_u32(mask.v[0], b.v[0], a.v[0]), vbslq_u32(mask.v[1], b.v[1], a.v[1]) } };
}

// a > b, signed
static inline v8 gts8(v8 a, v8 b) {
	return (v8) { { vcgtq_s32(vreinterpretq_s32_u32(a.v[0]), vreinterpretq_s32_u32(b.v[0])),
		            vcgtq_s32(vreinterpretq_s32_u32(a.v[1]), vreinterpretq_s32_u32(b.v[1])) } };
}

// Whether no lane is set.
static inline bool none8(v8 a) {
	return !vmaxvq_u32(vorrq_u32(a.v[0], a.v[1]));
}

// Every bit of every lane inverted.
static inline v8 not8(v8 a) {
	return (v8) { { vmvnq_u32(a.v[0]), vmvnq_u32(a.v[1]) } };
}

#define SHL8(a, n) ((v8) { { vshlq_n_u32((a).v[0], n), vshlq_n_u32((a).v[1], n) } })
#define SHR8(a, n) ((v8) { { vshrq_n_u32((a).v[0], n), vshrq_n_u32((a).v[1], n) } })
#define SAR8(a, n) ((v8) { { vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32((a).v[0]), n)), \
	                         vreinterpretq_u32_s32(vshrq_n_s32(vreinterpretq_s32_u32((a).v[1]), n)) } })

// a >> b per lane, logical.
static inline v8 srlv8(v8 a, v8 b) {
	return (v8) { { vshlq_u32(a.v[0], vnegq_s32(vreinterpretq_s32_u32(b.v[0]))), vshlq_u32(a.v[1], vnegq_s32(vreinterpretq_s32_u32(b.v[1]))) } };
}

// The lanes of a mask as bits.
static inline int movemask8(v8 m) {
	static const uint32_t weights[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
	return vaddvq_u32(vandq_u32(m.v[0], vld1q_u32(weights))) + vaddvq_u32(vandq_u32(m.v[1], vld1q_u32(weights + 4)));
}

static const uint32_t laneNumbers[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

// The lanes' own numbers, 0 to 7.
static inline v8 laneIndex8(void) {
	return load8(laneNumbers);
}

// Eight 32-bit lanes narrowed to bytes.
static inline uint8x8_t narrow8(v8 lanes) {
	return vmovn_u16(vcombine_u16(vmovn_u32(lanes.v[0]), vmovn_u32(lanes.v[1])));
}

// 32-bit copies of the palettes for lookups, and each bank split into bytes for table lookups.
static void updatePalette32(struct GBA* gba) {
	struct Renderer* renderer = &gba->renderer;
	const uint16_t* normal = (const uint16_t*) gba->palette;
	for (uint32_t banks = renderer->palette32Stale; banks; banks &= banks - 1) {
		int bank = __builtin_ctz(banks);
		for (int i = bank * 16; i < bank * 16 + 16; ++i) {
			renderer->normal32[i] = normal[i];
			renderer->variant32[i] = renderer->variantPalette[i];
			renderer->bankBytes[0][bank][0][i & 15] = normal[i];
			renderer->bankBytes[0][bank][1][i & 15] = normal[i] >> 8;
			renderer->bankBytes[1][bank][0][i & 15] = renderer->variantPalette[i];
			renderer->bankBytes[1][bank][1][i & 15] = renderer->variantPalette[i] >> 8;
		}
	}
	renderer->palette32Stale = 0;
}

// Looks up eight table entries by index.
static inline v8 lookup8(const uint32_t* table, v8 index) {
	uint32_t idx[8];
	uint32_t out[8];
	store8(idx, index);
	for (int i = 0; i < 8; ++i) {
		out[i] = table[idx[i]];
	}
	return load8(out);
}

// Eight pixels as their low halves (the color) and high halves (the flags, a pixel's bits 24-31 in
// bits 8-15), one register each.
typedef struct {
	uint16x8_t lo;
	uint16x8_t hi;
} h8;

// Loads eight lanes with their halves apart, for palette words.
static inline h8 loadH8(const uint32_t* p) {
	uint16x8x2_t halves = vld2q_u16((const uint16_t*) p);
	return (h8) { halves.val[0], halves.val[1] };
}

// Zips rather than st2: GCC moves st2's register pair through the stack.
static inline void storeH8(uint32_t* p, h8 a) {
	vst1q_u16((uint16_t*) p, vzip1q_u16(a.lo, a.hi));
	vst1q_u16((uint16_t*) (p + 4), vzip2q_u16(a.lo, a.hi));
}

// Loads eight pixels of the row from x; its two planes are already the halves apart.
static inline h8 loadRowH8(const struct Renderer* renderer, int x) {
	return (h8) { vld1q_u16(&renderer->rowColor[x]), vld1q_u16(&renderer->rowFlags[x]) };
}

static inline void storeRowH8(struct Renderer* renderer, int x, h8 a) {
	vst1q_u16(&renderer->rowColor[x], a.lo);
	vst1q_u16(&renderer->rowFlags[x], a.hi);
}

// The same for lanes [from, to) only, at an end of the line; the others read as 0.
static inline h8 loadRowPartH8(const struct Renderer* renderer, int x, int from, int to) {
	uint16_t color[8] = { 0 };
	uint16_t flags[8] = { 0 };
	for (int i = from; i < to; ++i) {
		color[i] = renderer->rowColor[x + i];
		flags[i] = renderer->rowFlags[x + i];
	}
	return (h8) { vld1q_u16(color), vld1q_u16(flags) };
}

static inline void storeRowPartH8(struct Renderer* renderer, int x, int from, int to, h8 a) {
	uint16_t color[8];
	uint16_t flags[8];
	vst1q_u16(color, a.lo);
	vst1q_u16(flags, a.hi);
	for (int i = from; i < to; ++i) {
		renderer->rowColor[x + i] = color[i];
		renderer->rowFlags[x + i] = flags[i];
	}
}

// Whether all lanes are zero; narrowing to 64 bits is quicker than a horizontal maximum on small
// cores.
static inline bool noneH(uint16x8_t a) {
	return !vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(a, 4)), 0);
}

// mix5 on eight colors: each channel is min((a * weightA + b * weightB) / 16, 31).
static inline uint16x8_t mix5x8h(uint16_t weightA, uint16x8_t colorA, uint16_t weightB, uint16x8_t colorB) {
	const uint16x8_t five = vdupq_n_u16(0x1F);
	uint16x8_t r = vmlaq_n_u16(vmulq_n_u16(vandq_u16(colorA, five), weightA), vandq_u16(colorB, five), weightB);
	uint16x8_t g = vmlaq_n_u16(vmulq_n_u16(vandq_u16(vshrq_n_u16(colorA, 5), five), weightA), vandq_u16(vshrq_n_u16(colorB, 5), five), weightB);
	uint16x8_t b = vmlaq_n_u16(vmulq_n_u16(vandq_u16(vshrq_n_u16(colorA, 10), five), weightA), vandq_u16(vshrq_n_u16(colorB, 10), five), weightB);
	r = vminq_u16(vshrq_n_u16(r, 4), five);
	g = vminq_u16(vshrq_n_u16(g, 4), five);
	b = vminq_u16(vshrq_n_u16(b, 4), five);
	return vorrq_u16(vorrq_u16(r, vshlq_n_u16(g, 5)), vshlq_n_u16(b, 10));
}

// compositeEight on halves: color >= current compares the flags, then the colors.
static inline h8 compositeEightH(struct Renderer* renderer, h8 current, h8 color, uint16x8_t drawn, bool blend) {
	uint16x8_t notBelow = vorrq_u16(vcgtq_u16(color.hi, current.hi), vandq_u16(vceqq_u16(color.hi, current.hi), vcgeq_u16(color.lo, current.lo)));
	// current & (0x00FFFFFF | FLAG_REBLEND | FLAG_OBJWIN)
	uint16x8_t keptHi = vandq_u16(current.hi, vdupq_n_u16(0x05FF));
	h8 result;
	if (!blend) {
		result.lo = vbslq_u16(notBelow, current.lo, color.lo);
		result.hi = vbslq_u16(notBelow, keptHi, color.hi);
	} else {
		// A mix leaves just the color.
		uint16x8_t mixes = vandq_u16(vtstq_u16(current.hi, vdupq_n_u16(FLAG_TARGET_1 >> 16)), vtstq_u16(color.hi, vdupq_n_u16(FLAG_TARGET_2 >> 16)));
		uint16x8_t aboveLo = current.lo;
		uint16x8_t aboveHi = keptHi;
		if (!noneH(mixes)) {
			aboveLo = vbslq_u16(mixes, mix5x8h(renderer->blda, current.lo, renderer->bldb, color.lo), current.lo);
			aboveHi = vbicq_u16(keptHi, mixes);
		}
		result.lo = vbslq_u16(notBelow, aboveLo, color.lo);
		result.hi = vbslq_u16(notBelow, aboveHi, vbicq_u16(color.hi, vdupq_n_u16(FLAG_TARGET_2 >> 16)));
	}
	return (h8) { vbslq_u16(drawn, result.lo, current.lo), vbslq_u16(drawn, result.hi, current.hi) };
}

// compositeEightH under the object window: a color drawn above keeps the pixel's object window bit,
// as render.c's composite*Objwin do.
static inline h8 compositeEightObjwinH(struct Renderer* renderer, h8 current, h8 color, uint16x8_t drawn, bool blend) {
	uint16x8_t notBelow = vorrq_u16(vcgtq_u16(color.hi, current.hi), vandq_u16(vceqq_u16(color.hi, current.hi), vcgeq_u16(color.lo, current.lo)));
	uint16x8_t keptHi = vandq_u16(current.hi, vdupq_n_u16(0x05FF));
	uint16x8_t objwinHi = vandq_u16(current.hi, vdupq_n_u16(FLAG_OBJWIN >> 16));
	h8 result;
	if (!blend) {
		result.lo = vbslq_u16(notBelow, current.lo, color.lo);
		result.hi = vbslq_u16(notBelow, keptHi, vorrq_u16(color.hi, objwinHi));
	} else {
		uint16x8_t mixes = vandq_u16(vtstq_u16(current.hi, vdupq_n_u16(FLAG_TARGET_1 >> 16)), vtstq_u16(color.hi, vdupq_n_u16(FLAG_TARGET_2 >> 16)));
		uint16x8_t aboveLo = current.lo;
		uint16x8_t aboveHi = keptHi;
		if (!noneH(mixes)) {
			aboveLo = vbslq_u16(mixes, mix5x8h(renderer->blda, current.lo, renderer->bldb, color.lo), current.lo);
			aboveHi = vbicq_u16(keptHi, mixes);
		}
		result.lo = vbslq_u16(notBelow, aboveLo, color.lo);
		result.hi = vbslq_u16(notBelow, aboveHi, vorrq_u16(vbicq_u16(color.hi, vdupq_n_u16(FLAG_TARGET_2 >> 16)), objwinHi));
	}
	return (h8) { vbslq_u16(drawn, result.lo, current.lo), vbslq_u16(drawn, result.hi, current.hi) };
}

// Eight pixels' colors from one 16-color bank, as 16-bit lanes.
static inline uint16x8_t bankColorsH(const uint8_t (*bank)[16], uint8x8_t indices) {
	uint8x8_t lo = vqtbl1_u8(vld1q_u8(bank[0]), indices);
	uint8x8_t hi = vqtbl1_u8(vld1q_u8(bank[1]), indices);
	return vreinterpretq_u16_u8(vcombine_u8(vzip1_u8(lo, hi), vzip2_u8(lo, hi)));
}

// Composites palette indices onto the row: render.c's BACKGROUND_DRAW_PIXEL with
// COMPOSITE_*_NO_OBJWIN. index[i] holds the palette index of pixel start + i; 0 draws nothing.
static void compositeIndices(struct GBA* gba, const uint8_t* index, uint32_t flags, bool variant) {
	struct Renderer* renderer = &gba->renderer;
	const uint16_t* normal = (const uint16_t*) gba->palette;
	const uint16_t* palette = variant ? renderer->variantPalette : normal;
	const uint16x8_t flagsHi = vdupq_n_u16(flags >> 16);
	const uint16x8_t writable = vdupq_n_u16(0xFE000000 >> 16);
	const uint16x8_t reblendMask = vdupq_n_u16((FLAG_IS_BACKGROUND | FLAG_REBLEND) >> 16);
	const uint16x8_t reblend = vdupq_n_u16(FLAG_REBLEND >> 16);
	const uint16_t laneNumbers16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	const uint16x8_t lanes16 = vld1q_u16(laneNumbers16);
	bool blend = flags & FLAG_TARGET_2;
	int length = renderer->end - renderer->start;
	int start = renderer->start;
	for (int i = 0; i < length; i += 8) {
		int left = length - i;
		int to = left < 8 ? left : 8;
		h8 current = to < 8 ? loadRowPartH8(renderer, start + i, 0, to) : loadRowH8(renderer, start + i);
		uint16x8_t indices = vmovl_u8(vld1_u8(&index[i]));
		uint16x8_t drawn = vandq_u16(vtstq_u16(indices, indices), vcltq_u16(lanes16, vdupq_n_u16(to)));
		drawn = vandq_u16(drawn, vtstq_u16(current.hi, writable));
		if (noneH(drawn)) {
			continue;
		}
		uint16_t colors[8];
		for (int k = 0; k < 8; ++k) {
			colors[k] = palette[index[i + k]];
		}
		uint16x8_t color = vld1q_u16(colors);
		if (variant) {
			uint16_t normals[8];
			for (int k = 0; k < 8; ++k) {
				normals[k] = normal[index[i + k]];
			}
			uint16x8_t useNormal = vceqq_u16(vandq_u16(current.hi, reblendMask), reblend);
			color = vbslq_u16(useNormal, vld1q_u16(normals), color);
		}
		h8 result = compositeEightH(renderer, current, (h8) { color, flagsHi }, drawn, blend);
		if (to < 8) {
			storeRowPartH8(renderer, start + i, 0, to, result);
		} else {
			storeRowH8(renderer, start + i, result);
		}
	}
}

// Returns the map entry of tile column tx of a line, in a 256-pixel map or in two side by side.
static inline uint16_t textMapEntry(const uint8_t* map, int tx, unsigned wideMap) {
	tx &= 63;
	return load16(map, ((tx & 31) + ((tx & 32) << 5) * wideMap) << 1);
}

// What renderText16 draws a tile with. It is inlined twice, so the plain case carries no variant
// palette or object window tests.
struct Text16 {
	struct Renderer* renderer;
	uint16x8_t flagsHi;
	uint16x8_t objwinFlagsHi;
	uint16x8_t objwinOnly;
	bool blend;
	bool variant;
	bool objwinSlowPath;
	bool objwinVariant;
	bool forceEnable;
	// The palette bank last drawn with, as bytes for table lookups.
	int lastBank;
	uint8x16_t bankLo;
	uint8x16_t bankHi;
};

// Draws a tile's row of nibbles onto the eight pixels of the row from x; at an end of the run
// (edge), only [from, to).
static inline __attribute__((always_inline)) void text16Tile(struct Text16* s, int x, uint16_t mapData, uint32_t data,
                                                             int from, int to, bool edge) {
	struct Renderer* renderer = s->renderer;
	const uint16x8_t writable = vdupq_n_u16(0xFE000000 >> 16);
	uint8x8_t bytes = vcreate_u8(data);
	uint8x8_t nibbles = vzip1_u8(vand_u8(bytes, vdup_n_u8(0x0F)), vshr_n_u8(bytes, 4));
	if (TEXT_MAP_HFLIP(mapData)) {
		nibbles = vrev64_u8(nibbles);
	}
	h8 current;
	uint16x8_t drawn = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vtst_u8(nibbles, nibbles))));
	if (edge) {
		const uint16_t laneNumbers16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
		const uint16x8_t lanes16 = vld1q_u16(laneNumbers16);
		current = loadRowPartH8(renderer, x, from, to);
		drawn = vandq_u16(drawn, vandq_u16(vcgeq_u16(lanes16, vdupq_n_u16(from)), vcltq_u16(lanes16, vdupq_n_u16(to))));
	} else {
		current = loadRowH8(renderer, x);
	}
	drawn = vandq_u16(drawn, vtstq_u16(current.hi, writable));
	if (noneH(drawn)) {
		return;
	}
	int bank = TEXT_MAP_PALETTE(mapData);
	if (bank != s->lastBank) {
		s->bankLo = vld1q_u8(renderer->bankBytes[s->variant][bank][0]);
		s->bankHi = vld1q_u8(renderer->bankBytes[s->variant][bank][1]);
		s->lastBank = bank;
	}
	uint8x8_t lo = vqtbl1_u8(s->bankLo, nibbles);
	uint8x8_t hi = vqtbl1_u8(s->bankHi, nibbles);
	uint16x8_t color = vreinterpretq_u16_u8(vcombine_u8(vzip1_u8(lo, hi), vzip2_u8(lo, hi)));
	h8 result;
	if (!s->variant && !s->objwinSlowPath) {
		result = compositeEightH(renderer, current, (h8) { color, s->flagsHi }, drawn, s->blend);
	} else {
		uint16x8_t normal = color;
		if (s->variant) {
			// Pixels to blend again take the normal palette.
			normal = bankColorsH(renderer->bankBytes[0][bank], nibbles);
			uint16x8_t reblendBits = vandq_u16(current.hi, vdupq_n_u16((FLAG_IS_BACKGROUND | FLAG_REBLEND) >> 16));
			color = vbslq_u16(vceqq_u16(reblendBits, vdupq_n_u16(FLAG_REBLEND >> 16)), normal, color);
		}
		if (!s->objwinSlowPath) {
			result = compositeEightH(renderer, current, (h8) { color, s->flagsHi }, drawn, s->blend);
		} else {
			uint16x8_t underObjwin = vtstq_u16(current.hi, vdupq_n_u16(FLAG_OBJWIN >> 16));
			if (!s->forceEnable) {
				// (!(current & FLAG_OBJWIN)) == objwinOnly
				drawn = vandq_u16(drawn, veorq_u16(underObjwin, s->objwinOnly));
			}
			uint16x8_t objwinColor = s->objwinVariant ? bankColorsH(renderer->bankBytes[1][bank], nibbles) : normal;
			h8 merged = { vbslq_u16(underObjwin, objwinColor, color), vbslq_u16(underObjwin, s->objwinFlagsHi, s->flagsHi) };
			result = compositeEightObjwinH(renderer, current, merged, drawn, s->blend);
		}
	}
	if (edge) {
		storeRowPartH8(renderer, x, from, to, result);
	} else {
		storeRowH8(renderer, x, result);
	}
}

// A line's tiles in order, with each tile's loads started early, as an in-order core needs: the map
// entry two tiles ahead, the pixel row one. Past the run, they read map entries of the same line
// and rows inside VRAM.
struct TextTiles {
	const uint8_t* vram;
	const uint8_t* map;
	unsigned wideMap;
	uint32_t rowOffset;
	uint32_t flippedOffset;
	int tx;
	uint16_t map0;
	uint16_t map1;
	uint32_t base0;
	uint32_t data0;
};

// Returns where in VRAM the tile row a map entry names is, taking vertical flip into account.
static inline uint32_t textRowBase(const struct TextTiles* t, uint16_t mapData) {
	return (TEXT_MAP_VFLIP(mapData) ? t->flippedOffset : t->rowOffset) + (TEXT_MAP_TILE(mapData) << 5);
}

// The text background loop looks one tile ahead, so the next tile's data loads while this one is
// drawn.
static inline void textNextTile(struct TextTiles* t, uint16_t* mapData, uint32_t* charBase, uint32_t* data) {
	*mapData = t->map0;
	*charBase = t->base0;
	*data = t->data0;
	t->map0 = t->map1;
	t->base0 = textRowBase(t, t->map1);
	t->data0 = load32(t->vram, t->base0);
	t->map1 = textMapEntry(t->map, t->tx + 2, t->wideMap);
	++t->tx;
}

// 16-color text on 16-bit lanes, for renderTextSimd.
static inline __attribute__((always_inline)) void renderText16(struct GBA* gba, struct RenderBackground* background, unsigned yBase,
                                                               int inX, int lead, int count, int localY0, bool special) {
	struct Renderer* renderer = &gba->renderer;
	struct Text16 s = {
		.renderer = renderer,
		.flagsHi = vdupq_n_u16(background->flags >> 16),
		.objwinFlagsHi = vdupq_n_u16(background->objwinFlags >> 16),
		.objwinOnly = vdupq_n_u16(background->objwinOnly ? 0xFFFF : 0),
		.blend = background->flags & FLAG_TARGET_2,
		.variant = special && background->variant,
		// With the object window on, pixels under it take objwinFlags and the object window
		// palette.
		.objwinSlowPath = special && DISPCNT_OBJWIN_ENABLE(renderer->dispcnt),
		.forceEnable = background->objwinForceEnable,
		.lastBank = -1,
		.bankLo = vdupq_n_u8(0),
		.bankHi = vdupq_n_u8(0),
	};
	s.objwinVariant = s.objwinSlowPath && background->target1 && WIN_BLEND_ENABLE(renderer->objwin.packed) &&
	                  (renderer->blendEffect == BLEND_BRIGHTEN || renderer->blendEffect == BLEND_DARKEN);
	struct TextTiles tiles = {
		.vram = gba->vram,
		.map = &gba->vram[yBase << 1],
		.wideMap = background->size & 1,
		.rowOffset = background->charBase + (localY0 << 2),
		.flippedOffset = background->charBase + ((7 - localY0) << 2),
		.tx = (inX - lead) >> 3,
	};
	tiles.map0 = textMapEntry(tiles.map, tiles.tx, tiles.wideMap);
	tiles.map1 = textMapEntry(tiles.map, tiles.tx + 1, tiles.wideMap);
	tiles.base0 = textRowBase(&tiles, tiles.map0);
	tiles.data0 = load32(tiles.vram, tiles.base0);
	int x = renderer->start - lead;
	uint16_t mapData;
	uint32_t charBase;
	uint32_t data;
	// mGBA skips whole tiles past 64 KiB, but draws the partial ones at the ends of the run.
	if (lead) {
		textNextTile(&tiles, &mapData, &charBase, &data);
		if (data) {
			text16Tile(&s, x, mapData, data, lead, 8, true);
		}
		x += 8;
	}
	for (int t = lead ? 1 : 0; t < count >> 3; ++t, x += 8) {
		textNextTile(&tiles, &mapData, &charBase, &data);
		if (data && charBase < 0x10000) {
			text16Tile(&s, x, mapData, data, 0, 8, false);
		}
	}
	if (count & 7) {
		textNextTile(&tiles, &mapData, &charBase, &data);
		if (data) {
			text16Tile(&s, x, mapData, data, 0, count & 7, true);
		}
	}
}

// Draws a text background without mosaic or the object window, as render.c's drawBackgroundMode0
// does.
bool renderTextSimd(struct GBA* gba, struct RenderBackground* background, int y) {
	struct Renderer* renderer = &gba->renderer;
	int length = renderer->end - renderer->start;
	if (length <= 0) {
		return true;
	}
	if (renderer->palette32Stale) {
		updatePalette32(gba);
	}
	int inX = (renderer->start + background->x) & 0x1FF;
	int inY = y + background->y;
	unsigned yBase = inY & 0xF8;
	if (background->size == 2) {
		yBase += inY & 0x100;
	} else if (background->size == 3) {
		yBase += (inY & 0x100) << 1;
	}
	yBase = (background->screenBase >> 1) + (yBase << 2);
	const uint8_t* vram = gba->vram;

	// Indices from the start of the first tile; its lead pixels lie before the window.
	int lead = inX & 7;
	int count = lead + length;
	// When the window ends inside the first, partial tile, mGBA lines its pixels up differently.
	if (lead && count < 8) {
		return false;
	}
	int tiles = (count + 7) >> 3;
	int localY0 = inY & 0x7;
	if (!background->multipalette) {
		if (!background->variant && !DISPCNT_OBJWIN_ENABLE(renderer->dispcnt)) {
			renderText16(gba, background, yBase, inX, lead, count, localY0, false);
		} else {
			renderText16(gba, background, yBase, inX, lead, count, localY0, true);
		}
		return true;
	}
	// 256 colors: the tiles' palette indices first, with their loads ahead of use as above. Tiles
	// past 64 KiB draw nothing, and their row loads are moved inside VRAM.
	uint8_t index[SCREEN_WIDTH + 16] __attribute__((aligned(16)));
	const uint8_t* map = &vram[yBase << 1];
	int tx0 = (inX - lead) >> 3;
	unsigned wideMap = background->size & 1;
	uint32_t rowOffset[2] = { background->charBase + (localY0 << 3), background->charBase + ((7 - localY0) << 3) };
	uint16_t map0 = textMapEntry(map, tx0, wideMap);
	uint16_t map1 = textMapEntry(map, tx0 + 1, wideMap);
	uint32_t base0 = rowOffset[TEXT_MAP_VFLIP(map0) >> 11] + (TEXT_MAP_TILE(map0) << 6);
	uint8x8_t row0 = vld1_u8(&vram[base0 < 0x10000 ? base0 : 0]);
	for (int t = 0; t < tiles; ++t) {
		uint16_t mapData = map0;
		uint32_t charBase = base0;
		uint8x8_t bytes = row0;
		map0 = map1;
		base0 = rowOffset[TEXT_MAP_VFLIP(map1) >> 11] + (TEXT_MAP_TILE(map1) << 6);
		row0 = vld1_u8(&vram[base0 < 0x10000 ? base0 : 0]);
		map1 = textMapEntry(map, tx0 + t + 2, wideMap);
		if (charBase >= 0x10000) {
			bytes = vdup_n_u8(0);
		} else if (TEXT_MAP_HFLIP(mapData)) {
			bytes = vrev64_u8(bytes);
		}
		vst1_u8(&index[t * 8], bytes);
	}
	compositeIndices(gba, &index[lead], background->flags, background->variant);
	return true;
}

// Merges the sprites of one priority onto the row: render.c's postprocessSprite without the object
// window. Sprites wrote only columns [from, to) on this line.
void renderSpritesSimd(struct GBA* gba, unsigned priority, int from, int to) {
	struct Renderer* renderer = &gba->renderer;
	const uint16x8_t unwritten = vdupq_n_u16(FLAG_UNWRITTEN >> 16);
	const uint16x8_t priorityMask = vdupq_n_u16(FLAG_PRIORITY >> 16);
	const uint16x8_t wanted = vdupq_n_u16((priority << OFFSET_PRIORITY) >> 16);
	const uint16x8_t objwin = vdupq_n_u16(FLAG_OBJWIN >> 16);
	const uint16x8_t flags = vdupq_n_u16((FLAG_TARGET_2 * renderer->target2Obj) >> 16);
	const uint16_t laneNumbers16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	const uint16x8_t lanes16 = vld1q_u16(laneNumbers16);
	int start = renderer->start > from ? renderer->start : from;
	int length = (renderer->end < to ? renderer->end : to) - start;
	for (int i = 0; i < length; i += 8) {
		int x = start + i;
		int lanes = length - i < 8 ? length - i : 8;
		uint32_t partSprite[8] = { 0 };
		h8 sprite;
		if (lanes < 8) {
			for (int k = 0; k < lanes; ++k) {
				partSprite[k] = renderer->spriteLayer[x + k];
			}
			sprite = loadH8(partSprite);
		} else {
			sprite = loadH8(&renderer->spriteLayer[x]);
		}
		sprite.hi = vbicq_u16(sprite.hi, objwin);
		uint16x8_t drawn = vmvnq_u16(vceqq_u16(vandq_u16(sprite.hi, unwritten), unwritten));
		drawn = vandq_u16(drawn, vceqq_u16(vandq_u16(sprite.hi, priorityMask), wanted));
		drawn = vandq_u16(drawn, vcltq_u16(lanes16, vdupq_n_u16(lanes)));
		if (noneH(drawn)) {
			continue;
		}
		h8 current = lanes < 8 ? loadRowPartH8(renderer, x, 0, lanes) : loadRowH8(renderer, x);
		h8 color = { sprite.lo, vorrq_u16(sprite.hi, flags) };
		// compositeBlendNoObjwin: blends whenever target 1 meets target 2.
		h8 result = compositeEightH(renderer, current, color, drawn, true);
		if (lanes < 8) {
			storeRowPartH8(renderer, x, 0, lanes, result);
		} else {
			storeRowH8(renderer, x, result);
		}
	}
}

// The first part of render.c's postprocessBuffer: blends target 1 pixels with the backdrop.
void renderBlendBackdropSimd(struct GBA* gba, int x, int end, uint32_t backdrop) {
	struct Renderer* renderer = &gba->renderer;
	const uint16x8_t target1 = vdupq_n_u16(FLAG_TARGET_1 >> 16);
	const uint16x8_t backdropLo = vdupq_n_u16(backdrop);
	for (; x < end; x += 8) {
		int lanes = end - x < 8 ? end - x : 8;
		h8 color = lanes < 8 ? loadRowPartH8(renderer, x, 0, lanes) : loadRowH8(renderer, x);
		uint16x8_t hit = vtstq_u16(color.hi, target1);
		if (noneH(hit)) {
			continue;
		}
		// A mix leaves just the color.
		h8 result = { vbslq_u16(hit, mix5x8h(renderer->bldb, backdropLo, renderer->blda, color.lo), color.lo), vbicq_u16(color.hi, hit) };
		if (lanes < 8) {
			storeRowPartH8(renderer, x, 0, lanes, result);
		} else {
			storeRowH8(renderer, x, result);
		}
	}
}

// Merges eight sprite pixels onto the sprite layer on 16-bit lanes (sprite flags are all in the
// high halves): the first count of them, where they come before what's there.
static inline void mergeSpritePixels(uint32_t* layer, int count, uint16x8_t transparent, uint16x8_t color, uint32_t flags) {
	const uint16_t laneNumbers16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	const uint16x8_t flagsHi = vdupq_n_u16(flags >> 16);
	const uint16x8_t otherHi = vdupq_n_u16((FLAG_ORDER_MASK | FLAG_REBLEND | FLAG_TARGET_1) >> 16);
	uint32_t part[8] = { 0 };
	h8 current;
	if (count == 8) {
		current = loadH8(layer);
	} else {
		for (int k = 0; k < count; ++k) {
			part[k] = layer[k];
		}
		current = loadH8(part);
	}
	uint16x8_t lanes = vcltq_u16(vld1q_u16(laneNumbers16), vdupq_n_u16(count));
	// (current & FLAG_ORDER_MASK) > flags
	uint16x8_t before = vandq_u16(vcgtq_u16(vandq_u16(current.hi, vdupq_n_u16(FLAG_ORDER_MASK >> 16)), flagsHi), lanes);
	uint16x8_t clearHi = vorrq_u16(vbicq_u16(current.hi, otherHi), vandq_u16(flagsHi, otherHi));
	uint16x8_t written = vmvnq_u16(vandq_u16(vceqq_u16(current.lo, vdupq_n_u16(0)), vceqq_u16(current.hi, vdupq_n_u16(FLAG_UNWRITTEN >> 16))));
	uint16x8_t store = vandq_u16(before, vorrq_u16(vmvnq_u16(transparent), written));
	h8 result = { vbslq_u16(transparent, current.lo, color), vbslq_u16(transparent, clearHi, flagsHi) };
	result = (h8) { vbslq_u16(store, result.lo, current.lo), vbslq_u16(store, result.hi, current.hi) };
	if (count == 8) {
		storeH8(layer, result);
	} else {
		storeH8(part, result);
		for (int k = 0; k < count; ++k) {
			layer[k] = part[k];
		}
	}
}

// Returns how many pixels of a run with c == 0 lie in the sprite before the first that doesn't.
// Pixel k reads column (xAccum + (k + 1) * a) >> 8, which only moves one way.
static int spriteRowLength(int xAccum, int a, int width, int left) {
	int first = xAccum + a;
	int limit = width << 8;
	if (first < 0 || first >= limit) {
		return 0;
	}
	int n;
	if (a > 0) {
		n = (limit - xAccum + a - 1) / a - 1;
	} else if (a < 0) {
		n = xAccum / -a;
	} else {
		n = left;
	}
	return n < left ? n : left;
}

// Draws a run of a sprite's pixels eight at a time (render.c's SPRITE_NORMAL_LOOP and
// SPRITE_TRANSFORMED_LOOP with SPRITE_DRAW_PIXEL_*_NORMAL): tile data widened to a pixel per lane,
// looked up in the palette, and merged into the sprite layer under the same rules as the scalar
// loop.
void renderSpriteRunSimd(struct GBA* gba, const struct SpriteRun* run) {
	struct Renderer* renderer = &gba->renderer;
	if (renderer->palette32Stale) {
		updatePalette32(gba);
	}
	const uint8_t* vramBase = &gba->vram[BASE_TILE];
	const v8 lanesUp = laneIndex8();
	const v8 zero = set8(0);
	const v8 charBase = set8(run->charBase);
	const v8 maskLo = set8(run->maskLo);
	const v8 maskHi = set8(run->maskHi);
	const v8 stride = set8(run->stride);
	const v8 halfword = set8(0x7FFE);
	const v8 paletteBase = set8(run->paletteBase);
	const v8 rowBytes = set8(run->wide ? 8 : 4);
	const v8 seven = set8(7);
	const uint8_t (*bank)[16] = renderer->bankBytes[run->palette == renderer->variant32][run->paletteBase >> 4];
	int outX = run->outX;
	int inX = run->inX;
	int xAccum = run->xAccum;
	int yAccum = run->yAccum;
	// An affine sprite whose rows don't turn (c == 0) reads a single row of itself, so its texels
	// and their colors go into tables once.
	int rowY = yAccum >> 8;
	bool rowTable = run->transformed && !run->c && run->width <= 64 && !(rowY & ~(run->height - 1));
	if (rowTable) {
		uint8_t texels[64] = { 0 };
		uint8_t low[64] = { 0 };
		uint8_t high[64] = { 0 };
		spriteRow(gba, run, rowY, texels, low, high);
		uint8x16x4_t rowTexels = vld1q_u8_x4(texels);
		uint8x16x4_t rowLow = vld1q_u8_x4(low);
		uint8x16x4_t rowHigh = vld1q_u8_x4(high);
		int length = spriteRowLength(xAccum, run->a, run->width, run->condition - outX);
		v8 xs = add8(set8(xAccum), mul8(add8(lanesUp, set8(1)), set8(run->a)));
		const v8 step = set8(8 * run->a);
		const v8 column = set8(63);
		for (int i = 0; i < length; i += 8) {
			uint8x8_t index = narrow8(and8(SAR8(xs, 8), column));
			xs = add8(xs, step);
			uint16x8_t transparent = vreinterpretq_u16_s16(vmovl_s8(vreinterpret_s8_u8(vceq_u8(vqtbl4_u8(rowTexels, index), vdup_n_u8(0)))));
			uint8x8_t lo = vqtbl4_u8(rowLow, index);
			uint8x8_t hi = vqtbl4_u8(rowHigh, index);
			uint16x8_t color = vreinterpretq_u16_u8(vcombine_u8(vzip1_u8(lo, hi), vzip2_u8(lo, hi)));
			mergeSpritePixels(&renderer->spriteLayer[outX + i], length - i < 8 ? length - i : 8, transparent, color, run->flags);
		}
		return;
	}
	while (outX < run->condition) {
		int left = run->condition - outX;
		int count = left < 8 ? left : 8;
		uint16x8_t texel;
		uint16x8_t color;
		bool stop = false;
		if (!run->transformed && !run->wide && left >= 8 && (run->xOffset == 1 ? !(inX & 7) : run->xOffset == -1 && (inX & 7) == 7)) {
			// One whole row of a 16-color tile: its two halfwords, as SPRITE_DRAW_PIXEL_16_NORMAL
			// reads them.
			unsigned xBase = (inX & ~7) * 4;
			unsigned yBase = (run->inY & ~7) * run->stride + (run->inY & 7) * 4 + run->maskHi;
			uint32_t data = load16(vramBase, (yBase + ((xBase + run->charBase) & run->maskLo)) & 0x7FFE);
			data |= load16(vramBase, (yBase + ((xBase + 2 + run->charBase) & run->maskLo)) & 0x7FFE) << 16;
			uint8x8_t bytes = vcreate_u8(data);
			uint8x8_t nibbles = vzip1_u8(vand_u8(bytes, vdup_n_u8(0x0F)), vshr_n_u8(bytes, 4));
			if (run->xOffset < 0) {
				nibbles = vrev64_u8(nibbles);
			}
			texel = vmovl_u8(nibbles);
			color = bankColorsH(bank, nibbles);
		} else {
			v8 localX;
			v8 localY;
			if (!run->transformed) {
				localX = add8(set8(inX), mul8(lanesUp, set8(run->xOffset)));
				localY = set8(run->inY);
			} else {
				v8 step = add8(lanesUp, set8(1));
				v8 xs = add8(set8(xAccum), mul8(step, set8(run->a)));
				v8 ys = add8(set8(yAccum), mul8(step, set8(run->c)));
				localX = SAR8(xs, 8);
				localY = SAR8(ys, 8);
				// The loop stops at the first pixel outside the sprite.
				v8 outside = or8(and8(localX, set8(~(run->width - 1))), and8(localY, set8(~(run->height - 1))));
				outside = and8(not8(eq8(outside, zero)), gts8(set8(left), lanesUp));
				int bits = movemask8(outside);
				if (bits) {
					int firstOut = __builtin_ctz(bits);
					count = firstOut < count ? firstOut : count;
					stop = true;
				}
			}
			v8 yBase = add8(mul8(andnot8(seven, localY), stride), add8(mul8(and8(localY, seven), rowBytes), maskHi));
			v8 xBase;
			if (!run->wide) {
				xBase = add8(SHL8(andnot8(seven, localX), 2), and8(SHR8(localX, 1), set8(2)));
			} else {
				xBase = add8(SHL8(andnot8(seven, localX), 3), and8(localX, set8(6)));
			}
			v8 address = and8(add8(yBase, and8(add8(xBase, charBase), maskLo)), halfword);
			uint32_t addresses[8];
			uint32_t texels[8] = { 0 };
			store8(addresses, address);
			for (int i = 0; i < count; ++i) {
				memcpy(&texels[i], &vramBase[addresses[i]], 4);
			}
			v8 texels32 = load8(texels);
			if (!run->wide) {
				texels32 = and8(srlv8(texels32, SHL8(and8(localX, set8(3)), 2)), set8(0xF));
			} else {
				texels32 = and8(srlv8(texels32, SHL8(and8(localX, set8(1)), 3)), set8(0xFF));
			}
			v8 colors32 = lookup8(run->palette, add8(paletteBase, texels32));
			texel = vcombine_u16(vmovn_u32(texels32.v[0]), vmovn_u32(texels32.v[1]));
			color = vcombine_u16(vmovn_u32(colors32.v[0]), vmovn_u32(colors32.v[1]));
		}
		mergeSpritePixels(&renderer->spriteLayer[outX], count, vceqq_u16(texel, vdupq_n_u16(0)), color, run->flags);
		if (stop) {
			return;
		}
		outX += count;
		inX += count * run->xOffset;
		xAccum += count * run->a;
		yAccum += count * run->c;
	}
}

// render.c's drawBackgroundMode4 without the object window. x and y are the coordinates of the
// pixel before the window's first, as BACKGROUND_BITMAP_INIT leaves them; with mosaic, picked holds
// the color index each pixel took instead, or 0 for none.
void renderMode4Simd(struct GBA* gba, struct RenderBackground* background, int32_t x, int32_t y, const uint8_t* picked) {
	struct Renderer* renderer = &gba->renderer;
	const uint16_t* palette = background->variant ? renderer->variantPalette : (const uint16_t*) gba->palette;
	uint32_t offset = DISPCNT_FRAME_SELECT(renderer->dispcnt) ? 0xA000 : 0;
	const uint8_t* frame = &gba->vram[offset];
	const v8 width = set8(SCREEN_WIDTH);
	const v8 height = set8(SCREEN_HEIGHT);
	const v8 dx = set8(background->dx);
	const v8 dy = set8(background->dy);
	const v8 step = add8(laneIndex8(), set8(1));
	const uint16x8_t flagsHi = vdupq_n_u16(background->flags >> 16);
	const uint16x8_t writable = vdupq_n_u16(0xFE000000 >> 16);
	const uint16_t laneNumbers16[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	const uint16x8_t lanes16 = vld1q_u16(laneNumbers16);
	// Without rotation, a row reads one line; shrunk at most by half, eight pixels lie within 16
	// bytes of it.
	bool flat = background->dy == 0 && background->dx > 0 && background->dx <= 0x200;
	int length = renderer->end - renderer->start;
	for (int i = 0; i < length; i += 8) {
		int to = length - i < 8 ? length - i : 8;
		// The loop adds dx and dy before each pixel.
		v8 xs = add8(set8(x + i * background->dx), mul8(step, dx));
		v8 column = SAR8(xs, 8);
		uint8_t indices[8] = { 0 };
		int line0 = y >> 8;
		int first = (x + (i + 1) * background->dx) >> 8;
		int last = (x + (i + 8) * background->dx) >> 8;
		if (picked) {
			memcpy(indices, &picked[i], to);
		} else if (flat && to == 8 && (unsigned) line0 < SCREEN_HEIGHT && first >= 0 && last < SCREEN_WIDTH) {
			uint8x8_t offsets = narrow8(add8(column, set8(-first)));
			vst1_u8(indices, vqtbl1_u8(vld1q_u8(&frame[line0 * SCREEN_WIDTH + first]), offsets));
		} else {
			v8 ys = add8(set8(y + i * background->dy), mul8(step, dy));
			v8 line = SAR8(ys, 8);
			// x and y in range: (unsigned) column < 240 and line < 160.
			v8 inside = and8(gts8(width, column), gts8(height, line));
			inside = and8(inside, gts8(or8(column, line), set8(-1)));
			inside = and8(inside, gts8(set8(to), laneIndex8()));
			if (none8(inside)) {
				continue;
			}
			v8 address = add8(column, mul8(line, width));
			uint32_t addresses[8];
			uint32_t insideLanes[8];
			store8(addresses, address);
			store8(insideLanes, inside);
			for (int k = 0; k < 8; ++k) {
				if (insideLanes[k]) {
					indices[k] = frame[addresses[k]];
				}
			}
		}
		uint16x8_t index = vmovl_u8(vld1_u8(indices));
		int at = renderer->start + i;
		h8 current = to < 8 ? loadRowPartH8(renderer, at, 0, to) : loadRowH8(renderer, at);
		uint16x8_t drawn = vandq_u16(vtstq_u16(index, index), vtstq_u16(current.hi, writable));
		drawn = vandq_u16(drawn, vcltq_u16(lanes16, vdupq_n_u16(to)));
		if (noneH(drawn)) {
			continue;
		}
		uint16_t colors[8];
		for (int k = 0; k < 8; ++k) {
			colors[k] = palette[indices[k]];
		}
		h8 color = { vld1q_u16(colors), flagsHi };
		h8 result = compositeEightH(renderer, current, color, drawn, true);
		if (to < 8) {
			storeRowPartH8(renderer, at, 0, to, result);
		} else {
			storeRowH8(renderer, at, result);
		}
	}
}

// Returns the pixels [*from, *to) of n whose coordinate a + i * d lies in [0, size).
static void insideRun(int32_t a, int32_t d, int32_t size, int n, int* from, int* to) {
	if (d < 0) {
		// The same run, measured from the far edge.
		a = size - 1 - a;
		d = -d;
	}
	int64_t first = 0;
	int64_t last = n;
	if (d == 0) {
		last = a >= 0 && a < size ? n : 0;
	} else {
		if (a < 0) {
			first = (-(int64_t) a + d - 1) / d;
		}
		last = a < size ? ((int64_t) size - 1 - a) / d + 1 : 0;
	}
	*from = first < *from ? *from : first;
	*to = last > *to ? *to : last;
}

// The palette indices of an affine background's pixels [from, to) whose coordinates are inside the
// map or, with wrap, wrapped into it; x and y are those of pixel -1.
static inline __attribute__((always_inline)) void affineIndices(uint8_t* index, int from, int to, const uint8_t* screenBase, const uint8_t* charBase,
                                                                int32_t x, int32_t y, int32_t dx, int32_t dy, int size, bool wrap) {
	uint32_t mask = (0x8000 << size) - 1;
	x += (from + 1) * dx;
	y += (from + 1) * dy;
#pragma GCC unroll 4
	for (int i = from; i < to; ++i, x += dx, y += dy) {
		uint32_t localX = wrap ? x & mask : (uint32_t) x;
		uint32_t localY = wrap ? y & mask : (uint32_t) y;
		uint8_t mapData = screenBase[(localX >> 11) + ((localY >> 11) << (4 + size))];
		index[i] = charBase[(mapData << 6) + (((localY >> 8) & 7) << 3) + ((localX >> 8) & 7)];
	}
}

// render.c's drawBackgroundMode2 without mosaic or the object window: the palette indices first,
// then composited as 256-color text is. x and y are the coordinates of the pixel before the
// window's first, as BACKGROUND_BITMAP_INIT leaves them.
void renderAffineSimd(struct GBA* gba, struct RenderBackground* background, int32_t x, int32_t y) {
	struct Renderer* renderer = &gba->renderer;
	int length = renderer->end - renderer->start;
	if (length <= 0) {
		return;
	}
	uint8_t index[SCREEN_WIDTH + 16] __attribute__((aligned(16)));
	const uint8_t* screenBase = &gba->vram[background->screenBase];
	const uint8_t* charBase = &gba->vram[background->charBase];
	int32_t dx = background->dx;
	int32_t dy = background->dy;
	bool wrap = background->overflow;
	// Outside the map draws nothing unless it wraps.
	int from = 0;
	int to = length;
	if (!wrap) {
		insideRun(x + dx, dx, 0x8000 << background->size, length, &from, &to);
		insideRun(y + dy, dy, 0x8000 << background->size, length, &from, &to);
		if (from >= to) {
			from = to = 0;
		}
		memset(index, 0, length);
	}
	switch (background->size) {
	case 0:
		affineIndices(index, from, to, screenBase, charBase, x, y, dx, dy, 0, wrap);
		break;
	case 1:
		affineIndices(index, from, to, screenBase, charBase, x, y, dx, dy, 1, wrap);
		break;
	case 2:
		affineIndices(index, from, to, screenBase, charBase, x, y, dx, dy, 2, wrap);
		break;
	case 3:
		affineIndices(index, from, to, screenBase, charBase, x, y, dx, dy, 3, wrap);
		break;
	}
	compositeIndices(gba, index, background->flags, background->variant);
}
